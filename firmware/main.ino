#include <WiFi.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <MPU6050.h>
#include <Arduino.h>

#define HEARTBEAT_LED_PIN 26
#define BUZZER_PIN 25
#define SMOKE_PIN  14
#define LED_GROUP_PIN 27     // קבוצה 1 ליציאה (עשן/רעידה)
#define TERROR_LED_PIN 23    // קבוצה 2 לממ"ד (פח"ע)

// ================== groupLeds for EarthQuake and smoke ==================
const unsigned long GROUP_BLINK_MS = 2500;
bool groupLedState = false;
unsigned long groupLedLastToggle = 0;

// ================== LED_HEARTBEAT ==================
const unsigned long HEARTBEAT_ON_MS  = 150;
const unsigned long HEARTBEAT_OFF_MS = 850;
bool hbLedState = false;
unsigned long hbLastToggle = 0;

// ================== polling for terror ==================
bool terrorActive = false;
unsigned long lastPollMs = 0;
const unsigned long POLL_INTERVAL_MS = 1000;  // כל שניה

// ================== groupLeds for terror ==================
bool terrorLedState = false;
unsigned long terrorLedLastToggle = 0;
const unsigned long TERROR_LED_ON_MS  = 2500;
const unsigned long TERROR_LED_OFF_MS = 2000;

// ================== מיקום המערכת (לשנות ידנית) ==================
const float DEVICE_LAT = 32.******;
const float DEVICE_LON = 35.******;

// ================== שרת ==================
const char* SERVER_URL    = "https://esp32-alert-server.onrender.com/alert";
const char* SHARED_SECRET = "******";
const char* DEVICE_ID     = "******";

// endpoint שמחזיר את האירוע הנוכחי (מהתמונה שלך)
const char* CURRENT_EVENT_URL = "https://esp32-alert-server.onrender.com/current_event";

// ================== WiFi ==================
const char* ssid     = "****";
const char* password = "****";

// ================== LCD ==================
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ================== חיישן עשן ==================
const unsigned long SMOKE_MIN_INTERVAL_MS = 10000;

// ✅ חדש: להחזיק התראה לפחות 2 דקות ולחזור לשגרה רק אחרי 2 דקות "נקי"
const unsigned long SMOKE_HOLD_MS = 120000; // 2 דקות
unsigned long lastSmokeDetectedMs = 0;

bool smokeActive = false;
unsigned long lastSmokeEventTime = 0;

// ================== רעידת אדמה (MPU6050) ==================
const float QUAKE_LIGHT_THRESHOLD  = 0.05f;
const float QUAKE_STRONG_THRESHOLD = 0.20f;

const unsigned long QUAKE_END_QUIET_MS = 120000;   // 2 דקות שקט
const unsigned long QUAKE_COOLDOWN_MS  = 180000;   // 3 דקות בין שליחות לשרת

int quakeState = 0;  // 0=normal, 1=light, 2=strong
unsigned long lastQuakeMovementTime = 0;

int  latchedQuakeLevel = 0;
bool lightSentThisEvent = false;
bool strongSentThisEvent = false;
bool normalSentAfterQuake = true;
unsigned long lastAlertSentMs = 0;

MPU6050 mpu;

// ================== BUZZER (non-blocking) ==================
bool buzzerEnabled = false;
bool buzzerOn = false;
unsigned long buzzerLastToggle = 0;

const unsigned long BUZZ_ON_MS  = 250;
const unsigned long BUZZ_OFF_MS = 250;
const int BUZZ_DUTY = 200;   // 0..255

// ================== helpers ==================
void buzzerStart() { buzzerEnabled = true; }

void buzzerStop() {
  buzzerEnabled = false;
  buzzerOn = false;
  ledcWrite(BUZZER_PIN, 0);
}

