# Weather‑Station‑1 (ESP32)

An ESP32‑based, solar‑friendly weather station that logs to SD, serves a live dark‑mode dashboard, and exposes a clean HTTP API.

<p align="center">
  <img alt="ESP32 Weather Station" src="docs/hero.png" width="640">
</p>

---

##  Features

- **Sensors:** BME680 (T/RH/P + gas), VEML7700 (ambient light)
- **Storage:** SD card (`/logs.csv`) with CSV header & rolling logs
- **Time:** DS3231 RTC (preferred) with NTP fallback
- **Connectivity:** Wi‑Fi Station with AP fallback, mDNS
- **Web UI:** Live dashboard, charts, log viewer & download
- **REST API:** `/live`, `/download`, `/view-logs`, `/config`, `/add`, `/del`, etc.
- **Power modes:** DAY (awake, periodic logs) / NIGHT (short serve window then deep sleep)
- **OTA:** ElegantOTA at `/update` (basic auth)

> Full endpoint and data schema docs: see **[API and Function Reference](docs/API.md)**.

---

##  Hardware

- **MCU:** ESP32 (DevKit style)
- **Sensors:** BME680 (I²C), VEML7700 (I²C)
- **RTC:** DS3231 (INT/SQW → GPIO2)
- **Storage:** microSD (SPI)
- **LED:** status LED on GPIO4
- **Rain gauge:** reed switch on GPIO27 (to GND)
- **Battery sense:** ADC pin 35 via 100k/100k divider

### Pinout (defaults)

| Function | Pin(s) |
|---|---|
| I²C SDA / SCL | 21 / 22 |
| SD CS / SCK / MISO / MOSI | 5 / 18 / 19 / 23 |
| Battery ADC | 35 |
| DS3231 INT | GPIO2 |
| Status LED | 4 |
| Rain gauge | 27 |

---

##  Repo layout

