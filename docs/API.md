### ESP32 Weather Station – Public API and Function Reference

This document describes the HTTP endpoints, persisted configuration, CSV schema, and the major functions exported in the `TempLoggerBatDev_v1_v16_08_13_25.ino` sketch. It also includes examples and integration guidance.

## Overview
- Device: ESP32-based weather station
- Sensors: BME680 (temperature, humidity, pressure, gas resistance), VEML7700 (ambient light)
- Storage: SD card (`/logs.csv`)
- Time: DS3231 RTC (preferred) and NTP syncing
- Connectivity: Wi‑Fi STA with AP fallback, mDNS
- OTA: ElegantOTA at `/update` (basic auth)
- Modes: DAY (stay awake, periodic logs) / NIGHT (short serve window, deep sleep)

## Pins, Interfaces, and Constants
- I2C SDA: 21, I2C SCL: 22
- SD: CS=5, SCK=18, MISO=19, MOSI=23
- Battery ADC pin: 35 (voltage divider R1=100k, R2=100k)
- Rain gauge reed switch: 27 (to GND)
- DS3231 interrupt: `RTC_INT_PIN = GPIO_NUM_2`
- Status LED: 4
- BME680 I2C addresses tried: `0x76`, then `0x77`

Timing and mode constants:
- `DEEP_SLEEP_SECONDS = 600` (10 minutes)
- Initial serve window after cold boot: `UPTIME_STARTUP = 1800` seconds
- Subsequent serve windows after wake: `UPTIME_CONFIG = 120` seconds
- Logging cadence while awake: `LOG_INTERVAL_MS = 600000` (10 minutes)
- Day/Night light thresholds: enter DAY ≥ 1600 lux; exit DAY ≤ 1400 lux, dwell 30 s

## CSV Log Schema
File: `/logs.csv`

Header created by Reset:
```
timestamp,temp_f,humidity,dew_f,hi_f,pressure,pressure_trend,lux,voltage,voc_kohm,boot_count
```

Example row:
```
2025-01-01 15:42:17,72.8,43.2,50.3,73.9,1013.62,Steady,455.0,4.07,12.5,123
```

Notes:
- Temperature values in CSV are in °F.
- Pressure is in hPa.
- `pressure_trend` is one of `Rising|Falling|Steady`.
- VOC is derived from BME680 gas resistance in kΩ.

## HTTP API
Base: device IP (e.g., `http://192.168.1.50`) or mDNS `http://WeatherStation1.local` if supported.

### GET `/`
- Returns the live dashboard HTML with cards, charts, and Wi‑Fi management UI.
- Includes buttons/links for downloading logs, viewing logs, starting OTA, forcing sleep, restarting, and opening the Config page.

### GET `/live`
- Returns current sensor readings and diagnostics as JSON.

Example:
```json
{
  "temp": 72.8,
  "hum": 43.2,
  "pressure": 1013.62,
  "lux": 455,
  "batt": 4.07,
  "voc_kohm": 12.5,
  "uptime": 1234,
  "heap": 176520,
  "flash_free_kb": 2048,
  "time": "2025-01-01 15:42:17",
  "rain_mmph": 0.28,
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
  "hi_f": 73.9,
  "wbt_f": 54.4,
  "mslp_hPa": 1019.3,
  "mslp_inHg": 30.10,
  "pressure_trend": "Steady",
  "forecast": "Fair",
  "pressure_unit": "hPa"
}
```

- Units:
  - Temperature in this endpoint is °F.
  - `pressure_unit` reports preferred unit for MSLP cards (`hPa` or `inHg`).
  - Battery is pack voltage (single-cell Li‑ion) in volts.
  - `rain_mmph` is instantaneous rate based on tips in last hour.

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
- Query helper parameters (optional): `field`, `type`, `min`, `max`.
  - Example: `?field=temp&type=between&min=60&max=80`.

### GET `/config`
- Returns a simple HTML form to view/change persistent settings.
- Fields:
  - `altitude_m` (float) — affects MSLP calculation
  - `temp_unit` (`F` or `C`) — UI formatting preference
  - `pressure_unit` (`hPa` or `inHg`) — UI formatting preference for MSLP
  - `bat_cal` (float) — multiplier for battery calibration
  - `time_12h` (bool via form select: `12` → true, `24` → false)

### POST `/config`
- Saves settings to Preferences namespace `app`.

Example:
```bash
curl -X POST \
  -F alt=123.4 \
  -F tu=F \
  -F pu=hPa \
  -F bc=1.08 \
  -F tf=24 \
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
- Service attempts to register at `WeatherStation1.local`.
- If mDNS fails, use the device IP printed on serial on connect.

## Persistent Configuration (Preferences)
Namespace: `app`. Stored as a JSON string under key `cfg`.

Fields:
- `altitude_m` (float)
- `temp_unit` (`F`|`C`)
- `pressure_unit` (`inHg`|`hPa`)
- `bat_cal` (float)
- `time_12h` (bool)

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
- `void updateStatusLed()` — Centralized status LED behavior for AP/connected/attempting and non-blocking pulse overrides.
- `void blinkStatus(int times, int durationMs)` — Schedules non-blocking LED pulses handled by `updateStatusLed()`.
- `void prepareDeepSleep(uint32_t wakeAfterSeconds)` — Programs DS3231 alarm and ESP32 timer wake, stores `lastAlarmUnix`, and enters deep sleep.

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

## Usage Notes
- The device records a boot event row immediately after the first `performLogging()` in `setup()` (an extra line with only timestamp and boot count at the end).
- `last_sd_log` in `/live` is formatted per the 12/24‑hour preference; raw unix is persisted in `RTC_DATA_ATTR`.
- If DS3231 is unavailable, wake is timer-only and `rtc_ok` will be `false`.
- LED behavior is calm by default when disconnected; use `blinkStatus()` to signal events.

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