void buzzerUpdate() {
  if (!buzzerEnabled) return;

  unsigned long now = millis();

  if (buzzerOn) {
    if (now - buzzerLastToggle >= BUZZ_ON_MS) {
      buzzerOn = false;
      buzzerLastToggle = now;
      ledcWrite(BUZZER_PIN, 0);
    }
  } else {
    if (now - buzzerLastToggle >= BUZZ_OFF_MS) {
      buzzerOn = true;
      buzzerLastToggle = now;
      ledcWrite(BUZZER_PIN, BUZZ_DUTY);
    }
  }
}

void heartbeatUpdate() {
  unsigned long now = millis();
  unsigned long interval = hbLedState ? HEARTBEAT_ON_MS : HEARTBEAT_OFF_MS;

  if (now - hbLastToggle >= interval) {
    hbLastToggle = now;
    hbLedState = !hbLedState;
    digitalWrite(HEARTBEAT_LED_PIN, hbLedState ? HIGH : LOW);
  }
}

// ============================================================
// Terror LED update (קבוצה 2)
// ============================================================
void terrorLedUpdate() {
  if (!terrorActive) {
    digitalWrite(TERROR_LED_PIN, LOW);
    terrorLedState = false;
    return;
  }

  unsigned long now = millis();
  unsigned long interval = terrorLedState ? TERROR_LED_ON_MS : TERROR_LED_OFF_MS;

  if (now - terrorLedLastToggle >= interval) {
    terrorLedLastToggle = now;
    terrorLedState = !terrorLedState;
    digitalWrite(TERROR_LED_PIN, terrorLedState ? HIGH : LOW);
  }
}

// ============================================================
// קבוצה 1 (עשן/רעידה)
// ============================================================
void groupLedsUpdate() {
  bool eventActive = smokeActive || (quakeState > 0);
  unsigned long now = millis();

  if (!eventActive) {
    digitalWrite(LED_GROUP_PIN, LOW);
    groupLedState = false;
    groupLedLastToggle = now;
    return;
  }

  if (now - groupLedLastToggle >= GROUP_BLINK_MS) {
    groupLedLastToggle = now;
    groupLedState = !groupLedState;
    digitalWrite(LED_GROUP_PIN, groupLedState ? HIGH : LOW);
  }
}

// ============================================================
// Polling: משיכת אירוע נוכחי מהשרת
// תומך בשני פורמטים:
// 1) {"active":true,...}
// 2) {"ok":true,"event":{...}}
// ============================================================
bool pollServerForEvent() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure net;
  net.setInsecure();

  HTTPClient http;
  http.setTimeout(5000);

  http.begin(net, CURRENT_EVENT_URL);
  int code = http.GET();

  if (code <= 0) {
    Serial.printf("[POLL] GET failed, code=%d\n", code);
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  Serial.printf("[POLL] code=%d body=%s\n", code, body.c_str());

  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("[POLL] JSON parse error: %s\n", err.c_str());
    return false;
  }

  JsonObject ev;
  if (doc.containsKey("event")) ev = doc["event"].as<JsonObject>();
  else ev = doc.as<JsonObject>();

  bool active = ev["active"] | false;
  const char* type = ev["type"] | "";

  terrorActive = (active && String(type) == "terror");

  Serial.printf("[POLL] terrorActive=%d (active=%d type=%s)\n",
                terrorActive ? 1 : 0, active ? 1 : 0, type);

  return true;
}

// ============================================================
// שליחת התראה לשרת
// ============================================================
bool sendAlertToServer(const String& status, const String& message) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[SERVER] WiFi not connected");
    return false;
  }

  WiFiClientSecure net;
  net.setInsecure();

  HTTPClient http;
  http.begin(net, SERVER_URL);
  http.addHeader("Content-Type", "application/json");

  if (String(SHARED_SECRET).length() > 0) {
    http.addHeader("X-SECRET", SHARED_SECRET);
  }

  StaticJsonDocument<384> doc;
  doc["device_id"] = DEVICE_ID;
  doc["status"]    = status;
  doc["message"]   = message;
  doc["event_lat"] = DEVICE_LAT;
  doc["event_lon"] = DEVICE_LON;

  JsonObject data = doc.createNestedObject("data");
  data["rssi"] = WiFi.RSSI();
  data["ip"]   = WiFi.localIP().toString();

  String payload;
  serializeJson(doc, payload);

  int code = http.POST(payload);
  String resp = http.getString();
  http.end();

  Serial.printf("[SERVER] POST status=%s msg=%s code=%d\n", status.c_str(), message.c_str(), code);
  Serial.println(resp);

  return (code > 0 && code < 300);
}

