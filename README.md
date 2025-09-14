# Weather‑Station‑1 (ESP32)

An ESP32‑based, solar‑friendly weather station that logs to SD, serves a live dark‑mode dashboard, and exposes a clean HTTP API.

<p align="center">
  <img alt="Weather Station on Tripod" src="docs/WeatherStationTripodMain.jpg" width="60%">
  <br><br>
  <img alt="Wind Speed Gauge Anemometer" src="docs/WindspeedgaugeAnemometer.jpg" width="32%">
  <img alt="Rain Gauge Tipping Bucket" src="docs/RainGaugeTippingBucketRainMeter.jpg" width="32%">
</p>

---

## Features

- **Sensors:** BME680 (T/RH/P + gas), VEML7700 (ambient light); optional: UV analog (GUVA‑S12SD), SDS011 (PM2.5/PM10), SCD41 (CO₂), Hall anemometer (wind), Wind vane (PCF8574)
- **Storage:** SD card (`/logs.csv`) with CSV header & rolling logs
- **Time:** DS3231 RTC (preferred) with NTP fallback
- **Connectivity:** Wi‑Fi Station with AP fallback, mDNS (configurable hostname)
- **Web UI:** Live dashboard, charts, log viewer & download
- **REST API:** `/live`, `/download`, `/view-logs`, `/config`, `/add`, `/del`, etc.
- **Power modes:** DAY (awake, periodic logs) / NIGHT (short serve window → deep sleep)
- **OTA:** ElegantOTA at `/update` (basic auth)

> Full endpoint and data schema: see **[docs/API.md](docs/API.md)**.

---

## Hardware

- **MCU:** ESP32‑S3 (Lonely Binary Dev Board, 16MB Flash / 8MB PSRAM); classic ESP32 also works
- **Sensors:** BME680 (I²C), VEML7700 (I²C), optional UV analog (GUVA‑S12SD), SDS011 (PM2.5/PM10), SCD41 (CO₂), Hall anemometer (wind), Wind vane (PCF8574 I²C)
- **RTC:** DS3231 (INT/SQW → GPIO2)
- **Storage:** microSD (SPI)
- **LED:** status LED on GPIO37 (S3 mapping)
- **Rain gauge:** reed switch to GND (GPIO18 in S3 mapping)
- **Battery sense:** ADC pin with 100k/100k divider (GPIO4 in S3 mapping)

### Pinout (defaults)

| Function | Pin(s) |
|---|---|
| I²C SDA / SCL | 8 / 9 |
| SD CS / SCK / MISO / MOSI | 5 / 12 / 13 / 11 |
| Battery ADC | 4 |
| DS3231 INT | 2 |
| Status LED | 37 |
| Rain gauge (tipping) | 18 |
| Wind (Hall) | 7 |
| Wind vane (PCF8574) | I²C (0x20-0x27) |
| UV analog (GUVA‑S12SD) | 6 |

### Board specifics

