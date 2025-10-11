### ESP32 Weather Station – Public API and Function Reference

This document describes the HTTP endpoints, persisted configuration, CSV schema, and the major functions exported in the current Arduino sketch (`WeatherStationv20_modular/`). The firmware uses a modular architecture with 7 clean modules for easier maintenance. It also includes examples and integration guidance.

## Modular Architecture (v20)

The firmware has been refactored into 7 clean modules for easier maintenance and development:

| Module | Files | Responsibilities |
|--------|-------|------------------|
| **Main** | `WeatherStationv20_modular.ino` | Orchestrates all modules, setup() and loop() coordination |
| **config** | `config.h/.cpp` | Configuration persistence (Preferences), `loadAppConfig()`, `saveAppConfig()` |
| **sensors** | `sensors.h/.cpp` | All sensor I/O, ISRs (rain, wind), hardware initialization |
| **weather** | `weather.h/.cpp` | 30+ meteorological calculations (dew point, ETo, forecasting) |
| **power** | `power.h/.cpp` | Day/Night state machine, deep sleep, LED status |
| **storage** | `storage.h/.cpp` | SD card operations, CSV logging, pressure history |
| **mqtt** | `mqtt.h/.cpp` | MQTT client, broker connection, forecast publishing |
| **web** | `web.h/.cpp` | HTTP server, all endpoints, WiFi management, OTA |

**Total**: 4,560 lines across 15 files (97% of original monolithic v19)

All HTTP endpoints are implemented in `web.cpp`, sensor functions in `sensors.cpp`, etc.

### Recent Improvements (v20.1)

**8 code quality improvements** implemented:
- ✅ Global timezone support via configurable `timezoneString` (no more hardcoded EST)
- ✅ MQTT exponential backoff (1s→30s) prevents broker abuse
- ✅ Stack optimization: 98.4% reduction (512→8 bytes) in rain calculations
- ✅ Code deduplication: `getRainTipsInWindow()` helper eliminates duplicate logic
- ✅ Named constants: All magic numbers replaced (BATTERY_ADC_SAMPLES, WIFI_CONNECT_TIMEOUT_MS, etc.)
- ✅ Const correctness: 30+ function signatures improved
- ✅ SD error handling: Directory creation failures now logged
- ✅ Code quality improved from 8.5 → 9.5/10

---

## Overview
- Device: ESP32-based weather station (tested on ESP32‑S3 — Lonely Binary ESP32‑S3 Dev Board, 16MB Flash / 8MB PSRAM; classic ESP32 also supported)
- Sensors: 
  - BME680 (temperature, humidity, pressure, gas resistance/VOC)
  - VEML7700 (ambient light, 0-120k lux range)
  - Optional: GUVA‑S12SD (UV index analog)
  - Optional: SDS011 (PM2.5/PM10 particulate matter with duty cycling)
  - Optional: Hall anemometer (wind speed + gust)
  - Optional: PCF8574 wind vane (8-point compass direction)
  - Optional: LM393 leaf wetness sensor (moisture detection with 24h accumulation)
- Optional: Accessories and verified purchase links are listed in the project **Bill of Materials**; see README or the quick list below.
- Storage: SD card (`/logs.csv`)
- Time: DS3231 RTC (preferred) with daily NTP syncing and drift correction
- Connectivity: Wi‑Fi STA with AP fallback, mDNS
- OTA: ElegantOTA at `/update` (basic auth)
- Modes: DAY (stay awake, periodic logs) / NIGHT (short serve window, deep sleep)
- MQTT: PubSubClient for home automation integration