// ============================================================
// זיהוי עשן (✅ מתוקן: HOLD של 2 דקות)
// ============================================================
void detectSmoke() {
  int state = digitalRead(SMOKE_PIN);
  unsigned long now = millis();

  bool smokeDetectedNow = (state == LOW); // ברוב המודולים: LOW=עשן

  // יש עשן עכשיו -> נעדכן timestamp אחרון
  if (smokeDetectedNow) {
    lastSmokeDetectedMs = now;

    // אם עוד לא נכנסנו למצב עשן, נתחיל אירוע
    if (!smokeActive) {
      if (now - lastSmokeEventTime < SMOKE_MIN_INTERVAL_MS) return;

      smokeActive = true;
      lastSmokeEventTime = now;
      sendAlertToServer("smoke", "detected");
    }
    return;
  }

  // אין עשן עכשיו:
  if (!smokeActive) return;

  // נשארים במצב עשן עד שעברו 2 דקות נקי
  if (now - lastSmokeDetectedMs < SMOKE_HOLD_MS) return;

  // עברו 2 דקות בלי עשן -> חוזרים לשגרה
  smokeActive = false;
  sendAlertToServer("normal", "חזרה לשגרה - אין עשן");
}

// ============================================================
// זיהוי רעידת אדמה
// ============================================================
void detectEarthQuake() {
  int16_t ax, ay, az;
  mpu.getAcceleration(&ax, &ay, &az);

  const float G_PER_LSB = 1.0f / 16384.0f;
  float axg = ax * G_PER_LSB;
  float ayg = ay * G_PER_LSB;
  float azg = az * G_PER_LSB;

  float totalG = sqrt(axg * axg + ayg * ayg + azg * azg);
  float deltaG = fabs(totalG - 1.0f);

  unsigned long now = millis();

  int currentLevel = 0;
  if (deltaG >= QUAKE_STRONG_THRESHOLD) currentLevel = 2;
  else if (deltaG >= QUAKE_LIGHT_THRESHOLD) currentLevel = 1;

  if (currentLevel > 0) {
    lastQuakeMovementTime = now;
    normalSentAfterQuake = false;

    if (currentLevel > latchedQuakeLevel) {
      latchedQuakeLevel = currentLevel;
    }

    bool inCooldown = (now - lastAlertSentMs < QUAKE_COOLDOWN_MS);

    if (currentLevel == 2) {
      if (!strongSentThisEvent || !inCooldown) {
        sendAlertToServer("quake", "strong");
        lastAlertSentMs = now;
        strongSentThisEvent = true;
        lightSentThisEvent = true;
      }
    } else if (currentLevel == 1) {
      if (!lightSentThisEvent || !inCooldown) {
        sendAlertToServer("quake", "light");
        lastAlertSentMs = now;
        lightSentThisEvent = true;
      }
    }
  }

  if (latchedQuakeLevel > 0) {
    if (now - lastQuakeMovementTime <= QUAKE_END_QUIET_MS) {
      quakeState = latchedQuakeLevel;
      return;
    } else {
      quakeState = 0;

      if (!normalSentAfterQuake) {
        sendAlertToServer("normal", "חזרה לשגרה - רעידת אדמה פסקה");
        normalSentAfterQuake = true;
      }

      latchedQuakeLevel = 0;
      lightSentThisEvent = false;
      strongSentThisEvent = false;
      return;
    }
  }

  quakeState = 0;
}

