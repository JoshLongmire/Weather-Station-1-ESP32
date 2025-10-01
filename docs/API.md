### ESP32 Weather Station – Public API and Function Reference

This document describes the HTTP endpoints, persisted configuration, CSV schema, and the major functions exported in the current Arduino sketch (`WaetherStation08_24_25_v18/WaetherStation08_24_25_v18.ino`). It also includes examples and integration guidance.

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
- Storage: SD card (`/logs.csv`)
- Time: DS3231 RTC (preferred) with daily NTP syncing and drift correction
- Connectivity: Wi‑Fi STA with AP fallback, mDNS
- OTA: ElegantOTA at `/update` (basic auth)
- Modes: DAY (stay awake, periodic logs) / NIGHT (short serve window, deep sleep)

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

### Header (extended v18+, includes wind gust, rain totals):
```
timestamp,temp_f,humidity,dew_f,hi_f,pressure,pressure_trend,forecast,lux,uv_mv,uv_index,voltage,voc_kohm,mslp_inHg,rain,boot_count,pm25_ugm3,pm10_ugm3,wind_mph,wind_dir,wind_gust_mph,rain_1h,rain_today,rain_event
```

### Column descriptions (24 columns total):
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

### Example row:
```
2025-01-01 15:42:17,72.8,43.2,50.3,73.9,1013.62,Steady,Fair,455.0,320,3.2,4.07,12.5,30.10,0.28,123,8.5,12.1,3.4,NE,7.8,0.12,0.34,0.34
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

**Description:** Returns the live dashboard HTML with dark theme, cards, real-time charts (180-point rolling history), and Wi‑Fi management UI.

**Features:**
- Live sensor readings with 2-second auto-refresh
- Canvas-based line charts for temperature, humidity, pressure, MSLP, lux, UV, battery, dew point, heat index, wet bulb, VOC, PM2.5/PM10, rain rate, wind speed, wind average
- Estimated battery % (Li-ion curve) in battery card header
- Wi-Fi signal strength bars (▂▄▅▆▇) in RSSI card
- Buttons/links: Download CSV, View Logs, OTA Update, Force Sleep, Restart Device, Config Settings, Clear Logs
- Legend panel explaining all metrics and units

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
- `forecast` (string) — Simple Zambretti-style label: `Settled Fine|Fine|Fair|Change|Unsettled|Rain`
- `general_forecast` (string) — Rich forecast considering pressure, rain, UV, humidity: e.g., `Raining now`, `Rain likely`, `Improving / Fair`, `High UV / Sunny`, etc.
- `forecast_detail` (string) — Detailed multi-line forecast with:
  - Air quality category (Good/Moderate/USG/Unhealthy/Very Unhealthy/Hazardous)
  - UV risk (Low/Moderate/High/Very High/Extreme)
  - Wind descriptor (Calm/Light/Breezy/Windy/Very Windy) with speed, direction, gust
  - Humidity note (Humid/Dry)
  - Falling pressure warning
  - Rain risk (Low/Moderate/High/Now)
  - Rain totals (1h/Today/Event in configured unit)
  - Heat risk (Low/Caution/Extreme)
  - Storm risk (Low/Slight/Possible)
  - Fog/Frost risk (when conditions met)
  - Rain 3/6/12h trend arrows (↑ rising, → steady, ↓ falling)
- `storm_risk` (bool) — True when storm risk is not "Low"
- `aqi_category` (string) — Air quality category from PM2.5 (Good/Moderate/USG/Unhealthy/Very Unhealthy/Hazardous)

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

**Displayed Fields:**
- `altitude_m` (float) — Altitude in meters for MSLP calculation
- `temp_unit` (select: F/C) — Temperature display preference
- `bat_cal` (float) — Battery voltage calibration multiplier (default: 1.08)
- `time_12h` (select: 12/24) — Clock format
- `lux_enter_day` (float) — Light threshold to enter DAY mode (default: 1600 lux)
- `lux_exit_day` (float) — Light threshold to exit DAY mode (default: 1400 lux)
- `log_interval_min` (int) — CSV logging interval while awake (1-1440 minutes, default: 10)
- `sleep_minutes` (int) — Deep sleep duration between wakes (1-1440 minutes, default: 10)
- `trend_threshold_hpa` (float) — Pressure delta threshold for trend classification (0.1-5.0 hPa, default: 0.6)
- `rain_unit` (select: mm/in) — Rain rate unit for CSV/UI
- `rain_tip_in` (float) — Inches per tipping bucket tip (0.001-0.1, default: 0.011)
- `rain_debounce_ms` (int) — ISR debounce window in milliseconds (50-500, default: 150)
- `mdns_host` (string) — mDNS hostname label (no `.local` suffix)
- `sds_mode` (select) — SDS011 duty preset:
  - `off` — Keep sensor asleep
  - `pre1` — Wake 1 minute before each log
  - `pre2` — Wake 2 minutes before each log (default)
  - `pre5` — Wake 5 minutes before each log
  - `cont` — Continuous operation while awake
- `leaf_debug` (select: Off/On) — Show raw LEAF_ADC_DRY/WET values on dashboard for field calibration
- `debug_verbose` (select: No/Yes) — Enable verbose serial logging

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
- `ru` (string) — Rain unit: `mm` or `in`
- `rtip` (float) — Rain tip size (0.001-0.1 inches)
- `rdb` (int) — Rain debounce (50+ ms)
- `mdns` (string) — mDNS hostname label
- `sds` (string) — SDS011 mode: `off|pre1|pre2|pre5|cont`
- `leafdbg` (string) — Leaf debug: `0` or `1`
- `dbg` (string) — Verbose debug: `0` or `1`

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
  -F ru=mm \
  -F rtip=0.011 \
  -F rdb=150 \
  -F mdns=weatherstation1 \
  -F sds=pre2 \
  -F leafdbg=0 \
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
- `rain_unit` (string) — `in` or `mm`
- `rain_tip_in` (float) — Inches per bucket tip
- `rain_debounce_ms` (int) — Rain ISR debounce (ms)
- `mdns_host` (string) — mDNS hostname label
- `sds_mode` (string) — `off|pre1|pre2|pre5|cont`
- `leaf_debug` (bool) — Show raw leaf calibration values
- `debug_verbose` (bool) — Verbose serial logging

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
  "rain_unit": "mm",
  "rain_tip_in": 0.011,
  "rain_debounce_ms": 150,
  "mdns_host": "weatherstation1",
  "sds_mode": "pre2",
  "leaf_debug": false,
  "debug_verbose": false
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
Simplified Zambretti forecast label.

**Returns:** `Settled Fine|Fine|Fair|Change|Unsettled|Rain`

#### `const char* generalForecastFromSensors(...)`
Rich general forecast considering pressure, trend, humidity, temperature, rain rate, lux, UV.

**Returns:** Concise forecast string like:
- `Raining now`
- `Rain likely`
- `Unsettled / Showers`
- `Cloudy / Chance of rain`
- `Improving / Fair`
- `Sunny / High UV`
- `Humid but fair`
- `Neutral`

#### `String buildForecastDetail(...)`
Detailed multi-line forecast with all available metrics.

**Includes:**
- Air quality category (from PM2.5)
- UV risk category
- Wind descriptor (Calm/Light/Breezy/Windy/Very Windy) with speed, direction, gust
- Humidity note (Humid/Dry)
- Falling pressure warning
- Rain risk (Low/Moderate/High/Now)
- Rain totals (1h/Today/Event)
- Heat risk (Low/Caution/Extreme)
- Storm risk (Low/Slight/Possible)
- Fog/Frost warnings
- Rain 3/6/12h trend arrows (↑↓→)

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

### v18.1 (Current)
- Added wind gust, rain totals (1h/today/event) to CSV (now 24 columns)
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

### Agriculture/greenhouse (leaf wetness + humidity)
```bash
curl -X POST \
  -F leafdbg=1 \
  -F lim=5 \
  http://weatherstation1.local/config
```

---

**Document Version:** 1.0 (2025-01-01)  
**Sketch Version:** WaetherStation08_24_25_v18.ino  
**Author:** Weather-Station-1 contributors  
**License:** API.md is licensed under CC BY-4.0
