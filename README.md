# Weather‑Station‑1 (ESP32)

An ESP32‑based, solar‑friendly weather station that logs to SD, serves a live dark‑mode dashboard, and exposes a clean HTTP API.

<p align="center">
  <img alt="ESP32 Weather Station" src="docs/hero.png" width="640">
</p>

---

## Features

- **Sensors:** BME680 (T/RH/P + gas), VEML7700 (ambient light)
- **Storage:** SD card (`/logs.csv`) with CSV header & rolling logs
- **Time:** DS3231 RTC (preferred) with NTP fallback
- **Connectivity:** Wi‑Fi Station with AP fallback, mDNS
- **Web UI:** Live dashboard, charts, log viewer & download
- **REST API:** `/live`, `/download`, `/view-logs`, `/config`, `/add`, `/del`, etc.
- **Power modes:** DAY (awake, periodic logs) / NIGHT (short serve window → deep sleep)
- **OTA:** ElegantOTA at `/update` (basic auth)

> Full endpoint and data schema: see **[docs/API.md](docs/API.md)**.

---

## Hardware

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
| DS3231 INT | 2 |
| Status LED | 4 |
| Rain gauge | 27 |

---

## Repo layout

```text
.
├─ WaetherStation08_22_25v17/   # Main Arduino sketch
├─ API.md                       # API reference & schema
└─ README.md
```

---

## Getting started

If no saved networks are found, the ESP32 launches a **temporary AP** for ~3 minutes so you can add Wi‑Fi credentials at `/add`.

### Prerequisites

- **Arduino IDE** (or PlatformIO)
- **ESP32 board package**
- Libraries:
  - Adafruit BME680 + Adafruit Unified Sensor
  - Adafruit VEML7700
  - RTClib
  - ArduinoJson
  - ElegantOTA
  - Core: `WiFi`, `WebServer`, `ESPmDNS`, `SD`, `SPI`, `Preferences`

### Build & flash

1. Open `WaetherStation08_22_25v17/*.ino` in Arduino IDE.
2. Select your ESP32 board & COM port.
3. (Optional) Update default OTA/AP credentials in the sketch.
4. Upload the firmware.
5. Open **Serial Monitor** @ **115200** to see IP and **mDNS** name.

---

## Web interface & API

After boot and Wi‑Fi join, open:

**http://WeatherStation1.local**

### Endpoints

| Endpoint        | Method | Description                                   |
|-----------------|--------|-----------------------------------------------|
| `/`             | GET    | Dashboard (live cards, charts, Wi‑Fi mgmt)    |
| `/update`       | GET    | ElegantOTA firmware upload (basic auth)       |
| `/download`     | GET    | Raw CSV log stream                            |
| `/view-logs`    | GET    | Recent rows in a table with filters           |
| `/config`       | GET/POST | Read/update persistent settings            |
| `/add`          | POST   | Add Wi‑Fi SSID/password                       |
| `/del?ssid=…`   | GET    | Delete a saved SSID                           |
| `/live`         | GET    | JSON telemetry & diagnostics                  |

### Example `/live` JSON

```json
{
  "temp": 72.8,
  "hum": 43.2,
  "pressure": 1013.62,
  "lux": 455,
  "batt": 4.07,
  "voc_kohm": 12.5,
  "dew_f": 50.3,
  "hi_f": 73.9,
  "wbt_f": 54.4,
  "mslp_hPa": 1019.3,
  "mslp_inHg": 30.10,
  "pressure_trend": "Steady",
  "forecast": "Fair",
  "rain_mmph": 0.28,
  "boot_count": 123,
  "uptime": 1234,
  "rssi": -56,
  "ssid": "MyWiFi",
  "sd_ok": true,
  "rtc_ok": true
}
```

---

## Power behavior

- **DAY:** stays awake; logs on cadence (`LOG_INTERVAL_MS`, default 10 min)
- **NIGHT:** short “serve” window after wake, then deep sleep (`DEEP_SLEEP_SECONDS`, default 10 min)
- Light thresholds (enter/exit DAY) use VEML7700 with hysteresis & dwell.

---

## Configuration

Open **`/config`** to adjust persistent settings (stored in Preferences):

- `altitude_m` — used for MSLP calculation  
- `temp_unit` — `F` or `C` (UI formatting)  
- `pressure_unit` — `hPa` or `inHg` (UI formatting)  
- `bat_cal` — ADC voltage calibration multiplier  
- `time_12h` — 12h or 24h display toggle  

Wi‑Fi networks are managed via:

- `POST /add` (add SSID/password)  
- `GET /del?ssid=...` (delete SSID)

---

## Security notes

- OTA endpoint `/update` uses **basic auth** — change the defaults before deploying.  
- `/add` and `/config` are plain HTTP; run on a trusted LAN.

---

## Troubleshooting

- If **mDNS** fails, use the serial‑printed IP or your router’s DHCP leases.
- If **SD** fails, verify wiring, CS pin, and card format.
- If **BME680** or **VEML7700** aren’t detected, check I²C wiring/addresses.
- If **RTC** is absent, the device falls back to timer‑only wakes.

---

## Roadmap (ideas)

- Optional AQ modules: MiCS‑5524, SCD41  
- UV sensing: S12SD  
- Power metering: INA3221  
- Wind subsystem: wind vane, accelerometer  
- Solar/charging: 900 mA MPPT controller
- RGB LED Status debugging


---

## License

Add your preferred license here (e.g., MIT).

---

## Credits

Built by @JoshLongmire and contributors. Libraries by Adafruit, Ayush Sharma (ElegantOTA), and the Arduino community.
