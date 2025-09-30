### ESP32 Weather Station – Public API and Function Reference

This document describes the HTTP endpoints, persisted configuration, CSV schema, and the major functions exported in the current Arduino sketch (`WaetherStation08_24_25_v18/WaetherStation08_24_25_v18.ino`). It also includes examples and integration guidance.

## Overview
- Device: ESP32-based weather station (tested on ESP32‑S3 — Lonely Binary ESP32‑S3 Dev Board, 16MB Flash / 8MB PSRAM)
- Sensors: BME680 (temperature, humidity, pressure, gas resistance), VEML7700 (ambient light), optional UV analog (GUVA‑S12SD), optional SDS011 (PM2.5/PM10), optional Hall anemometer (wind)
- Storage: SD card (`/logs.csv`)
- Time: DS3231 RTC (preferred) and NTP syncing
- Connectivity: Wi‑Fi STA with AP fallback, mDNS
- OTA: ElegantOTA at `/update` (basic auth)
- Modes: DAY (stay awake, periodic logs) / NIGHT (short serve window, deep sleep)

## Pins, Interfaces, and Constants
ESP32‑S3 (Lonely Binary) default mapping in this sketch:
- I²C SDA: 8, I²C SCL: 9
- SD: CS=5, SCK=12, MISO=13, MOSI=11
- Battery ADC pin: 4 (voltage divider R1=100k, R2=100k)
- Rain gauge reed switch: 18 (to GND)
- DS3231 interrupt: `RTC_INT_PIN = GPIO_NUM_2`
- Status LED: 37
- Hall anemometer (wind): 7
- UV analog (GUVA‑S12SD): 6
- BME680 I²C addresses tried: `0x76`, then `0x77`

Timing and mode constants:
- `DEEP_SLEEP_SECONDS = 600` (10 minutes)
- Initial serve window after cold boot: `UPTIME_STARTUP = 1800` seconds
- Subsequent serve windows after wake: `UPTIME_CONFIG = 120` seconds
- Logging cadence while awake: `LOG_INTERVAL_MS = 600000` (10 minutes)
- Day/Night light thresholds: enter DAY ≥ 1600 lux; exit DAY ≤ 1400 lux, dwell 30 s

## CSV Log Schema
File: `/logs.csv`

Header created by Reset (extended v18+, CO₂ removed). As of v18.1, wind gust and rain totals are appended at the end:
```
timestamp,temp_f,humidity,dew_f,hi_f,pressure,pressure_trend,forecast,lux,uv_mv,uv_index,voltage,voc_kohm,mslp_inHg,rain,boot_count,pm25_ugm3,pm10_ugm3,wind_mph,wind_dir,wind_gust_mph,rain_1h,rain_today,rain_event
```

Example row (units: temp °F, pressure hPa, MSLP inHg; rain columns use the selected unit at write time):
```
2025-01-01 15:42:17,72.8,43.2,50.3,73.9,1013.62,Steady,Fair,455.0,320,3.2,4.07,12.5,30.10,0.28,123,8.5,12.1,3.4,NE,7.8,0.12,0.34,0.34
```

Notes:
- Temperature values in CSV are in °F.
- Pressure is in hPa.
- `pressure_trend` is one of `Rising|Falling|Steady`.
- `forecast` is a simplified label derived from MSLP, trend, humidity, temp, rain rate, and lux.
- `voc_kohm` is derived from BME680 gas resistance in kΩ.
- `rain` column is written in mm/h or in/h depending on the current unit setting.
- `wind_gust_mph` is computed as the highest 5‑second sample rate observed in the last 10 minutes.
- `rain_1h`, `rain_today`, and `rain_event` are totals in the same unit as `rain` (mm or inches) when the row was written.
- Legacy logs (pre‑v18) used a 14‑column header; new columns were added later. When you Clear Logs via `/reset`, the extended header above is written.
 - Additional `/live`-only fields like `forecast_detail`, `aqi_category`, `wind_avg_mph_1h`, `storm_risk`, and `sds_auto_sleep_ms_left` support richer dashboards but are not in the CSV.

## HTTP API
Base: device IP (e.g., `http://192.168.1.50`) or mDNS `http://<mdnsHost>.local` (configurable in `/config`).

