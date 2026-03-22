#  ESP32 Smart Alert System

##  Overview
A real-time embedded system that detects hazards such as smoke, earthquakes, and terror events using ESP32.

The system processes sensor data, displays alerts on an LCD, activates alarms (LEDs & buzzer), and communicates with a cloud server.
This project demonstrates real-time embedded system design, sensor integration, and cloud communication.
---

##  System Features

- Smoke detection using digital sensor
- Earthquake detection using MPU6050 (accelerometer)
- Terror alert system via server polling
- LCD display for real-time alerts
- Buzzer and LED alert system
- WiFi communication with backend server
- Automatic return to normal state after events

---

##  Technologies

- ESP32 (Arduino framework)
- C/C++
- WiFi & HTTP communication
- MPU6050 sensor
- I2C LCD display
- Flask backend (Python)

---
##  Hardware Components

- ESP32
- MPU6050 (accelerometer)
- Smoke sensor
- I2C LCD display
- Buzzer
- LEDs

##  Project Structure
firmware/ → ESP32 embedded code
server/ → Flask backend server
docs/ → diagrams and system explanation

---

##  How It Works

1. ESP32 reads data from sensors (smoke + accelerometer)
2. Detects events (smoke / earthquake)
3. Polls server for external alerts (terror events)
4. Displays alerts on LCD
5. Activates buzzer and LEDs
6. Sends alerts to backend server

---

##  Future Improvements

- Add RTOS support
- Improve filtering (DSP)
- Add more sensors
- Optimize power consumption