Tested with the Lonely Binary ESP32‑S3 Development Board (16MB Flash, 8MB PSRAM, IPEX antenna). For board details and pinout, see the product page: [Lonely Binary ESP32‑S3 Dev Board (Gold, IPEX)](https://lonelybinary.com/en-us/collections/esp32/products/esp32-s3-ipex?variant=43699253706909).

---

## Repo layout

```text
.
├─ WaetherStation08_24_25_v18/   # Main Arduino sketch
├─ docs/API.md                  # API reference & schema
└─ README.md
```

---

## Getting started

If no saved networks are found, the ESP32 launches a **temporary AP** for ~3 minutes so you can add Wi‑Fi credentials at `/add`.

Default AP: SSID `WeatherStation1`, password `12345678`.

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

1. Open `WaetherStation08_24_25_v18/WaetherStation08_24_25_v18.ino` in Arduino IDE.
2. For ESP32‑S3 select:
   - Board: `ESP32S3 Dev Module`
   - Flash Size: `16MB (128Mb)` (match your module)
   - PSRAM: `OPI PSRAM` (if present)
   - USB CDC On Boot: `Enabled` (optional)
   - CPU Freq: `240 MHz`
3. Select your COM port.
4. (Optional) Update default OTA/AP credentials in the sketch.
5. Upload the firmware.
6. Open **Serial Monitor** @ **115200** to see IP and **mDNS** name.

<p align="center">
  <img alt="Arduino IDE board settings" src="docs/IDE_Set.png" width="80%">
</p>

---

## Web interface & API

After boot and Wi‑Fi join, open:

**http://<mdnsHost>.local** (default: `weatherstation1.local`)

<p align="center">
  <img alt="Dashboard main page 1" src="docs/Mainpage1.png" width="32%">
  <img alt="Dashboard main page 2" src="docs/Mainpage2.png" width="32%">
  <img alt="Dashboard main page 3" src="docs/Mainpage3.png" width="32%">
</p>

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
| `/sleep`        | POST   | Enter deep sleep immediately                  |
| `/restart`      | GET/POST | Soft reboot                                 |

#### View Logs page (`/view-logs`)
<p align="center">
  <img alt="View Logs page" src="docs/ViewLogspage.png" width="90%">
</p>

#### OTA Update page (`/update`)
<p align="center">
  <img alt="OTA Update page" src="docs/OTAUpdatepage.png" width="70%">
</p>

### Example `/live` JSON (selected fields)

```json
{
  "temp_unit": "F",
  "temp": 72.8,
  "temp_f": 72.8,
  "temp_c": 22.7,
  "hum": 43.2,
  "pressure": 1013.62,
  "lux": 455,
  "uv_mv": 320,
  "uv_index": 3.2,
  "batt": 4.07,
  "voc_kohm": 12.5,
  "pm25_ugm3": 8.5,
  "pm10_ugm3": 12.1,
  "sds_ok": true,
  "sds_awake": true,
  "sds_warm": true,
  "sds_auto_sleep_ms_left": 85000,
  "co2_ppm": 760,
  "wind_mph": 3.4,
  "wind_avg_mph_1h": 2.7,
  "wind_dir": "NE",
  "wind_dir_idx": 1,
  "wind_vane_ok": true,
  "dew_f": 50.3,
  "hi_f": 73.9,
  "wbt_f": 54.4,
  "mslp_hPa": 1019.3,
  "mslp_inHg": 30.10,
  "pressure_trend": "Steady",
  "forecast": "Fair",
  "general_forecast": "Improving / Fair",
  "forecast_detail": "Air: Good | UV: High | Wind: Light (3 mph)",
  "aqi_category": "Good",
  "rain_mmph": 0.28,
  "rain_inph": 0.01,
  "rain_unit": "mm/h",
  "boot_count": 123,
  "uptime": 1234,
  "rssi": -56,
  "ssid": "MyWiFi",
  "time": "2025-01-01 15:42:17",
  "last_sd_log": "2025-01-01 15:40:00",
  "boot_started": "15:21:43",
  "wakeup_cause": 2,
  "wakeup_cause_text": "TIMER",
  "last_alarm": "2025-01-01 15:50:00",
  "sd_free_kb": 512000,
  "flash_free_kb": 2048,
  "heap": 176520,
  "sd_ok": true,
  "rtc_ok": true
}
```

---

## CSV log schema

File: `/logs.csv`

Header (extended v18+):
```
timestamp,temp_f,humidity,dew_f,hi_f,pressure,pressure_trend,forecast,lux,uv_mv,uv_index,voltage,voc_kohm,mslp_inHg,rain,boot_count,pm25_ugm3,pm10_ugm3,co2_ppm,wind_mph,wind_dir
```

Example row (units: temp °F, pressure hPa, MSLP inHg, rain mm/h or in/h per setting):
```
2025-01-01 15:42:17,72.8,43.2,50.3,73.9,1013.62,Steady,Fair,455.0,320,3.2,4.07,12.5,30.10,0.28,123,8.5,12.1,760,3.4,NE
```

Note: After the initial startup log, an extra boot event row is appended containing only the timestamp and `boot_count` (other numeric columns blank). Clearing logs via `/reset` writes the extended header above.

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
- `bat_cal` — ADC voltage calibration multiplier  
- `time_12h` — 12h or 24h display toggle  
- `rain_unit` — `mm/h` or `in/h` for log/UI rain values  
 - `lux_enter_day` — Daylight entry (lux). Default: 1600  
 - `lux_exit_day` — Night entry (lux). Default: 1400  
 - `log_interval_min` — Log interval (minutes) while awake. Default: 10  
 - `sleep_minutes` — Deep sleep duration (minutes) between wakes. Default: 10  
 - `trend_threshold_hpa` — Pressure trend threshold (hPa). Default: 0.6  
 - `mdns_host` — mDNS hostname label (no `.local`)  
 - `sds_mode` — SDS011 duty: `off`, `pre1`, `pre2`, `pre5`, `cont`  
 - `debug_verbose` — verbose serial logging toggle  

<p align="center">
  <img alt="Config settings page" src="docs/Configsettingspage.png" width="80%">
</p>

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
- Power metering: INA3221 (tried to wire,)
- RGB LED Status debugging

---

## Implemented Hardware
- [Added Wind subsystem: Accelerometer, the Wind Vane ](https://a.co/d/0iTu9BR)is connected to [PCF8574T PCF8574 IO Expansion Board Module](https://a.co/d/fuVj1YV)
- [Added a SCD41](https://a.co/d/65sVqnm)
- [UV sensing: S12SD UV Index](https://www.amazon.com/dp/B0CDWXCZ8L?ref=ppx_yo2ov_dt_b_fed_asin_title)
- [Solar/charging: 900 mA MPPT controller (Efficiency approved)](https://www.amazon.com/dp/B07MML4YJV?ref=ppx_yo2ov_dt_b_fed_asin_title)
- [Lonely Binary ESP32-S3 Development Board-16MB Flash, 8MB PSRAM, IPEX Antenna (Gold Edition)](https://lonelybinary.com/en-us/collections/esp32/products/esp32-s3-ipex?variant=43699253706909)

## Credits
- Main enclosure based on [Frog Box v2.0 (Rugged Waterproof Box Remix)](https://www.thingiverse.com/thing:4094861) by Nibb31 on Thingiverse
- Rain gauge: [Tipping bucket rain meter](https://www.printables.com/model/641148-tipping-bucket-rain-meter) By jattie on Printables 
- Wind anemometer: [Wind speed gauge anemometer v3.0](https://www.printables.com/model/625326-wind-speed-gauge-anemometer-v30) by shermluge on Printables
- SDS011 dust sensor enclosure: [SDS011 Dust sensor enclosure](https://www.thingiverse.com/thing:2516382) by sumpfing on Thingiverse
- Wind vane: Currently in development and design phase I need help with balancing the model, 

<p align="center">
  <img alt="Wind vane design in Fusion 360" src="docs/WindVaneFuison.png" width="48%">
  <img alt="Wind vane cross section selection" src="docs/WindVaneCrossSelection.png" width="48%">
</p>



Built by @JoshLongmire and contributors. Libraries by Adafruit, Ayush Sharma (ElegantOTA), and the Arduino community.