### GET `/`
- Returns the live dashboard HTML with cards, charts, and Wi‑Fi management UI.
- Includes buttons/links for downloading logs, viewing logs, starting OTA, forcing sleep, restarting, and opening the Config page.

### GET `/live`
- Returns current sensor readings and diagnostics as JSON.

Example:
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
  "forecast_detail": "Air: Good | UV: High | Wind: Light (3 mph) | Rain 3/6/12h: →/→/→",
  "aqi_category": "Good",
  "storm_risk": false,
  "last_sd_log": "2025-01-01 15:40:00"
}
```

- Units and notes:
  - Temperature fields include both °F and °C; `temp_unit` indicates UI preference.
  - Pressure includes station pressure (`pressure`, hPa) and MSLP (`mslp_hPa`, `mslp_inHg`).
  - Battery is pack voltage (single-cell Li‑ion) in volts.
  - Rain rate is provided in mm/h and in/h; `rain_unit` indicates current CSV/UI unit. Forecast detail also shows a compact rain trend indicator for 3/6/12 hours using arrows (↑ rising, → steady, ↓ falling).
  - Wind speed is reported in multiple units; `wind_mph` is commonly used in CSV. `wind_avg_mph_1h` is a rolling 1‑hour average sampled once per minute and is used in `forecast_detail` when available.
  - Wind direction uses an 8‑point compass: `N, NE, E, SE, S, SW, W, NW`. Fields: `wind_dir` (text), `wind_dir_idx` (0..7), `wind_vane_ok` (bool).
  - UV fields include raw sensor millivolts (`uv_mv`) and an approximate index (`uv_index`).
  - `aqi_category` is derived from PM2.5 for a simple dashboard label.
  - `storm_risk` is a boolean derived from pressure, trend, wind, UV, and humidity; true indicates non‑low storm risk.
  - When SDS011 is duty-cycled, `sds_auto_sleep_ms_left` shows milliseconds until auto-sleep during the 2‑minute window.

cURL example:
```bash
curl http://WeatherStation1.local/live | jq .
```

### GET `/download`
- Streams the raw CSV file for download.

```bash
curl -LOJ http://WeatherStation1.local/download
```

### GET `/view-logs`
- Renders the last N rows of the CSV in a styled HTML table with client-side filters.
- Client-side controls include field selector (Temp, Hum, Dew, Heat Index, Pressure, Lux, Battery, VOC, MSLP, Rain, Boot Count, PM2.5/PM10, Wind, Wind Gust, Rain 1h, Rain Today, Rain Event), comparison type, and Min/Max with Auto Range.
- Query helper parameters (optional): `field`, `type`, `min`, `max`.
  - Example: `?field=temp&type=between&min=60&max=80`.

### GET `/config`
- Returns a simple HTML form to view/change persistent settings.
- Fields:
  - `altitude_m` (float) — affects MSLP calculation
  - `temp_unit` (`F` or `C`) — UI formatting preference
  - `bat_cal` (float) — multiplier for battery calibration
  - `time_12h` (bool via form select: `12` → true, `24` → false)
  - Light thresholds and timing:
    - `lux_enter_day` — enter DAY mode; default 1600
    - `lux_exit_day` — enter NIGHT mode; default 1400
    - `log_interval_min` — CSV cadence while awake; default 10
    - `sleep_minutes` — deep sleep duration between wakes; default 10
    - `trend_threshold_hpa` — pressure Δ threshold; default 0.6
  - `rain_unit` (`mm` or `in`) — unit used for rain rate in CSV/UI
  - `rain_tip_in` (float) — inches per bucket tip for accumulation; default 0.011
  - `mdns_host` (string) — mDNS hostname label (no `.local`)
  - `sds_mode` (`off|pre1|pre2|pre5|cont`) — SDS011 duty preset
  - `debug_verbose` (`0|1`) — verbose serial logging

### POST `/config`
- Saves settings to Preferences namespace `app`.

Example:
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
  -F mdns=weatherstation1 \
  -F sds=pre2 \
  -F dbg=0 \
  http://WeatherStation1.local/config -i
```

### POST `/add`
- Adds a Wi‑Fi network to saved list.
- Body fields: `ssid`, `pass`.