### Bill of Materials (Quick Links)
- GPIO Expander PCF8574: [Amazon](https://www.amazon.com/dp/B098B5CGYJ?ref=ppx_yo2ov_dt_b_fed_asin_title)
- SDS011 High Precision PM2.5 Sensor: [Amazon](https://www.amazon.com/dp/B08QRJSVW7?ref=ppx_yo2ov_dt_b_fed_asin_title)
- VEML7700 Ambient Light Sensor Module: [Amazon](https://www.amazon.com/dp/B09KGYF83T?ref=ppx_yo2ov_dt_b_fed_asin_title)
- GUVA-S12SD UV Detect Sensor: [Amazon](https://www.amazon.com/dp/B0CDWXCZ8L?ref=ppx_yo2ov_dt_b_fed_asin_title)
- 3144E Hall Sensor Modules (10 pcs): [Amazon](https://www.amazon.com/dp/B09723WH5V?ref=ppx_yo2ov_dt_b_fed_asin_title)
- LM393 Rain Drops Sensor: [Amazon](https://www.amazon.com/dp/B01DK29K28?ref=ppx_yo2ov_dt_b_fed_asin_title)
- KOFU/OKI Reed Switches (6 pcs): [Amazon](https://www.amazon.com/dp/B0CW9418F6?ref=ppx_yo2ov_dt_b_fed_asin_title)
- DS3231 RTC Module (AT24C32): [Amazon](https://www.amazon.com/AITRIP-Precision-AT24C32-Arduino-Raspberry/dp/B09KPC8JZQ/ref=sr_1_5?crid=1BBM1A7IR757A&dib=eyJ2IjoiMSJ9.Av0ZT44mgzkEZLgrGYpmsc1bvAskDxukuEiBsIwEXkYXsy4pV1QL2kCmA6ATcUEOhtm86LcHQ3Ou8hoDzPAtWIL_MkbCcCGZqELL_JF2uIvDc5X5CHjf0QYUkwW1rr16e5uazRba5GyGkncrnCRiF33UoRrt3gmwKCu_76rOcOhUzpetDdE9-LbBG-jY1gpHfbe9xFJP3_h11FboQfv7qtcjeKRFkHxdTkyHdOtSbhA.KEgg1t8EO4x71IAhc9Eu8IFMK_E0WWU1dN-m4JL65EI&dib_tag=se&keywords=DS3231+RTC+Module&qid=1759834055&sprefix=ds3231+rtc+module+%2Caps%2C352&sr=8-5)
- BME680 Temperature/Humidity/Pressure/VOC: [Amazon](https://www.amazon.com/dp/B0CDWXZNY7?ref=ppx_yo2ov_dt_b_fed_asin_title)
- 5W Solar Panel: [Amazon](https://www.amazon.com/dp/B0DPDNGYDV?ref=ppx_yo2ov_dt_b_fed_asin_title)

## Pins, Interfaces, and Constants

### ESP32‑S3 (Lonely Binary) default mapping in this sketch:
- **I²C:** SDA=8, SCL=9, Frequency=100kHz
- **SD Card (SPI):** CS=5, SCK=12, MISO=13, MOSI=11
- **Battery ADC:** Pin 4 (voltage divider R1=100k, R2=100k)
- **Rain gauge (reed switch):** Pin 18 (to GND, interrupt-driven)
- **DS3231 RTC interrupt:** GPIO_NUM_2
- **Status LED:** Pin 37
- **Hall anemometer (wind):** Pin 7 (interrupt-driven)
- **Wind vane (PCF8574 I²C):** Addresses 0x20-0x27 (auto-detected)
- **UV analog (GUVA‑S12SD):** Pin 6
- **Leaf wetness analog:** Pin 3 (S3), Pin 34 (classic ESP32)
- **SDS011 UART:** RX=16, TX=17
- **BME680 I²C addresses:** 0x76 (primary), 0x77 (fallback)

### Classic ESP32 pins:
- **I²C:** SDA=21, SCL=22
- **Leaf wetness analog:** Pin 34

### Timing and mode constants:
- `DEEP_SLEEP_SECONDS = 600` (10 minutes between wakes in NIGHT mode)
- Initial serve window after cold boot: `UPTIME_STARTUP = 1800` seconds (30 minutes)
- Subsequent serve windows after wake: `UPTIME_CONFIG = 120` seconds (2 minutes)
- Logging cadence while awake: `LOG_INTERVAL_MS = 600000` (10 minutes, configurable via `/config`)
- Day/Night light thresholds: 
  - Enter DAY: ≥ 1600 lux (configurable via `lux_enter_day`)
  - Exit DAY: ≤ 1400 lux (configurable via `lux_exit_day`)
  - Dwell time: 30 seconds (hysteresis to prevent mode flapping)

### Feature flags (compile-time):
- `ENABLE_SDS011` — SDS011 PM2.5/PM10 sensor support (default: 1)
- `ENABLE_BSEC2` — Bosch BSEC2 library for advanced BME680 IAQ (default: 0, uses Adafruit_BME680 instead)
- `ENABLE_WIND` — Hall anemometer wind speed (default: 1)
- `ENABLE_WINDVANE` — PCF8574 wind vane direction (default: 1)
- `ENABLE_LEAF` — LM393 leaf wetness sensor (default: 1)

## CSV Log Schema

File: `/logs.csv`

### Header (extended v18.2, includes wind gust, rain totals, leaf wetness, ETo):
```
timestamp,temp_f,humidity,dew_f,hi_f,pressure,pressure_trend,forecast,lux,uv_mv,uv_index,voltage,voc_kohm,mslp_inHg,rain,boot_count,pm25_ugm3,pm10_ugm3,wind_mph,wind_dir,wind_gust_mph,rain_1h,rain_today,rain_event,leaf_raw,leaf_pct,leaf_wet,leaf_wet_hours,eto_hourly,eto_daily
```

### Column descriptions (30 columns total):
1. **timestamp** — Local time in format `YYYY-MM-DD HH:MM:SS` (12h or 24h per config)
2. **temp_f** — Temperature in °F
3. **humidity** — Relative humidity (%)
4. **dew_f** — Dew point in °F (computed from temp and humidity)
5. **hi_f** — Heat index in °F (Rothfusz regression)
6. **pressure** — Station pressure in hPa
7. **pressure_trend** — `Rising|Falling|Steady` based on 3/6/12h history
8. **forecast** — Simple forecast label (Zambretti-style)
9. **lux** — Ambient light from VEML7700 (0-120k range)
10. **uv_mv** — UV sensor raw millivolts
11. **uv_index** — Approximate UV index (0-11+)
12. **voltage** — Battery voltage (V) with calibration applied
13. **voc_kohm** — BME680 gas resistance in kΩ (VOC proxy; higher = cleaner air)
14. **mslp_inHg** — Mean sea-level pressure in inHg
15. **rain** — Instantaneous rain rate (mm/h or in/h per config unit)
16. **boot_count** — Cumulative boot/wake counter (persisted in RTC memory)
17. **pm25_ugm3** — PM2.5 particulate matter (µg/m³) from SDS011
18. **pm10_ugm3** — PM10 particulate matter (µg/m³) from SDS011
19. **wind_mph** — Instantaneous wind speed in mph from Hall anemometer
20. **wind_dir** — Wind direction (8-point compass: N, NE, E, SE, S, SW, W, NW)
21. **wind_gust_mph** — Wind gust (highest 5-second sample in last 10 minutes)
22. **rain_1h** — Rain accumulation last 1 hour (mm or in per config unit)
23. **rain_today** — Rain accumulation since local midnight (mm or in per config unit)
24. **rain_event** — Rain accumulation since last ≥6h dry gap (mm or in per config unit)
25. **leaf_raw** — EMA-smoothed raw ADC value from leaf wetness sensor (0-4095)
26. **leaf_pct** — Wetness percentage (0-100%, 0=dry, 100=wet)
27. **leaf_wet** — Wet/dry boolean state (0=dry, 1=wet)
28. **leaf_wet_hours** — Accumulated wet hours since local midnight (decimal hours)
29. **eto_hourly** — Reference evapotranspiration rate (mm/h or in/h per config unit)
30. **eto_daily** — Cumulative reference evapotranspiration today (mm/day or in/day per config unit)

### Example row:
```
2025-01-01 15:42:17,72.8,43.2,50.3,73.9,1013.62,Steady,Fair,455.0,320,3.2,4.07,12.5,30.10,0.28,123,8.5,12.1,3.4,NE,7.8,0.12,0.34,0.34,2450,45,0,2.3,0.152,3.65
```

### Notes:
- Temperature values are always in °F in CSV (Celsius available in `/live` JSON)
- Pressure is station pressure in hPa; MSLP column is in inHg
- Rain columns use the configured unit (`rain_unit` setting: `mm` or `in`) at write time
- `wind_gust_mph` uses classic NOAA method: highest 5-second sample rate in last 10 minutes
- Legacy logs (pre‑v18) used 14 columns; new columns were appended. Clearing logs via `/reset` writes the extended header.
- Boot event: After initial startup log, an extra row is appended with only timestamp and boot_count (other fields blank)

## HTTP API

Base: device IP (e.g., `http://192.168.1.50`) or mDNS `http://<mdnsHost>.local` (configurable in `/config`, default: `weatherstation1.local`).

---

### GET `/`

**Description:** Returns the live dashboard HTML with dark theme, organized cards, real-time charts (180-point rolling history), and Wi‑Fi management UI.

**Features:**
- **Enhanced organized tile layout** with forecast prominently displayed at top-left (3-column span for comprehensive details)
- Live sensor readings with 2-second auto-refresh
- **Enhanced forecast tile** with advanced multi-sensor prediction and detailed outlook spanning 3 columns
- Canvas-based line charts for temperature, humidity, pressure/MSLP, lux, UV, battery, dew point, heat index, wet bulb, VOC, PM2.5/PM10, rain rate, wind speed, wind average
- **Categorized legend** with visual sections: Wind & Rain, Light & UV, Air Quality, Comfort, Agriculture, Forecast, System
- Configurable pressure display (MSLP sea-level or station pressure)
- Estimated battery % (Li-ion curve) in battery card header
- Wi-Fi signal strength bars (▂▄▅▆▇) in RSSI card
- Buttons/links: Download CSV, View Logs, OTA Update, Force Sleep, Restart Device, Config Settings, Clear Logs

**Dashboard Organization (v19.0):**
- Row 1: Weather Forecast (3-column span for detailed outlook)
- Row 2: Key Metrics (Pressure Trend, Temperature, Humidity)
- Row 3: Core Environmental Sensors (MSLP/Station Pressure, Dew Point)
- Row 4: Wind & Rain metrics (speed, direction, avg, gust, rain)
- Row 5: Light & UV (Lux, UV Index, UV mV)
- Row 6: Air Quality (PM2.5, PM10, VOC)
- Row 7: Comfort (Heat Index, Wet Bulb)
- Row 8: Agriculture (Leaf Wetness, ETo)
- Row 9: System Status

**Response:** `text/html; charset=utf-8`

**Example:**
```bash
curl http://weatherstation1.local/
```

---

### GET `/live`

**Description:** Returns current sensor readings and diagnostics as JSON. This endpoint is polled by the dashboard every 2 seconds and can be used by external integrations.

**Response:** `application/json`

**Fields (comprehensive):**

#### Core Sensors
- `temp_unit` (string) — Current unit preference: `F` or `C`
- `temp` (float) — Temperature in preferred unit
- `temp_f` (float) — Temperature in °F
- `temp_c` (float) — Temperature in °C
- `hum` (float) — Relative humidity (%)
- `pressure` (float) — Station pressure (hPa)
- `lux` (float) — Ambient light (0-120000 lux)
- `uv_mv` (float) — UV sensor raw millivolts
- `uv_index` (float) — Approximate UV index (0-11+)
- `batt` (float) — Battery voltage (V)
- `voc_kohm` (float) — BME680 gas resistance (kΩ)
- `bme_ok` (bool) — BME680 reading success flag

#### Derived Meteorology
- `dew_f` (float) — Dew point in °F
- `dew_c` (float) — Dew point in °C
- `hi_f` (float) — Heat index in °F
- `hi_c` (float) — Heat index in °C
- `wbt_f` (float) — Wet bulb temperature in °F (Stull approximation)
- `wbt_c` (float) — Wet bulb temperature in °C
- `mslp_hPa` (float) — Mean sea-level pressure (hPa)
- `mslp_inHg` (float) — Mean sea-level pressure (inHg)

#### Pressure Trends & Forecast
- `pressure_trend` (string) — `Rising|Falling|Steady` (based on 3/6/12h Δ)
- `forecast` (string) — **Enhanced multi-sensor prediction** with 40+ distinct states including:
  - Storm conditions: `Severe Storm|Major Storm|Storm / Heavy Rain|Heavy Showers|Thunderstorms Possible|Severe Risk`
  - Frontal systems: `Front Approaching|Becoming Cloudy|Clouds Increasing`
  - Rain levels: `Heavy Rain|Raining|Light Rain|Heavy Showers Soon|Rain Likely|Rain Developing`
  - Improving conditions: `Very Clear & Sunny|Clear & Bright|Clearing / Sunny|Improving|Slowly Improving`
  - Settled weather: `Very Settled / Clear|Settled / Fine|Fair / Settled|Clear & Dry`
  - Variable: `Partly Cloudy|Variable / Bright Spells|Cloudy / Stable|Unsettled / Variable`
- `general_forecast` (string) — Concise forecast summary for quick reference
- `forecast_detail` (string) — Comprehensive multi-line forecast with:
  - **Storm warnings** (Severe/High/Possible risk with ⚠️ alerts)
  - **Frontal passage detection** (approaching cold/warm fronts)
  - Air quality category (Good/Moderate/USG/Unhealthy/Very Unhealthy/Hazardous)
  - UV risk (Low/Moderate/High/Very High/Extreme)
  - Enhanced wind info (Calm/Light/Breezy/Windy/Very Windy) with speed, direction, and gust warnings
  - Humidity comfort notes (Very Humid/Humid/Dry)
  - Pressure tendency (Rapid P Drop/Falling P/Rising P)
  - Rain status (Raining Now) or risk (High/Moderate/Low)
  - Rain totals (1h/Today/Event in configured unit)
  - Heat risk (Low/Caution/Extreme)
  - Fog/Frost risk (when conditions met)
  - Rain 3/6/12h trend arrows (↑ rising, → steady, ↓ falling)
- `storm_risk` (bool) — True when storm risk is elevated (not "Minimal")
- `aqi_category` (string) — Air quality category from PM2.5 (Good/Moderate/USG/Unhealthy/Very Unhealthy/Hazardous)

**Forecast Algorithm:**  
Uses advanced multi-sensor fusion combining pressure trends, wind speed/gusts, humidity, temperature, dew point, UV index, rain rate, and light levels. Includes:
- Severe weather scoring (0-3 scale based on pressure drop rate, wind gusts, atmospheric instability)
- Frontal detection (cold front: falling P + rising wind; warm front: falling P + high humidity)
- Storm risk levels: Minimal/Low/Slight/Possible/High/Severe
- Temperature trend analysis using dew point spread

#### Rain Gauge
- `rain_mmph` (float) — Instantaneous rain rate (mm/h)
- `rain_inph` (float) — Instantaneous rain rate (in/h)
- `rain_unit` (string) — Current CSV/UI unit: `mm/h` or `in/h`
- `rain_1h_mm` (float) — Rain last 1 hour (mm)
- `rain_1h_in` (float) — Rain last 1 hour (inches)
- `rain_today_mm` (float) — Rain since local midnight (mm)
- `rain_today_in` (float) — Rain since local midnight (inches)
- `rain_event_mm` (float) — Rain since last ≥6h dry gap (mm)
- `rain_event_in` (float) — Rain since last ≥6h dry gap (inches)
- `rain_in_per_tip` (float) — Configured tip size (inches per bucket tip)

#### Wind (Hall Anemometer + PCF8574 Vane)
- `wind_hz` (float) — Pulse frequency (Hz)
- `wind_mps` (float) — Wind speed (m/s)
- `wind_kmh` (float) — Wind speed (km/h)
- `wind_mph` (float) — Wind speed (mph)
- `wind_gust_mph` (float) — Highest 5-second sample in last 10 minutes (mph)
- `wind_ok` (bool) — Wind sensor recent activity flag
- `wind_avg_mph_1h` (float) — Rolling 1-hour average wind speed (sampled once per minute)
- `wind_dir` (string) — 8-point compass direction: `N|NE|E|SE|S|SW|W|NW` or `--` (dead zone) or `N/A` (no vane)
- `wind_dir_idx` (int) — Direction index 0-7, or -1 if unknown
- `wind_vane_ok` (bool) — PCF8574 wind vane detected flag

#### Dust Sensor (SDS011)
- `pm25_ugm3` (float) — PM2.5 particulate matter (µg/m³)
- `pm10_ugm3` (float) — PM10 particulate matter (µg/m³)
- `sds_ok` (bool) — SDS011 presence flag (true after first valid frame)
- `sds_awake` (bool) — SDS011 fan powered on
- `sds_warm` (bool) — SDS011 warm-up complete (30s after wake)
- `sds_auto_sleep_ms_left` (int) — Milliseconds until auto-sleep in 2-minute window (0 when not applicable)

#### Leaf Wetness (LM393)
- `leaf_raw` (float) — EMA-smoothed raw ADC value (0-4095)
- `leaf_pct` (float) — Wetness percentage (0-100%, 0=dry, 100=wet)
- `leaf_wet` (bool) — Wet/dry state (hysteresis applied)
- `leaf_wet_hours_today` (float) — Accumulated wet hours since local midnight
- `leaf_adc_dry` (int) — Raw calibration endpoint DRY (only when `leaf_debug=true`)
- `leaf_adc_wet` (int) — Raw calibration endpoint WET (only when `leaf_debug=true`)
- `leaf_wet_on_pct` (float) — Wet threshold ON % (only when `leaf_debug=true`)
- `leaf_wet_off_pct` (float) — Wet threshold OFF % (only when `leaf_debug=true`)

#### Reference Evapotranspiration (FAO-56)
- `eto_hourly_mm` (float) — Hourly ETo rate (mm/h)
- `eto_hourly_in` (float) — Hourly ETo rate (in/h)
- `eto_daily_mm` (float) — Cumulative ETo today (mm/day)
- `eto_daily_in` (float) — Cumulative ETo today (in/day)
- `eto_unit` (string) — Current CSV/UI unit: `mm/day` or `in/day`

**Method:**
- Prefers FAO-56 Penman-Monteith when temperature, humidity, wind speed, pressure, and lux are available
- Falls back to Hargreaves-Samani when solar radiation (lux) is unavailable or during night
- Daily accumulation resets at local midnight
- Hourly values are logged at each cadence interval

#### System & Diagnostics
- `uptime` (int) — Uptime in seconds since current boot
- `heap` (int) — Free heap memory (bytes)
- `flash_free_kb` (int) — Free flash space (KB)
- `time` (string) — Current local time (12h or 24h format per config)
- `boot_started` (string) — Boot start time (HH:MM:SS format, 12h or 24h)
- `sd_ok` (bool) — SD card mounted and `/logs.csv` accessible
- `rtc_ok` (bool) — DS3231 RTC detected and functional
- `boot_count` (int) — Cumulative boot/wake counter (persisted in RTC memory)
- `rssi` (int) — Wi-Fi signal strength (dBm), 0 when disconnected
- `ssid` (string) — Connected Wi-Fi SSID, empty when disconnected
- `wakeup_cause` (int) — ESP32 wake reason code
- `wakeup_cause_text` (string) — Human-readable wake reason: `RTC ALARM|TIMER|ColdStart/Config|TOUCH|ULP`
- `last_alarm` (string) — Last scheduled RTC alarm time (format per config), `N/A` when RTC unavailable
- `sd_free_kb` (int) — Estimated SD card free space (KB)
- `last_sd_log` (string) — Timestamp of last CSV log write (format per config)

### Example JSON response:
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
  "bme_ok": true,
  "pm25_ugm3": 8.5,
  "pm10_ugm3": 12.1,
  "sds_ok": true,
  "sds_awake": true,
  "sds_warm": true,
  "sds_auto_sleep_ms_left": 85000,
  "wind_hz": 2.2,
  "wind_mps": 1.5,
  "wind_kmh": 5.4,
  "wind_mph": 3.4,
  "wind_gust_mph": 7.8,
  "wind_ok": true,
  "wind_avg_mph_1h": 2.7,
  "wind_dir": "NE",
  "wind_dir_idx": 1,
  "wind_vane_ok": true,
  "leaf_raw": 2450.3,
  "leaf_pct": 45.2,
  "leaf_wet": false,
  "leaf_wet_hours_today": 2.3,
  "eto_hourly_mm": 0.152,
  "eto_hourly_in": 0.006,
  "eto_daily_mm": 3.65,
  "eto_daily_in": 0.144,
  "eto_unit": "mm/day",
  "uptime": 1234,
  "heap": 176520,
  "flash_free_kb": 2048,
  "time": "2025-01-01 15:42:17",
  "rain_mmph": 0.28,
  "rain_inph": 0.01,
  "rain_unit": "mm/h",
  "rain_1h_mm": 3.0,
  "rain_1h_in": 0.12,
  "rain_today_mm": 8.6,
  "rain_today_in": 0.34,
  "rain_event_mm": 8.6,
  "rain_event_in": 0.34,
  "rain_in_per_tip": 0.011,
  "boot_started": "15:21:43",
  "sd_ok": true,
  "rtc_ok": true,
  "boot_count": 123,
  "rssi": -56,
  "ssid": "MyWiFi",
  "wakeup_cause": 2,
  "wakeup_cause_text": "TIMER",
  "last_alarm": "2025-01-01 15:50:00",
  "sd_free_kb": 512000,
  "mqtt_enabled": true,
  "mqtt_connected": true,
  "mqtt_last_publish": 1735748537,
  "mqtt_broker": "192.168.1.100",
  "mqtt_topic_prefix": "weatherstation",
  "dew_f": 50.3,
  "dew_c": 10.2,
  "hi_f": 73.9,
  "hi_c": 23.3,
  "wbt_f": 54.4,
  "wbt_c": 12.4,
  "mslp_hPa": 1019.3,
  "mslp_inHg": 30.10,
  "pressure_trend": "Steady",
  "forecast": "Fair",
  "general_forecast": "Improving / Fair",
  "forecast_detail": "Air: Good | UV: High | Wind: Light (3 mph NE, G: 8) | Rain 3/6/12h: →/→/→",
  "aqi_category": "Good",
  "storm_risk": false,
  "last_sd_log": "2025-01-01 15:40:00"
}
```

**cURL example:**
```bash
curl http://weatherstation1.local/live | jq .
```

---

### GET `/download`

**Description:** Streams the raw CSV file for download. Suitable for batch ingestion and archival.

**Response:** `text/csv`

**Headers:** `Content-Disposition: attachment; filename=logs.csv`

**Example:**
```bash
curl -LOJ http://weatherstation1.local/download
```

---

### GET `/view-logs`

**Description:** Renders the last 200 rows of the CSV in a styled HTML table with dark theme and client-side filters.

**Features:**
- Field selector dropdown (Temp, Humidity, Dew Point, Heat Index, Pressure, Lux, UV mV, UV Index, Battery, VOC, MSLP, Rain, Boot Count, PM2.5, PM10, Wind, Wind Gust, Rain 1h, Rain Today, Rain Event)
- Filter types: Between (min/max), ≥ Min, ≤ Max
- Auto Range button to auto-populate min/max from column data
- Query parameter support for direct links (e.g., `?field=temp&type=between&min=60&max=80`)
- Backward-compatible with legacy 6-column and 14-column CSV formats

**Response:** `text/html; charset=utf-8`

**Query Parameters (optional):**
- `field` (string) — Field name or column index (e.g., `temp`, `hum`, `1`, `2`)
- `type` (string) — Filter type: `between|gte|lte`
- `min` (number) — Minimum value
- `max` (number) — Maximum value

**Field name mappings:**
```
temp/temperature → 1
hum/humidity → 2
dew/dew_f → 3
hi/heat/heat_index → 4
pressure → 5
lux/light/illuminance → 8
bat/batv/battery → 11
voc/gas → 12
mslp/slp/sea_level → 13
rain → 14
boot/boots/boot_count → 15
pm25/pm25_ugm3 → 16
pm10/pm10_ugm3 → 17
wind/wind_mph → 18
gust/wind_gust → 20
rain1h/rain_1h → 21
rain_today → 22
rain_event → 23
```

**Example:**
```bash
# View logs page
curl http://weatherstation1.local/view-logs

# Direct filter link: show rows with temp between 60-80°F
curl "http://weatherstation1.local/view-logs?field=temp&type=between&min=60&max=80"
```

---

### GET `/config`

**Description:** Returns a simple HTML form to view and change persistent settings.

**Response:** `text/html`

**Displayed Fields (organized into sections):**

**🖥️ System Settings**
- `altitude_m` (float) — Altitude in meters for MSLP calculation
- `temp_unit` (select: F/C) — Temperature display preference
- `bat_cal` (float) — Battery voltage calibration multiplier (default: 1.08)
- `time_12h` (select: 12/24) — Clock format
- `mdns_host` (string) — mDNS hostname label (no `.local` suffix)
- `location_name` (string) — Friendly display name for this station (max 50 chars)
- `longitude` (float) — Longitude in degrees for solar calculations (-180 to 180)
- `timezone_offset` (int) — Hours offset from UTC (supports half-hour zones like +5.5)
- `timezone_string` (string) — **NEW v20.1:** POSIX timezone string for accurate DST handling (max 64 chars, default: "EST5EDT,M3.2.0/2,M11.1.0/2")

**🔋 Power & Timing**
- `lux_enter_day` (float) — Light threshold to enter DAY mode (default: 1600 lux)
- `lux_exit_day` (float) — Light threshold to exit DAY mode (default: 1400 lux)
- `log_interval_min` (int) — CSV logging interval while awake (1-1440 minutes, default: 10)
- `sleep_minutes` (int) — Deep sleep duration between wakes (1-1440 minutes, default: 10)

**🌡️ Pressure & Forecast**
- `trend_threshold_hpa` (float) — Pressure delta threshold for trend classification (0.1-5.0 hPa, default: 0.6)
- `show_mslp` (select: mslp/station) — **NEW:** Dashboard pressure display: MSLP (sea-level, inHg) or station pressure (hPa, not altitude-adjusted). Default: MSLP

**🌧️ Rain Gauge**
- `rain_unit` (select: mm/in) — Rain rate unit for CSV/UI
- `rain_tip_in` (float) — Inches per tipping bucket tip (0.001-0.1, default: 0.011)
- `rain_debounce_ms` (int) — ISR debounce window in milliseconds (50-500, default: 150)

**💨 Air Quality (SDS011)**
- `sds_mode` (select) — SDS011 duty preset:
  - `off` — Keep sensor asleep
  - `pre1` — Wake 1 minute before each log
  - `pre2` — Wake 2 minutes before each log (default)
  - `pre5` — Wake 5 minutes before each log
  - `cont` — Continuous operation while awake

**🍃 Leaf Wetness Sensor**
- `leaf_debug` (select: Off/On) — Show raw ADC and calibration values on dashboard for field calibration
- `leaf_adc_dry` (int) — Raw ADC value when sensor is dry (0-4095, default: 3300)
- `leaf_adc_wet` (int) — Raw ADC value when sensor is wet (0-4095, default: 1400)
- `leaf_wet_on_pct` (float) — % threshold to declare WET (0-100%, default: 55.0)
- `leaf_wet_off_pct` (float) — % threshold to declare DRY (0-100%, default: 45.0)

**💧 Evapotranspiration (ETo)**
- `eto_unit` (select: mm/in) — ETo unit for CSV/UI (mm/day or in/day)
- `latitude` (float) — Latitude in degrees for ETo solar radiation calculation (−90 to +90, negative = South, default: 40.0)

**🐛 Debug**
- `debug_verbose` (select: Off/On) — Enable verbose serial logging

**📊 Dashboard Settings**
- `dashboard_refresh_rate` (int) — Dashboard auto-refresh rate in seconds (1-60, default: 2)
- `show_advanced_metrics` (select: Off/On) — Display advanced meteorological calculations (default: On)
- `dark_mode` (select: Off/On) — UI theme preference: Dark mode for night viewing, Light mode for bright conditions (default: On)
- `chart_data_points` (int) — Number of historical data points in line charts (60-500, default: 180)

**🌦️ Enhanced Forecasting**
- `enhanced_forecast_enabled` (select: Off/On) — Enable advanced multi-sensor forecasting with 40+ forecast states (default: On)
- `forecast_sensitivity` (select: 1-5) — Forecast response sensitivity: 1=Conservative, 2=Stable, 3=Balanced, 4=Sensitive, 5=Very Sensitive (default: 3)
- `storm_detection_enabled` (select: Off/On) — Enable automatic storm detection and severe weather alerts (default: On)
- `storm_risk_threshold` (float) — Minimum storm risk level to trigger alerts (0.5-5.0 scale, default: 2.0)

**📡 WiFi & MQTT Integration**
- `wifi_reconnect_delay` (int) — Seconds between WiFi reconnection attempts (5-300, default: 30)
- `mqtt_enabled` (select: Off/On) — Enable MQTT publishing for home automation integration (default: Off)
- `mqtt_broker` (string) — MQTT broker IP address or hostname (max 100 chars)
- `mqtt_port` (int) — MQTT broker TCP port (1-65535, default: 1883)
- `mqtt_username` (string) — MQTT authentication username (max 50 chars)
- `mqtt_password` (string) — MQTT authentication password (max 100 chars)
- `mqtt_topic` (string) — MQTT topic prefix for all published messages (max 50 chars, default: "weatherstation")
- `mqtt_interval` (int) — MQTT publish interval in minutes (1-60, default: 5)
- **NEW v20.1:** MQTT now implements exponential backoff (1s→2s→4s→8s→16s→30s) on connection failures to prevent broker abuse


**🔋 Battery & Power Management**
- `battery_low_threshold` (float) — Low battery voltage threshold for warnings (2.5-4.2V, default: 3.3V)
- `battery_critical_threshold` (float) — Critical battery voltage for forced shutdown (2.5-4.2V, default: 3.0V)
- `solar_power_mode` (select: Off/On) — Enable solar power optimizations (default: Off)
- `deep_sleep_timeout` (int) — Safety timeout before forced deep sleep in DAY mode (30-1440 minutes, default: 180)

**Example:**
```bash
curl http://weatherstation1.local/config
```

---

### POST `/config`

**Description:** Saves settings to Preferences namespace `app`. All fields are optional; only provided fields are updated.

**Form Parameters:**
- `alt` (float) — Altitude in meters
- `tu` (string) — Temperature unit: `F` or `C`
- `bc` (float) — Battery calibration multiplier
- `tf` (string) — Time format: `12` or `24`
- `led` (float) — Lux threshold to enter DAY
- `lxd` (float) — Lux threshold to exit DAY
- `lim` (int) — Log interval in minutes (1-1440)
- `slm` (int) — Sleep duration in minutes (1-1440)
- `pth` (float) — Pressure trend threshold (0.1-5.0 hPa)
- `pdisp` (string) — Pressure display: `mslp` (sea-level) or `station` (raw)
- `lon` (float) — Longitude (-180 to 180 degrees)
- `tz` (float) — Timezone offset from UTC (-12 to 14 hours)
- `tzstr` (string) — **NEW v20.1:** POSIX timezone string (max 64 chars)
- `locname` (string) — Location display name (max 50 chars)
- `ru` (string) — Rain unit: `mm` or `in`
- `rtip` (float) — Rain tip size (0.001-0.1 inches)
- `rdb` (int) — Rain debounce (50+ ms)
- `mdns` (string) — mDNS hostname label
- `sds` (string) — SDS011 mode: `off|pre1|pre2|pre5|cont`
- `leafdbg` (string) — Leaf debug: `0` or `1`
- `leafdry` (int) — Leaf ADC dry calibration (0-4095)
- `leafwet` (int) — Leaf ADC wet calibration (0-4095)
- `leafweton` (float) — Leaf wet threshold ON (0-100%)
- `leafwetoff` (float) — Leaf wet threshold OFF (0-100%)
- `etou` (string) — ETo unit: `mm` or `in`
- `lat` (float) — Latitude (−90 to 90 degrees)
- `dbg` (string) — Verbose debug: `0` or `1`
- `dashref` (int) — Dashboard refresh rate (1-60 seconds)
- `advmet` (string) — Show advanced metrics: `0` or `1`
- `darkmod` (string) — Dark mode: `0` or `1`
- `chartpts` (int) — Chart data points (60-500)
- `enhfore` (string) — Enhanced forecast: `0` or `1`
- `forecsen` (int) — Forecast sensitivity (1-5)
- `stormdet` (string) — Storm detection: `0` or `1`
- `stormthr` (float) — Storm risk threshold (0.5-5.0)
- `wifidelay` (int) — WiFi reconnect delay (5-300 seconds)
- `mqtten` (string) — Enable MQTT: `0` or `1`
- `mqttbrok` (string) — MQTT broker address (max 100 chars)
- `mqttport` (int) — MQTT port (1-65535)
- `mqttuser` (string) — **NEW v20.1:** MQTT username (max 50 chars)
- `mqttpass` (string) — **NEW v20.1:** MQTT password (max 100 chars)
- `mqtttop` (string) — MQTT topic prefix (max 50 chars)
- `mqttint` (int) — MQTT publish interval (1-60 minutes)
- `batlow` (float) — Low battery threshold (2.5-4.2V)
- `batcrit` (float) — Critical battery threshold (2.5-4.2V)
- `solar` (string) — Solar power mode: `0` or `1`
- `sleeptimeout` (int) — Deep sleep timeout (30-1440 minutes)

**Response:** `302` redirect to `/config`

**Example:**
```bash
curl -X POST \
  -F alt=123.4 \
  -F tu=F \
  -F bc=1.08 \
  -F tf=24 \
  -F led=1600 \
  -F lxd=1400 \
  -F lim=10 \
  -F slm=10 \
  -F pth=0.6 \
  -F pdisp=mslp \
  -F ru=mm \
  -F rtip=0.011 \
  -F rdb=150 \
  -F mdns=weatherstation1 \
  -F sds=pre2 \
  -F leafdbg=0 \
  -F leafdry=3300 \
  -F leafwet=1400 \
  -F leafweton=55.0 \
  -F leafwetoff=45.0 \
  -F etou=mm \
  -F lat=40.0 \
  -F dbg=0 \
  http://weatherstation1.local/config -i
```

---

### POST `/add`

**Description:** Adds a Wi‑Fi network to the saved list (stored in Preferences namespace `wifi`).

**Form Parameters:**
- `ssid` (string, required) — Wi-Fi SSID
- `pass` (string, required) — Wi-Fi password

**Response:** `302` redirect to `/`

**Example:**
```bash
curl -X POST \
  -F ssid=MyWiFi \
  -F pass=MyPassword \
  http://weatherstation1.local/add -i
```

---

### GET `/del?ssid=...`

**Description:** Deletes a saved Wi‑Fi network by SSID.

**Query Parameters:**
- `ssid` (string, required) — Wi-Fi SSID to delete (URL-encoded)

**Response:** `302` redirect to `/`

**Example:**
```bash
curl "http://weatherstation1.local/del?ssid=MyWiFi" -i
```

---

### POST `/reset`

**Description:** Clears the CSV log file and recreates it with the extended 24-column header.

**Response:** `302` redirect to `/`

**Example:**
```bash
curl -X POST http://weatherstation1.local/reset -i
```

---

### POST `/sleep`

**Description:** Forces the device into deep sleep immediately. Programs RTC alarm and ESP32 timer wake using configured `sleep_minutes` duration.

**Response:** `text/html` with sleep confirmation page

**Example:**
```bash
curl -X POST http://weatherstation1.local/sleep -i
```

---

### POST `/restart` and GET `/restart`

**Description:** Triggers a soft reboot (ESP.restart()).

**Response:** `text/html` with restart confirmation page (auto-redirects to `/` after 8 seconds)

**Example:**
```bash
curl -X POST http://weatherstation1.local/restart -i
# or
curl http://weatherstation1.local/restart -i
```

---

### GET `/update` (OTA)

**Description:** Provided by ElegantOTA library. Allows firmware upload via web interface with basic authentication.

**Authentication:** HTTP Basic Auth
- Default username: `weatherstation1`
- Default password: `12345678`
- **⚠️ Change these defaults before deployment!**

**Response:** HTML OTA upload interface

**Example:**
```bash
# Open in browser (will prompt for credentials)
xdg-open http://weatherstation1.local/update
```

---

## mDNS

- Service registers at `<mdnsHost>.local` (defaults to `weatherstation1.local`, configurable via `/config`)
- If mDNS fails, use the device IP printed on serial console at boot

**Example:**
```bash
ping weatherstation1.local
```

---

## Persistent Configuration (Preferences)

### Wi-Fi Configuration
- **Namespace:** `wifi`
- **Key:** `config`
- **Format:** JSON string containing array of network objects

**Structure:**
```json
{
  "networks": [
    {"ssid": "MyWiFi", "pass": "MyPassword"},
    {"ssid": "GuestWiFi", "pass": "GuestPassword"}
  ]
}
```

### Application Configuration
- **Namespace:** `app`
- **Key:** `cfg`
- **Format:** JSON string with all settings

**Fields:**
- `altitude_m` (float) — Altitude in meters
- `temp_unit` (string) — `F` or `C`
- `bat_cal` (float) — Battery calibration multiplier
- `time_12h` (bool) — 12-hour clock format
- `lux_enter_day` (float) — Enter DAY mode threshold (lux)
- `lux_exit_day` (float) — Exit DAY mode threshold (lux)
- `log_interval_min` (int) — Log interval while awake (minutes)
- `sleep_minutes` (int) — Sleep duration between wakes (minutes)
- `trend_threshold_hpa` (float) — Pressure trend threshold (hPa)
- `show_mslp` (bool) — Display MSLP (sea-level pressure, inHg) or station pressure (hPa) on dashboard
- `rain_unit` (string) — `in` or `mm`
- `rain_tip_in` (float) — Inches per bucket tip
- `rain_debounce_ms` (int) — Rain ISR debounce (ms)
- `mdns_host` (string) — mDNS hostname label
- `sds_mode` (string) — `off|pre1|pre2|pre5|cont`
- `leaf_debug` (bool) — Show raw leaf calibration values
- `leaf_adc_dry` (int) — Leaf ADC dry endpoint (0-4095)
- `leaf_adc_wet` (int) — Leaf ADC wet endpoint (0-4095)
- `leaf_wet_on_pct` (float) — Wet threshold ON (0-100%)
- `leaf_wet_off_pct` (float) — Wet threshold OFF (0-100%)
- `eto_unit` (string) — `in` or `mm`
- `latitude` (float) — Latitude in degrees (−90 to +90)
- `longitude` (float) — **NEW v20.1:** Longitude in degrees (−180 to +180)
- `timezone_offset` (int) — **NEW v20.1:** Hours offset from UTC
- `location_name` (string) — **NEW v20.1:** Friendly station name
- `timezone_string` (string) — **NEW v20.1:** POSIX timezone string with DST rules
- `debug_verbose` (bool) — Verbose serial logging
- `dashboard_refresh_rate` (int) — Dashboard auto-refresh rate in seconds (1-60)
- `show_advanced_metrics` (bool) — Display advanced meteorological calculations
- `dark_mode` (bool) — UI theme preference
- `chart_data_points` (int) — Number of historical data points in charts
- `enhanced_forecast_enabled` (bool) — Enable enhanced multi-sensor forecasting
- `forecast_sensitivity` (int) — Forecast sensitivity level (1-5)
- `storm_detection_enabled` (bool) — Enable storm detection alerts
- `storm_risk_threshold` (float) — Storm risk threshold for alerts
- `wifi_reconnect_delay` (int) — Seconds between WiFi reconnection attempts
- `mqtt_enabled` (bool) — Enable MQTT publishing
- `mqtt_broker` (string) — MQTT broker address
- `mqtt_port` (int) — MQTT broker port
- `mqtt_username` (string) — **NEW v20.1:** MQTT authentication username
- `mqtt_password` (string) — **NEW v20.1:** MQTT authentication password
- `mqtt_topic` (string) — MQTT topic prefix
- `mqtt_interval` (int) — MQTT publish interval (minutes)

Note (PubSubClient v2.8 — 2020‑05‑20):

- Added `setBufferSize()` to override `MQTT_MAX_PACKET_SIZE`
- Added `setKeepAlive()` to override `MQTT_KEEPALIVE`
- Added `setSocketTimeout()` to override `MQTT_SOCKET_TIMEOUT`
- Prevent subscribe/unsubscribe to empty topics
- ESP examples declare Wi‑Fi mode before connect
- Use `strnlen` to avoid overruns
- Supports pre‑connected `Client` objects

Reference: PubSubClient API — https://pubsubclient.knolleary.net
- `esphome_discovery_enabled` (bool) — Enable ESPHome discovery for Home Assistant integration
- `battery_low_threshold` (float) — Low battery voltage threshold
- `battery_critical_threshold` (float) — Critical battery voltage threshold
- `solar_power_mode` (bool) — Enable solar power optimizations
- `deep_sleep_timeout` (int) — Timeout before forced deep sleep (minutes)

**Structure Example:**
```json
{
  "altitude_m": 123.4,
  "temp_unit": "F",
  "bat_cal": 1.08,
  "time_12h": false,
  "lux_enter_day": 1600,
  "lux_exit_day": 1400,
  "log_interval_min": 10,
  "sleep_minutes": 10,
  "trend_threshold_hpa": 0.6,
  "show_mslp": true,
  "rain_unit": "mm",
  "rain_tip_in": 0.011,
  "rain_debounce_ms": 150,
  "mdns_host": "weatherstation1",
  "sds_mode": "pre2",
  "leaf_debug": false,
  "leaf_adc_dry": 3300,
  "leaf_adc_wet": 1400,
  "leaf_wet_on_pct": 55.0,
  "leaf_wet_off_pct": 45.0,
  "eto_unit": "mm",
  "latitude": 40.0,
  "debug_verbose": false,
  "dashboard_refresh_rate": 2,
  "show_advanced_metrics": true,
  "dark_mode": true,
  "chart_data_points": 180,
  "enhanced_forecast_enabled": true,
  "forecast_sensitivity": 3,
  "storm_detection_enabled": true,
  "storm_risk_threshold": 2.0,
  "wifi_reconnect_delay": 30,
  "mqtt_enabled": false,
  "mqtt_broker": "",
  "mqtt_port": 1883,
  "mqtt_topic": "weatherstation",
  "mqtt_interval": 5,
  "esphome_discovery_enabled": false,
  "battery_low_threshold": 3.3,
  "battery_critical_threshold": 3.0,
  "solar_power_mode": false,
  "deep_sleep_timeout": 180
}
```

---

## Function Reference

All functions are defined within the main `.ino` file. Key functions and their roles:

### Core Functions

#### `setup()`, `loop()`
Standard Arduino entry points.

- **`setup()`** — Initializes:
  - Serial console (115200 baud)
  - Status LED
  - I²C bus and sensors (BME680, VEML7700, DS3231, PCF8574)
  - SPI and SD card
  - Wi-Fi (station or AP fallback)
  - HTTP server and OTA
  - RTC with NTP sync and drift check
  - Rain/wind/leaf ISRs
  - Performs initial logging
  - Determines serve window based on wake cause
  
- **`loop()`** — Services:
  - HTTP client requests
  - Status LED updates
  - Day/night state machine
  - Periodic logging (`performLogging()`)
  - SDS011 UART frame parsing and duty cycling
  - Wind 1-hour rolling average sampling
  - Leaf wetness sampling and wet-hours accumulation
  - Daily NTP resync (02:00 local)
  - AP-stuck fallback reset (3-minute timeout)
  - Sleep scheduling in NIGHT mode

### Wi‑Fi and OTA

#### `bool connectToWifi()`
Attempts to join saved networks.

**Behavior:**
- Scans for visible networks
- Tries saved networks in order (visible first, then all)
- 15-second timeout per network
- Enables auto-reconnect on success
- Returns `true` if connected, `false` otherwise

#### `void startAPMode()`
Starts AP mode for configuration.

**Details:**
- SSID: `WeatherStation1`
- Password: `12345678`
- Blinks status LED 2 times (300ms) to indicate AP mode
- Prints AP IP to serial console

#### `void setupOTA()`
Initializes ElegantOTA on `/update` with basic auth.

**Credentials (default):**
- Username: `weatherstation1`
- Password: `12345678`

### HTTP Handlers

#### `void setupServerRoutes()`
Registers all HTTP routes on the WebServer instance.

**Routes:**
- `GET /` → `handleRoot()`
- `GET /live` → `handleLive()`
- `GET /download` → `handleDownload()`
- `GET /view-logs` → `handleViewLogs()`
- `GET /config` + `POST /config` → inline handlers
- `POST /add` → `handleAdd()`
- `GET /del` → `handleDel()`
- `POST /reset` → `handleReset()`
- `POST /sleep` → `handleSleep()`
- `POST /restart` + `GET /restart` → `restartHandler()` + inline handler
- `404` → inline handler

#### `void handleRoot()`
Renders the live dashboard HTML with dark theme.

**Features:**
- Inline CSS for dark mode (#121212 background)
- JavaScript for `/live` polling (2-second interval)
- Canvas-based line charts (180-point rolling history)
- Legend panel with metric explanations
- Wi-Fi management UI (add/delete networks)
- Action buttons (download, view logs, OTA, sleep, restart, config, clear logs)

#### `void handleLive()`
Reads sensors, computes derived metrics, returns JSON.

**Behavior:**
- Reads BME680, VEML7700, UV sensor, battery
- Computes dew point, heat index, MSLP, wet bulb temp
- Updates pressure history and trend
- Computes wind speed/gust from ISR ring buffer
- Reads wind vane direction (PCF8574)
- Computes rain rates and totals from ISR ring buffer
- Reads leaf wetness and wet-hours
- SDS011: returns averaged PM values when warm and awake; falls back to last persisted values when asleep
- Generates rich forecast with detailed metrics
- Performs single CSV append per wake cycle (guarded by `loggedThisWake` flag)
- Response payload typically < 1.5 KB JSON

#### `void handleDownload()`
Streams raw CSV file.

**Headers:**
- `Content-Disposition: attachment; filename=logs.csv`
- `Content-Type: text/csv`

#### `void handleViewLogs()`
Renders last 200 CSV rows in a styled HTML table with filters.

**Features:**
- Dark theme table (#1e1e1e background)
- Client-side JavaScript filtering (no page reload)
- Field selector, comparison type, min/max inputs, auto-range button
- URL query parameter support for direct links
- Backward-compatible with legacy CSV formats (6-column and 14-column)

#### `void handleAdd()`, `void handleDel()`
Manage Wi-Fi networks in Preferences.

- **`handleAdd()`** — Adds network to `networks` array, saves to Preferences, redirects to `/`
- **`handleDel()`** — Removes network by SSID, saves to Preferences, redirects to `/`

#### `void handleReset()`
Clears `/logs.csv` and recreates with extended 24-column header.

**Header written:**
```
timestamp,temp_f,humidity,dew_f,hi_f,pressure,pressure_trend,forecast,lux,uv_mv,uv_index,voltage,voc_kohm,mslp_inHg,rain,boot_count,pm25_ugm3,pm10_ugm3,wind_mph,wind_dir,wind_gust_mph,rain_1h,rain_today,rain_event
```

#### `void handleSleep()`
Forces device into deep sleep.

**Behavior:**
- Responds with HTML confirmation page
- Blinks status LED 3 times (200ms)
- Calls `prepareDeepSleep()` with configured `sleep_minutes` duration

#### `void restartHandler()`
Soft reboot with HTML confirmation page.

**Behavior:**
- Responds with HTML confirmation page (auto-redirects to `/` after 8 seconds)
- Delays 1.2 seconds for TCP flush
- Calls `ESP.restart()`

### Sensors, Logging, Power

#### `float readLux()`
Reads lux from VEML7700, clamps invalid values to 0.

**Returns:** Lux value (0-120000 range)

#### `float readUvMilliVolts()`
Reads GUVA-S12SD analog output, averages 16 samples, returns mV.

**Returns:** UV sensor millivolts

#### `float computeUvIndexFromMv(float uvMv)`
Crude UV index estimate from mV (linear map 50-1000 mV → 0-11).

**Returns:** UV index (0-11+)

#### `float readBatteryVolts()`
Reads battery voltage via ADC with voltage divider correction and calibration.

**Returns:** Battery voltage (V)

#### `void performLogging()`
Periodic CSV logging (scheduled by `LOG_INTERVAL_MS`).

**Behavior:**
- Only logs when `nextLogMillis` has arrived
- In DAY mode, manages SDS011 duty cycling per configured preset
- Reads BME680, UV, battery, lux
- Computes derived metrics (dew, heat index, MSLP, wet bulb)
- Computes rain rates and totals from ISR ring buffer
- Computes wind speed/gust from ISR ring buffer
- Reads wind vane direction
- Updates pressure history
- Appends CSV row to `/logs.csv` with 24 columns
- Persists last PM values to RTC memory when SDS011 is ready
- Updates `lastSdLogUnix` and `lastSdLogTime`
- Prints condensed log line to serial console

#### `void updateDayNightState()`
Debounced, averaged light-based state machine.

**Behavior:**
- Samples lux every `SAMPLE_INTERVAL_MS` (2 seconds)
- Maintains rolling average over `DWELL_MS` (30 seconds)
- Transitions MODE_NIGHT → MODE_DAY when average lux ≥ `LUX_ENTER_DAY` for full dwell
- Transitions MODE_DAY → MODE_NIGHT when average lux ≤ `LUX_EXIT_DAY` for full dwell
- On sunset (DAY → NIGHT), performs final log and enters deep sleep
- Defers decisions during initial 30-minute startup window

#### `void updateStatusLed()`
Centralized status LED behavior.

**Modes:**
- **Non-blocking pulse override:** Scheduled by `blinkStatus()`, highest precedence
- **AP mode:** Solid ON
- **DAY mode connected:** Slow blink (500ms)
- **DAY mode disconnected:** LED OFF (calm)
- **NIGHT mode connected:** Slow blink (500ms)
- **NIGHT mode disconnected:** LED OFF (calm)

#### `void blinkStatus(int times, int durationMs)`
Schedules non-blocking LED pulses handled by `updateStatusLed()`.

**Parameters:**
- `times` (int) — Number of blink cycles
- `durationMs` (int) — Duration per toggle (ms)

#### `void prepareDeepSleep(uint32_t wakeAfterSeconds)`
Programs DS3231 alarm (A1 minute match) and ESP32 timer wake, then enters deep sleep.

**Behavior:**
- If SDS011 is present and awake, sends sleep command
- If RTC is available:
  - Computes alarm time (now + wakeAfterSeconds, rounded to next minute boundary)
  - Programs DS3231 A1 alarm to fire when minute matches
  - Clears alarm flags
  - Disables 32kHz output
  - Enables ESP32 EXT0 wake on `RTC_INT_PIN` (GPIO2) going LOW
  - Stores `lastAlarmUnix` in RTC memory
- Always enables ESP32 timer wake (wakeAfterSeconds + 5s slack)
- Turns off status LED
- Enters deep sleep (never returns)

### Time, Pressure History, Forecast

#### `void loadAppConfig()`, `void saveAppConfig()`
Read/write `AppConfig` struct to/from Preferences namespace `app`.

**Config fields:**
- All persistent settings listed in "Persistent Configuration" section above

#### `void updatePressureHistory(float P_hPa, uint32_t nowUnix)`
Stores up to 13 hourly pressure samples in RTC memory.

**Behavior:**
- Circular buffer of 13 slots
- Stores one sample per hour boundary (≥3600s since last sample)
- Used by `getPressureDelta()` for trend computation

#### `bool getPressureDelta(float hours, float currentP, float* outDelta)`
Retrieves pressure delta vs a past sample.

**Parameters:**
- `hours` (float) — Lookback period (e.g., 3.0, 6.0, 12.0)
- `currentP` (float) — Current pressure (hPa)
- `outDelta` (float*) — Output delta (hPa)

**Returns:** `true` if delta computed, `false` if insufficient history

#### `const char* classifyTrendFromDelta(float delta)`
Maps pressure delta to trend string.

**Thresholds:** Uses `appCfg.trendThresholdHpa` (default: 0.6 hPa)

**Returns:** `Rising|Falling|Steady`

#### `const char* zambrettiSimple(float mslp_hPa, const char* trend)`
Simplified Zambretti forecast label (legacy function, still available).

**Returns:** `Settled Fine|Fine|Fair|Change|Unsettled|Rain`

#### `const char* enhancedZambrettiAdv(float mslp_hPa, const char* trend, float windMph, float hum)`
**NEW:** Enhanced Zambretti algorithm with wind and humidity integration.

**Returns:** 15+ distinct forecast states including:
- Storm conditions: `Severe Storm|Storm / Heavy Rain|Heavy Showers|Rain / Unsettled`
- Improving: `Very Settled / Clear|Settled / Fine|Fair / Clearing|Improving`
- Stable: `Variable / Partly Cloudy|Unsettled|Stormy`

#### `const char* generalForecastFromSensors(...)`
**ENHANCED:** Advanced multi-sensor forecast with deep sensor fusion.

**Algorithm:**
- Combines pressure trends, wind speed/gusts, humidity, temperature, dew point, UV index, rain rate, and light levels
- Frontal detection (cold front: falling P + rising wind; warm front: falling P + high humidity)
- Severe weather scoring (0-3 scale based on pressure drop rate, wind gusts, atmospheric instability)
- Storm risk assessment with multiple indicators

**Returns:** 40+ distinct forecast states including:
- Immediate conditions: `Heavy Rain|Raining|Light Rain`
- Warnings: `Severe Risk|Thunderstorms Possible|Front Approaching|Major Storm`
- Detailed conditions: `Heavy Showers Soon|Rain Likely|Rain Developing|Cloudy / Chance Rain`
- Improving: `Very Clear & Sunny|Clear & Bright|Clearing / Sunny|Slowly Improving`
- Settled: `Clear & Dry|Sunny / Settled|Fair / Settled|Humid but Stable`
- Variable: `Partly Cloudy|Variable / Bright Spells|Cloudy / Stable|Damp / Overcast`

#### Supporting Forecast Functions (NEW)
- `bool approachingFront(...)` — Detects cold/warm fronts using multi-sensor signals
- `int severePotential(...)` — Scores severe weather risk (0=None, 1=Low, 2=Moderate, 3=High)
- `const char* analyzeTempTrend(...)` — Temperature trend from dew point spread
- `const char* stormRiskLabel(...)` — Enhanced storm risk with 7 levels

#### `String buildForecastDetail(...)`
**ENHANCED:** Comprehensive multi-line forecast with all available metrics and warnings.

**Includes:**
- **Storm warnings** (Severe/High/Possible risk with ⚠️ emoji alerts)
- **Frontal passage alerts** ("Front Approaching" when conditions detected)
- Air quality category (from PM2.5: Good/Moderate/USG/Unhealthy/Very Unhealthy/Hazardous)
- UV risk category (Low/Moderate/High/Very High/Extreme)
- Enhanced wind descriptor (Calm/Light/Breezy/Windy/Very Windy) with:
  - Wind speed and direction
  - Gust warnings (⚠️ if >25 mph)
- Humidity comfort notes (Very Humid/Humid/Dry)
- Pressure tendency (Rapid P Drop/Falling P/Rising P)
- Rain status (Raining Now) or risk (High/Moderate/Low)
- Rain totals (1h/Today/Event in configured unit)
- Heat risk (Low/Caution/Extreme)
- Storm risk (Minimal/Low/Slight/Possible/High/Severe)
- Fog/Frost warnings (when temperature and humidity conditions met)
- Rain 3/6/12h trend arrows (↑ rising, → steady, ↓ falling)

### Derived Meteorology Utilities

#### `float computeDewPointC(float Tc, float RH)`
Dew point in °C (Magnus formula).

**Example:** `computeDewPointC(25.0f, 60.0f) ≈ 16.7`

#### `float computeHeatIndexF(float Tf, float RH)`
Heat index in °F (Rothfusz regression).

**Example:** `computeHeatIndexF(86.0f, 60.0f) ≈ 94.0`

#### `float computeMSLP_hPa(float P_hPa, float Tc, float altitudeM)`
Mean sea-level pressure in hPa (barometric formula).

**Example:** `computeMSLP_hPa(1013.0f, 25.0f, 100.0f) ≈ 1025.0`

#### `float computeWetBulbC(float Tc, float RH)`
Approximate wet-bulb temperature in °C (Stull 2011 approximation).

**Example:** `computeWetBulbC(25.0f, 60.0f) ≈ 19.0`

#### Additional utilities:
- `float computeAbsoluteHumidity_gm3(float Tc, float RH)` — Absolute humidity (g/m³)
- `float computeMixingRatio_gPerKg(float Tc, float RH, float P_hPa)` — Mixing ratio (g/kg)
- `float computeSpecificHumidity_gPerKg(float Tc, float RH, float P_hPa)` — Specific humidity (g/kg)
- `float computeVpd_kPa(float Tc, float RH)` — Vapor pressure deficit (kPa)
- `float computeAirDensity_kgm3(float Tc, float RH, float P_hPa)` — Air density (kg/m³)
- `float computeHumidexC(float Tc, float RH)` — Humidex (°C, Canadian comfort index)
- `float computeWindChillC(float Tc, float windSpeedKmh)` — Wind chill (°C)
- `float computeWbgtShadeC(float Tc, float RH)` — WBGT shade (°C, heat stress index)

### SD Utilities

#### `uint64_t computeSdUsedBytes()`
Estimates used bytes on SD card via recursive directory traversal.

**Returns:** Used bytes (uint64_t)

#### `uint64_t computeSdUsedBytesRecursive(File dir)`
Recursive helper for `computeSdUsedBytes()`.

### Rain Gauge ISR

#### `void IRAM_ATTR rainIsr()`
Interrupt service routine for tipping bucket rain gauge.

**Behavior:**
- Debounced (default: 150ms, configurable via `rain_debounce_ms`)
- Increments `rainTipCount`
- Stores timestamp in 128-entry ring buffer
- Protected by critical section (`portENTER_CRITICAL_ISR/EXIT_CRITICAL_ISR`)

#### `void updateRainAccumulators()`
Maintains daily and event rain totals.

**Behavior:**
- Rolls `rainTodayTips` at local midnight (uses `tm_yday`)
- Resets `rainEventTips` after ≥6h dry gap
- Copies ISR tip count atomically and updates `rainProcessedTips`
- Updates `lastTipUnixPersist`

### Wind Anemometer ISR

#### `void IRAM_ATTR windIsr()`
Interrupt service routine for Hall anemometer pulses.

**Behavior:**
- Debounced (5ms)
- Stores timestamp in 128-entry ring buffer
- Protected by critical section

#### `float computeWindGustMphLast10Min()`
Computes wind gust using classic NOAA method: highest 5-second sample rate in last 10 minutes.

**Returns:** Wind gust (mph), 0.0 if < 2 pulses

### Wind Vane (PCF8574) Helpers

#### `String wvReadDirection(int* outIdx)`
Reads wind vane direction from PCF8574.

**Behavior:**
- Performs majority vote of 3 quick samples (300µs apart) for stability
- Decodes 8-bit mask to direction:
  - Exactly 1 bit active → return single direction (N, NE, E, SE, S, SW, W, NW)
  - 2 adjacent bits → return first direction in clockwise order
  - Dead zone (0 bits or 3+ bits) → return last valid direction or `--`
- Updates `windDirLastMask`, `windDirIdx`, `lastValidWindDir`

**Returns:** Direction string (N|NE|E|SE|S|SW|W|NW|--|N/A)

---

## Usage Notes

- The device records a boot event immediately after first `performLogging()` in `setup()`
- `last_sd_log` in `/live` is formatted per the 12/24-hour preference; raw unix timestamp is persisted in `RTC_DATA_ATTR`
- If DS3231 is unavailable, wake is timer-only and `rtc_ok` will be `false`
- LED behavior is calm by default when disconnected; use `blinkStatus()` to signal events
- SDS011 duty cycling extends sensor lifespan; typical lifetime is ~8000 hours continuous or ~2 years with 20% duty cycle
- Wind gust computation requires at least 2 pulses in any 5-second window; calm periods report 0.0
- Leaf wetness calibration requires field adjustment of `LEAF_ADC_DRY` and `LEAF_ADC_WET` constants; enable `leaf_debug` to see raw values on dashboard

---

## Integration Recipes

### Polling telemetry
Fetch `/live` every 2–10 seconds and ingest JSON:

```bash
while true; do
  curl -s http://weatherstation1.local/live | jq '.temp_f, .hum, .pressure'
  sleep 10
done
```

### Batch download
GET `/download` daily and append to a data lake:

```bash
# Cron job: 0 2 * * * /path/to/download.sh
curl -LOJ http://weatherstation1.local/download
mv logs.csv "logs_$(date +%Y%m%d).csv"
```

### Headless config
POST `/config` after flashing to set altitude and units:

```bash
curl -X POST \
  -F alt=250 \
  -F tu=C \
  -F ru=mm \
  http://weatherstation1.local/config
```

### Wi‑Fi provisioning
POST `/add` repeatedly for multiple SSIDs:

```bash
for net in "Home:password1" "Office:password2"; do
  IFS=: read ssid pass <<< "$net"
  curl -X POST -F ssid="$ssid" -F pass="$pass" http://weatherstation1.local/add
done
```

### Force sleep
POST `/sleep` to immediately re-enter deep sleep after a maintenance session:

```bash
curl -X POST http://weatherstation1.local/sleep
```

### Prometheus exporter (example)
```python
#!/usr/bin/env python3
import requests
import time
from prometheus_client import start_http_server, Gauge

# Metrics
temp_gauge = Gauge('weather_temperature_fahrenheit', 'Temperature in Fahrenheit')
hum_gauge = Gauge('weather_humidity_percent', 'Relative Humidity')
pressure_gauge = Gauge('weather_pressure_hpa', 'Station Pressure')
# ... define more gauges for other fields

def fetch_and_update():
    r = requests.get('http://weatherstation1.local/live')
    data = r.json()
    temp_gauge.set(data['temp_f'])
    hum_gauge.set(data['hum'])
    pressure_gauge.set(data['pressure'])
    # ... update more gauges

if __name__ == '__main__':
    start_http_server(9100)  # Prometheus scrapes :9100/metrics
    while True:
        fetch_and_update()
        time.sleep(10)
```

---

## Security Considerations

- **OTA:** Protected by static basic auth. Rotate credentials in `setupOTA()` before deployment.
- **AP Fallback:** Default SSID `WeatherStation1`, password `12345678`. Change in `startAPMode()` before deployment.
- **HTTP:** All endpoints use plain HTTP. Run on a trusted LAN; do not expose to public internet.
- **Secrets:** Do not log credentials or sensitive data. Avoid including secrets in `/live` JSON or CSV.

---

## Troubleshooting

### Common Issues

**mDNS fails:**
- Use the printed IP from serial console or check router DHCP leases
- Verify your network supports multicast DNS (some guest networks block it)

**SD mount fails:**
- Check wiring (CS=5, SCK=12, MISO=13, MOSI=11)
- Verify card is FAT32 formatted (exFAT not supported)
- Try a different microSD card (some older/slower cards don't work)

**BME680/VEML7700 not detected:**
- Check I²C wiring (SDA=8, SCL=9 on ESP32-S3; SDA=21, SCL=22 on classic ESP32)
- Verify I²C addresses: BME680 at 0x76 or 0x77; VEML7700 at 0x10
- Use `i2cdetect` tool to scan addresses: add I2C scanner sketch

**RTC is absent:**
- Device will use timer-only wakes
- `/live` will show `rtc_ok: false`
- Timestamp accuracy depends on NTP sync frequency

**SDS011 shows no data:**
- Check UART wiring (RX=16, TX=17) and sensor power (5V)
- Verify `sds_mode` is not `off`
- Wait 30 seconds for warm-up after wake
- Look for `[SDS011]` debug messages in serial console when `debug_verbose=true`

**Wind vane shows N/A:**
- Verify PCF8574 I²C address (0x20-0x27) using I2C scanner
- Check wiring: PCF8574 needs pull-ups enabled (sketch writes 0xFF to set pins as inputs)
- Ensure magnet in vane is strong enough to trigger reed switches

**Leaf wetness readings seem inverted:**
- Adjust `LEAF_ADC_DRY` (raw ADC when dry, typically ~3300)
- Adjust `LEAF_ADC_WET` (raw ADC when wet, typically ~1400)
- Enable `leaf_debug` in `/config` to see raw values on dashboard
- Recompile and upload after changing calibration constants

**Battery voltage is incorrect:**
- Adjust `bat_cal` multiplier in `/config` (compare ADC reading to multimeter)
- Verify voltage divider: R1=100k, R2=100k (2:1 ratio)

**Time is wrong:**
- Check timezone in `configTzTime()` call (default: EST5EDT)
- Verify NTP server is reachable (`pool.ntp.org`)
- If RTC present, check battery (CR2032) for DS3231

---

## API Change Log

### v20.1-improved (Current)
- **Code Quality & Reliability Improvements**
  - Added configurable `timezone_string` field to AppConfig (global timezone support)
  - Added `location_name`, `longitude`, `timezone_offset` fields for better localization
  - Added `mqtt_username` and `mqtt_password` for authenticated MQTT connections
  - Implemented MQTT exponential backoff (1s→30s) to prevent broker abuse
  - Enhanced SD card error handling with comprehensive logging
  - Optimized stack allocations (512→8 bytes) in rain calculations
  - Added named constants for all magic numbers (ADC samples, timeouts, delays)
  - Applied const correctness to 30+ function signatures
  - Code quality improved from 8.5 → 9.5/10
  - **API Compatibility**: All endpoints unchanged, fully backward compatible

### v20.0-modular
- **Major Architectural Refactoring**
  - Refactored monolithic v19 (4,705 lines) into 7 clean modules (15 files)
  - 100% feature parity maintained (57 functions, 11 endpoints, 30 CSV columns)
  - No API changes, drop-in replacement for v19

### v19.2
- **Enhanced Configuration System**
  - Added Dashboard Settings section with customizable refresh rate, advanced metrics display, dark mode theme, and chart data points
  - Added Enhanced Forecasting section with multi-sensor forecasting toggle, sensitivity levels, storm detection, and risk thresholds
  - Added WiFi & MQTT Integration section with reconnection delay, MQTT broker configuration, and publish intervals
  - Added Battery & Power Management section with low/critical thresholds, solar power mode, and deep sleep timeout
  - Expanded configuration page with 4 new organized sections and comprehensive descriptive help text
  - All new settings persist across reboots and support backward compatibility

### v19.1
- **VEML7700 Lux Reading Enhancement**
  - Fixed lux reading limitation with non-linear correction for high light levels (>20k lux)
  - Extended sensor range to full 120k lux capability
  - Fixed compilation errors with raw literal string syntax

### v19.0
- **Enhanced Dashboard Layout with 3-Column Forecast Tile**
  - Expanded forecast detail tile to span 3 columns (previously 2) for comprehensive weather information display
  - Reorganized dashboard layout with improved tile grouping:
    - Row 1: Weather Forecast (3-column span for detailed outlook)
    - Row 2: Key Metrics (Pressure Trend, Temperature, Humidity)
    - Row 3: Core Environmental Sensors (MSLP/Station Pressure, Dew Point)
    - Row 4: Wind & Rain metrics
    - Row 5: Light & UV sensors
    - Row 6: Air Quality (PM2.5, PM10, VOC)
    - Row 7: Comfort metrics (Heat Index, Wet Bulb)
    - Row 8: Agriculture (Leaf Wetness, ETo)
    - Row 9: System Status
  - Enhanced forecast detail display with more space for comprehensive multi-line weather information

### v18.3
- **Enhanced Multi-Sensor Forecast System**
  - Added `enhancedZambrettiAdv()` with 40+ distinct forecast states
  - Integrated wind speed/gusts into forecast generation
  - Added frontal passage detection (cold/warm front indicators)
  - Added severe weather scoring (0-3 scale with multiple criteria)
  - Enhanced storm risk levels: Minimal/Low/Slight/Possible/High/Severe
  - Added temperature trend analysis using dew point spread
- **Dashboard Improvements**
  - Reorganized tile layout with forecast at top-left (2-column span)
  - Logical grouping: Core sensors, Wind & Rain, Light & UV, Air Quality, Comfort, Agriculture, System
  - Enhanced forecast detail with storm warnings (⚠️), frontal alerts, and comprehensive outlook
- **Configuration Page Redesign**
  - Organized settings into 7 logical sections: System, Power & Timing, Pressure & Forecast, Rain Gauge, Air Quality, Leaf Wetness, ETo, Debug
  - Added visual section separators with emoji icons and colored headers
  - Improved layout with section backgrounds and better spacing
- **Pressure Display Toggle**
  - Added `show_mslp` config option to choose between MSLP (sea-level, inHg) or station pressure (hPa)
  - Dashboard conditionally shows one pressure tile based on user preference
  - Legend dynamically updates to explain selected pressure metric
- **Legend Enhancements**
  - Removed CO₂ reference (sensor not present)
  - Reorganized into visual categories matching dashboard layout
  - Added clearer, more concise descriptions

### v18.2
- Added FAO-56 reference evapotranspiration (ETo) with Penman-Monteith and Hargreaves-Samani methods
- Added ETo to CSV (hourly rate + daily cumulative, now 30 columns total)
- Added ETo to `/live` JSON (`eto_hourly_mm`, `eto_hourly_in`, `eto_daily_mm`, `eto_daily_in`, `eto_unit`)
- Added ETo config options (`eto_unit`, `latitude`)
- Added runtime-configurable leaf wetness calibration (`leaf_adc_dry`, `leaf_adc_wet`, `leaf_wet_on_pct`, `leaf_wet_off_pct`)
- Enhanced leaf debug display to show all calibration values and current raw ADC
- Added leaf wetness to CSV (raw ADC, %, wet boolean, wet hours, now 28+ columns)

### v18.1
- Added wind gust, rain totals (1h/today/event) to CSV (24 columns)
- Added leaf wetness support (`leaf_pct`, `leaf_wet`, `leaf_wet_hours_today`)
- Added `forecast_detail` with rich multi-line forecast
- Added `storm_risk`, `aqi_category` booleans to `/live`
- Added `wind_avg_mph_1h` (rolling 1-hour average wind speed)
- Added `wind_vane_ok` flag and dead-zone handling
- Added `sds_auto_sleep_ms_left` for duty cycle visibility
- Added `leaf_debug` config option for field calibration
- Added `rain_debounce_ms` config option
- Changed CSV header to 24 columns (backward-compatible read)
- Improved `/view-logs` HTML rendering (dark theme, better filters)

### v18.0
- Initial release with extended CSV (14-20 columns)
- Added SDS011 PM2.5/PM10 support with duty cycling
- Added wind speed/direction support
- Added UV index support
- Removed BSEC2/CO2 (switched to Adafruit_BME680 library)

---

## Appendix: Example Configurations

### High-altitude mountain station
```bash
curl -X POST \
  -F alt=2500 \
  -F tu=C \
  -F ru=mm \
  -F lim=5 \
  -F slm=15 \
  http://weatherstation1.local/config
```

### Urban air quality monitor (PM2.5 focus)
```bash
curl -X POST \
  -F sds=cont \
  -F lim=1 \
  http://weatherstation1.local/config
```

### Battery-optimized (solar powered)
```bash
curl -X POST \
  -F sds=off \
  -F lim=30 \
  -F slm=30 \
  -F led=2000 \
  -F lxd=1800 \
  http://weatherstation1.local/config
```

### Agriculture/greenhouse (leaf wetness + humidity + ETo)
```bash
curl -X POST \
  -F leafdbg=1 \
  -F lim=5 \
  -F etou=mm \
  -F lat=35.5 \
  http://weatherstation1.local/config
```

### Irrigation scheduling (ETo-based)
```bash
# Set your location's latitude for accurate solar radiation estimation
curl -X POST \
  -F lat=34.05 \
  -F etou=in \
  -F lim=60 \
  http://weatherstation1.local/config

# Monitor daily ETo for smart irrigation
curl -s http://weatherstation1.local/live | jq '.eto_daily_in, .rain_today_in'
```

---

**Document Version:** 2.1 (2025-10-11)  
**Sketch Version:** WeatherStationv20_modular v20.1-improved (modular + optimized)  
**Author:** Weather-Station-1 contributors  
**License:** API.md is licensed under CC BY-4.0