// ============================================================
// עדכון LCD + באזר (כולל פח"ע)
// ============================================================
void updateDisplay() {
  int mode = 0;
  if (smokeActive) mode = 3;
  else if (quakeState == 2) mode = 2;
  else if (quakeState == 1) mode = 1;
  else mode = 0;

  bool anyEvent = (mode != 0) || terrorActive;
  if (!anyEvent) buzzerStop();
  else buzzerStart();

  static int lastShown = -999;
  int shown = terrorActive ? 99 : mode;

  if (shown == lastShown) return;
  lastShown = shown;

  lcd.clear();

  if (terrorActive) {
    lcd.setCursor(0, 0);
    lcd.print("TERROR ALERT!");
    lcd.setCursor(0, 1);
    lcd.print("GO TO MAMAD!");
    return;
  }

  switch (mode) {
    case 3:
      lcd.setCursor(0, 0);
      lcd.print("SMOKE ALERT!");
      lcd.setCursor(0, 1);
      lcd.print("CHECK HOUSE!");
      break;

    case 2:
      lcd.setCursor(0, 0);
      lcd.print("EARTH QUAKE!");
      lcd.setCursor(0, 1);
      lcd.print("LEAVE HOUSE!");
      break;

    case 1:
      lcd.setCursor(0, 0);
      lcd.print("Light quake");
      lcd.setCursor(0, 1);
      lcd.print("felt...");
      break;

    default:
      lcd.setCursor(0, 0);
      lcd.print("System Normal");
      lcd.setCursor(0, 1);
      lcd.print("Monitoring...");
      break;
  }
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);

  pinMode(TERROR_LED_PIN, OUTPUT);
  digitalWrite(TERROR_LED_PIN, LOW);

  pinMode(LED_GROUP_PIN, OUTPUT);
  digitalWrite(LED_GROUP_PIN, LOW);

  pinMode(HEARTBEAT_LED_PIN, OUTPUT);
  digitalWrite(HEARTBEAT_LED_PIN, LOW);

  Wire.begin(21, 22);

  // BUZZER PWM
  ledcAttach(BUZZER_PIN, 3000, 8);
  ledcWrite(BUZZER_PIN, 0);

  // MPU6050
  mpu.initialize();
  if (!mpu.testConnection()) {
    Serial.println("[MPU6050] ERROR: Connection failed! Check wiring.");
  } else {
    Serial.println("[MPU6050] Connected successfully.");
  }

  pinMode(SMOKE_PIN, INPUT);

  // LCD
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Booting system");
  lcd.setCursor(0, 1);
  lcd.print("Please wait...");
  delay(800);

  // WiFi
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Connecting to");
  lcd.setCursor(0, 1);
  lcd.print(String(ssid));
  delay(500);

  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  unsigned long startAttemptTime = millis();
  int counter = 9;

  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
    lcd.setCursor(counter, 1);
    lcd.print(".");
    Serial.print(".");
    counter++;
    if (counter > 15) {
      counter = 9;
      lcd.setCursor(9, 1);
      lcd.print("       ");
    }
    delay(500);
  }

  lcd.clear();
  if (WiFi.status() == WL_CONNECTED) {
    lcd.setCursor(0, 0);
    lcd.print("WiFi connected");
    Serial.println("\nWiFi connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    lcd.setCursor(0, 1);
    lcd.print("IP:");
    lcd.setCursor(3, 1);
    lcd.print(WiFi.localIP().toString());
  } else {
    lcd.setCursor(0, 0);
    lcd.print("WiFi isn't");
    lcd.setCursor(0, 1);
    lcd.print("connected");
    Serial.println("\nFailed to connect!");
  }

  // בדיקה ראשונית של polling
  pollServerForEvent();

  updateDisplay();
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  detectEarthQuake();
  detectSmoke();

  unsigned long now = millis();
  if (now - lastPollMs >= POLL_INTERVAL_MS) {
    lastPollMs = now;
    pollServerForEvent();
  }

  updateDisplay();
  buzzerUpdate();
  heartbeatUpdate();
  terrorLedUpdate();
  groupLedsUpdate();

  delay(50);
}