```bash
curl -X POST -F ssid=MyWiFi -F pass=MyPassword http://WeatherStation1.local/add -i
```

### GET `/del?ssid=...`
- Deletes a saved Wi‑Fi network by SSID.

```bash
curl "http://WeatherStation1.local/del?ssid=$(python3 - <<'PY'\nimport urllib.parse; print(urllib.parse.quote('MyWiFi'))\nPY)" -i
```

### POST `/reset`
- Clears the CSV and recreates the header line.

```bash
curl -X POST http://WeatherStation1.local/reset -i
```

### POST `/sleep`
- Forces device into deep sleep immediately with same wake logic.

```bash
curl -X POST http://WeatherStation1.local/sleep -i
```

### POST `/restart` and GET `/restart`
- Triggers a soft reboot.

```bash
curl -X POST http://WeatherStation1.local/restart -i
```

### GET `/update` (OTA)
- Provided by ElegantOTA. Basic auth is enabled.
- Default credentials set in sketch:
  - Username: `weatherstation1`
  - Password: `12345678`

```bash
# Open in browser and upload a new .bin
xdg-open http://WeatherStation1.local/update
```

## mDNS
- Service registers at `<mdnsHost>.local` (defaults to `weatherstation1.local`, configurable).
- If mDNS fails, use the device IP printed on serial on connect.

## Persistent Configuration (Preferences)
Namespace: `app`. Stored as a JSON string under key `cfg`.

Fields:
- `altitude_m` (float)
- `temp_unit` (`F`|`C`)
- `bat_cal` (float)
- `time_12h` (bool)
- `lux_enter_day` (number)
- `lux_exit_day` (number)
- `log_interval_min` (integer)
- `sleep_minutes` (integer)
- `trend_threshold_hpa` (float)
- `rain_unit` (`in`|`mm`)
- `mdns_host` (string) — mDNS host label (no `.local`)
- `sds_mode` (`off|pre1|pre2|pre5|cont`)
- `debug_verbose` (bool)

Wi‑Fi configuration is under namespace `wifi`, key `config`, containing `{"networks": [{"ssid":"...","pass":"..."}, ...]}`.

## Function Reference
All functions are defined within the main `.ino`. Key functions and their roles:

### setup(), loop()
- Standard Arduino entry points. `setup()` initializes peripherals, loads config, mounts SD, starts HTTP server and OTA, seeds time, and logs an initial row. `loop()` services HTTP, status LED, day/night logic, periodic logging, time sync, and sleep scheduling.

### Wi‑Fi and OTA
- `bool connectToWifi()` — Attempts to join saved networks (visible first, then all) with per-network timeout, enables auto-reconnect on success.
- `void startAPMode()` — Starts AP `WeatherStation1` with password `12345678` and indicates status by LED pulses.
- `void setupOTA()` — Initializes ElegantOTA on `/update` and enables basic auth.

### HTTP Handlers
- `void setupServerRoutes()` — Registers all routes:
  - `GET /` → `handleRoot()`
  - `GET /live` → `handleLive()`
  - `GET /download` → `handleDownload()`
  - `GET /view-logs` → `handleViewLogs()`
  - `GET /config` + `POST /config`
  - `POST /add`, `GET /del`, `POST /reset`, `POST /sleep`, `POST|GET /restart`
- `void handleRoot()` — Renders the dashboard HTML.
- `void handleLive()` — Reads sensors, computes derived metrics, returns JSON, and performs a single CSV append per wake.
- `void handleDownload()` — Streams CSV.
- `void handleViewLogs()` — Renders last rows of CSV into a table with client-side filtering.
- `void handleAdd()` — Adds a Wi‑Fi network to preferences.
- `void handleDel()` — Removes a Wi‑Fi network by SSID.
- `void handleReset()` — Clears and recreates log file with header.
- `void handleSleep()` — Programs RTC/timer and enters deep sleep.
- `void restartHandler()` — Replies and restarts.

### Sensors, Logging, Power
- `float readLux()` — Reads lux from VEML7700, clamps invalid values to 0.
- `void performLogging()` — On schedule, reads BME680 and battery, computes derived metrics, updates pressure history/trend, appends a CSV row, and updates `lastSdLogUnix`/`lastSdLogTime`.
- `void updateDayNightState()` — Debounced, averaged light-based state machine to transition between DAY/NIGHT and trigger sleep.
- `void updateStatusLed()` — Centralized status LED behavior for AP/connected/attempting and non-blocking pulse overrides; calm when disconnected in day/night.
- `void blinkStatus(int times, int durationMs)` — Schedules non-blocking LED pulses handled by `updateStatusLed()`.
- `void prepareDeepSleep(uint32_t wakeAfterSeconds)` — Programs DS3231 alarm (A1 minute match) and ESP32 timer wake, stores `lastAlarmUnix`, and enters deep sleep.

### Time, Pressure History, Forecast
- `void loadAppConfig()` / `void saveAppConfig()` — Read/write `AppConfig` struct to/from Preferences.
- `void updatePressureHistory(float P_hPa, uint32_t nowUnix)` — Stores up to 13 hourly samples in RTC memory.
- `bool getPressureDelta(float hours, float currentP, float* outDelta)` — Retrieves ΔP vs a past sample if available.
- `const char* classifyTrendFromDelta(float delta)` — Maps ΔP to `Rising|Falling|Steady`.
- `const char* zambrettiSimple(float mslp_hPa, const char* trend)` — Very simplified Zambretti forecast label.

### Derived Meteorology Utilities
- `float computeDewPointC(float Tc, float RH)` — Dew point in °C (Magnus formula). Example: `computeDewPointC(25.0f, 60.0f) ≈ 16.7`.
- `float computeHeatIndexF(float Tf, float RH)` — Heat index in °F (Rothfusz). Example: `computeHeatIndexF(86.0f, 60.0f) ≈ 94.0`.
- `float computeMSLP_hPa(float P_hPa, float Tc, float altitudeM)` — Sea-level pressure estimate in hPa.
- `float computeWetBulbC(float Tc, float RH)` — Approximate wet-bulb temp in °C (Stull). Example: `computeWetBulbC(25.0f, 60.0f) ≈ 19.0`.

### SD Utilities
- `uint64_t computeSdUsedBytesRecursive(File dir)` / `uint64_t computeSdUsedBytes()` — Estimates used bytes on SD card.

### Rain Gauge ISR
- `void IRAM_ATTR rainIsr()` — Debounced tipping bucket counter; stores timestamps in a ring buffer for rate calculations used by `/live`.
  - Accumulation totals: rolling 1h, daily (midnight reset), and event total (resets after ≥6h dry gap). Tip size is configurable via `rain_tip_in`.

### Wind Anemometer ISR (optional)
- `void IRAM_ATTR windIsr()` — Hall sensor pulse capture using a 128‑entry ring buffer; UI and `/live` expose wind in Hz, m/s, km/h, mph.
  - Wind gust metric exposed as `wind_gust_mph` using the classic NOAA method: highest 5‑second sample in the last 10 minutes.

## Usage Notes
- The device records a boot event row immediately after the first `performLogging()` in `setup()` (an extra line with only timestamp and boot count at the end).
- `last_sd_log` in `/live` is formatted per the 12/24‑hour preference; raw unix is persisted in `RTC_DATA_ATTR`.
- If DS3231 is unavailable, wake is timer-only and `rtc_ok` will be `false`.
- LED behavior is calm by default when disconnected; use `blinkStatus()` to signal events.

AP fallback: SSID `WeatherStation1`, password `12345678`.

## Integration Recipes
- **Polling telemetry**: fetch `/live` every 2–10 s and ingest JSON.
- **Batch download**: GET `/download` daily and append to a data lake.
- **Headless config**: POST `/config` after flashing to set altitude and units.
- **Wi‑Fi provisioning**: POST `/add` repeatedly for multiple SSIDs.
- **Force sleep**: POST `/sleep` to immediately re-enter deep sleep after a maintenance session.

## Security Considerations
- OTA is protected by static basic auth. Rotate credentials in `setupOTA()` and `startAPMode()` before deployment.
- `/add` and `/config` accept plaintext in HTTP; consider running on a trusted LAN only.

## Troubleshooting
- If mDNS fails, use the printed IP or router DHCP leases.
- If SD mount fails, verify wiring, CS pin, and card formatting; logs require `/logs.csv`.
- If BME680/VEML7700 are not detected, check I2C wiring and addresses.