# Changelog

All notable changes to Weather-Station-1-ESP32 will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [v19.1] - 2025-01-13

### Fixed
- **VEML7700 lux reading limitation** - Added non-linear correction for high light levels (>20k lux) to extend sensor range to full 120k lux capability
- **Compilation errors** - Fixed raw literal string syntax issues in `handleRoot()` function

### Changed
- **README documentation** - Updated dashboard screenshots to use new `Mainstaiton.png (1).png` and `Mainstaiton.png (2).png` images

---

## [v19.0] - 2025-01-13

### Added - Enhanced Dashboard Layout with 3-Column Forecast Tile
- **Expanded forecast detail tile** now spans 3 columns (previously 2) for more comprehensive weather information display
- **Reorganized dashboard layout** with improved tile grouping:
  - Row 1: Weather Forecast (3-column span for detailed outlook)
  - Row 2: Key Metrics (Pressure Trend, Temperature, Humidity)
  - Row 3: Core Environmental Sensors (MSLP/Station Pressure, Dew Point)
  - Row 4: Wind & Rain metrics
  - Row 5: Light & UV sensors
  - Row 6: Air Quality (PM2.5, PM10, VOC)
  - Row 7: Comfort metrics (Heat Index, Wet Bulb)
  - Row 8: Agriculture (Leaf Wetness, ETo)
  - Row 9: System Status
- **Enhanced forecast detail display** with more space for comprehensive multi-line weather information including:
  - Storm warnings and frontal passage alerts
  - Detailed wind information with direction and gust warnings
  - Air quality categories and UV risk assessments
  - Rain status and accumulation totals
  - Comfort and agricultural indicators


---

## [v18.3] - 2025-01-03

### Added - Enhanced Multi-Sensor Forecast System
- **Advanced weather prediction** with 40+ distinct forecast states using deep sensor fusion
- `enhancedZambrettiAdv()` function with wind and humidity integration
- Frontal passage detection (cold front: falling P + rising wind; warm front: falling P + high humidity)
- Severe weather scoring system (0-3 scale) based on:
  - Rapid pressure drop rate
  - Strong wind gusts
  - Atmospheric instability (temp + humidity + pressure)
  - Thunderstorm potential (UV + humidity + falling pressure)
- Enhanced storm risk levels: Minimal/Low/Slight/Possible/High/Severe
- Temperature trend analysis using dew point spread (Warming/Cooling/Stable)
- New forecast states including:
  - Storm conditions: `Severe Storm`, `Major Storm`, `Storm / Heavy Rain`, `Heavy Showers`, `Thunderstorms Possible`
  - Rain levels: `Heavy Rain`, `Raining`, `Light Rain`, `Heavy Showers Soon`, `Rain Likely`, `Rain Developing`
  - Improving: `Very Clear & Sunny`, `Clear & Bright`, `Clearing / Sunny`, `Improving`, `Slowly Improving`
  - Settled: `Very Settled / Clear`, `Settled / Fine`, `Fair / Settled`, `Clear & Dry`
  - Variable: `Partly Cloudy`, `Variable / Bright Spells`, `Cloudy / Stable`, `Damp / Overcast`

### Added - Dashboard Improvements
- Reorganized tile layout with **forecast prominently displayed at top-left** (2-column span with 🌤️ emoji)
- Logical tile grouping by function:
  - Row 1: Weather Forecast (featured), Pressure Trend, Temperature
  - Row 2: Core Environmental Sensors (Humidity, Pressure/MSLP, Dew Point)
  - Row 3: Wind & Rain metrics
  - Row 4: Light & UV sensors
  - Row 5: Air Quality (PM2.5, PM10, VOC)
  - Row 6: Comfort metrics (Heat Index, Wet Bulb)
  - Row 7: Agriculture (Leaf Wetness, ETo)
  - Row 8: System Status
- Enhanced forecast detail display with:
  - ⚠️ Storm warnings for severe/high risk conditions
  - "Front Approaching" alerts
  - Enhanced wind information with direction and gust warnings
  - "Rapid P Drop" vs "Falling P" distinction
  - "Raining Now" immediate status
  - Comfort notes: "Very Humid" vs "Humid"

### Added - Configuration Page Redesign
- Organized settings into **7 logical sections** with visual separators:
  - 🖥️ System Settings (Altitude, Temperature Unit, Battery Cal, Clock Format, mDNS)
  - 🔋 Power & Timing (Light thresholds, Log interval, Sleep duration)
  - 🌡️ Pressure & Forecast (Trend threshold, Pressure display)
  - 🌧️ Rain Gauge (Unit, Tip size, Debounce)
  - 💨 Air Quality (SDS011 duty cycle)
  - 🍃 Leaf Wetness (Debug mode, ADC calibration, Thresholds)
  - 💧 Evapotranspiration (Unit, Latitude)
  - 🐛 Debug (Verbose logging)
- Added colored section headers with emoji icons
- Added section backgrounds (#1a1a1a) with rounded corners for visual grouping
- Improved spacing and readability
- Added input validation (type='number', min/max/step attributes)

### Added - Pressure Display Toggle
- New config option `show_mslp` to choose between:
  - MSLP (sea-level pressure, altitude-adjusted, shown in inHg)
  - Station Pressure (raw barometric pressure, shown in hPa)
- Dashboard conditionally shows one pressure tile based on user preference
- Legend dynamically updates to explain selected pressure metric
- Reduces dashboard clutter while maintaining user choice

### Changed - Legend Enhancements
- Reorganized into visual categories matching dashboard layout:
  - 🌧️ Wind & Rain
  - ☀️ Light & UV
  - 💨 Air Quality
  - 🌡️ Comfort
  - 🍃 Agriculture
  - 🔮 Forecast
  - 🔋 System
- Removed CO₂ references (sensor not present in current build)
- Added clearer, more concise descriptions
- Added visual divider lines between categories

### Changed - Forecast Functions
- `generalForecastFromSensors()` now uses wind data for enhanced accuracy
- `buildForecastDetail()` includes frontal detection and enhanced storm warnings
- `stormRiskLabel()` enhanced with 7 distinct risk levels (vs previous 3)
- Forecast now uses 1-hour average wind (vs instantaneous) for stability

### Fixed
- Forecast tile now spans 2 columns for better visibility
- JavaScript handles both pressure and MSLP elements gracefully
- Legend matches actual dashboard tile layout
- Config page validation improved with proper input types

---

## [v18.2] - 2025-01-01

### Added
- FAO-56 reference evapotranspiration (ETo) with dual methods:
  - Penman-Monteith (preferred, when all sensors available)
  - Hargreaves-Samani (fallback, uses only temperature and solar radiation)
- ETo to CSV logging (columns 29-30: `eto_hourly`, `eto_daily`)
- ETo to `/live` JSON endpoint:
  - `eto_hourly_mm`, `eto_hourly_in` (hourly rate)
  - `eto_daily_mm`, `eto_daily_in` (daily cumulative)
  - `eto_unit` (current unit setting)
- ETo configuration options:
  - `eto_unit` (mm/day or in/day)
  - `latitude` (degrees, for solar radiation estimation)
- Runtime-configurable leaf wetness calibration:
  - `leaf_adc_dry` (dry endpoint, 0-4095)
  - `leaf_adc_wet` (wet endpoint, 0-4095)
  - `leaf_wet_on_pct` (wet threshold ON %, hysteresis)
  - `leaf_wet_off_pct` (wet threshold OFF %, hysteresis)
- Enhanced leaf debug display showing all calibration values and current raw ADC
- Leaf wetness to CSV (columns 25-28: `leaf_raw`, `leaf_pct`, `leaf_wet`, `leaf_wet_hours`)

### Changed
- CSV header extended to 30 columns (from 24)
- `/live` JSON expanded with ETo and enhanced leaf wetness fields
- `/config` page includes new ETo and leaf calibration settings
- Leaf wetness calibration moved from compile-time to runtime configuration

---

## [v18.1] - 2024-12-15

### Added
- Wind gust calculation (NOAA method: highest 5-second sample in last 10 minutes)
- Rain accumulation totals:
  - 1-hour accumulation (`rain_1h_mm`, `rain_1h_in`)
  - Daily accumulation since midnight (`rain_today_mm`, `rain_today_in`)
  - Event accumulation since last ≥6h dry gap (`rain_event_mm`, `rain_event_in`)
- Wind gust and rain totals to CSV (columns 21-24)
- Rolling 1-hour average wind speed (`wind_avg_mph_1h`) sampled once per minute
- Leaf wetness sensor support (LM393):
  - Percentage (0-100%) with EMA smoothing
  - Wet/dry state with hysteresis (55% ON, 45% OFF)
  - 24-hour wet-hours accumulation in RTC memory
  - Daily rollover at local midnight
- Enhanced forecast detail (`forecast_detail`) with rich multi-line output:
  - Air quality category from PM2.5
  - UV risk category
  - Wind descriptor with speed, direction, and gust
  - Humidity and pressure tendency notes
  - Rain risk assessment
  - Heat, storm, fog, frost risk indicators
  - Rain 3/6/12h trend arrows (↑↓→)
- `storm_risk` boolean flag to `/live` JSON
- `aqi_category` string to `/live` JSON
- Wind vane dead-zone handling (returns last valid direction)
- `wind_vane_ok` flag to indicate PCF8574 detection
- `sds_auto_sleep_ms_left` countdown for duty cycle visibility
- `leaf_debug` config option for field calibration
- `rain_debounce_ms` config option (runtime configurable ISR debounce)

### Changed
- CSV header extended to 24 columns (from 14-20)
- `/view-logs` page improved with dark theme and better filtering
- Dashboard includes new wind average, gust, rain totals, and leaf wetness cards
- Forecast now considers wind speed and leaf wetness for agriculture use cases

### Fixed
- Wind direction stability improved with majority voting (3 samples, 300µs apart)
- Rain event reset logic (≥6 hour dry gap requirement)
- Leaf wetness daily rollover at local midnight using `tm_yday`

---

## [v18.0] - 2024-11-01

### Added
- Extended CSV logging (14-20 columns) with wind and dust sensors
- SDS011 PM2.5/PM10 sensor support with smart duty cycling:
  - Configurable presets: off/pre1/pre2/pre5/cont
  - 30-second warm-up period tracking
  - Auto-sleep enforcement in 2-minute serve windows
  - Persistent last-known values in RTC memory
- Hall anemometer wind speed support:
  - Interrupt-driven pulse counting (GPIO7)
  - 128-entry ring buffer for historical analysis
  - Calibration: 1.52870388047 mph per Hz
- PCF8574-based wind vane direction (8-point compass)
- UV index support via GUVA-S12SD analog sensor
- Enhanced `/live` JSON with wind, PM, and UV fields
- `debugPrintf()` helper respecting `debug_verbose` flag

### Changed
- **BREAKING:** Switched from BSEC2 to Adafruit_BME680 library (simpler, more compatible)
- Removed CO₂ sensor support (SCD41) - may return in future version
- CSV schema extended significantly
- `/view-logs` backward-compatible with legacy CSV formats (6/14 columns)

### Removed
- BSEC2 library dependency (IAQ, eCO₂, bVOC metrics removed)
- SCD41 CO₂ sensor support

---

## [v17.x] - 2024-08-24

### Legacy Versions
Earlier versions (v17 and below) included:
- Basic BME680 + VEML7700 logging
- Simple Zambretti forecast
- Wi-Fi management
- SD card storage
- DS3231 RTC timekeeping
- DAY/NIGHT power modes
- OTA updates via ElegantOTA

Full history available in git commit log.

---

## Upcoming / Planned

See [Roadmap](README.md#roadmap-ideas) in README.md

- MQTT support for home automation integration
- Optional CO₂ sensor re-integration (SCD41) Not needed anymore
- Web-based calibration tools in /config
- RGB LED status indicators
- Power metering (INA3221)

---

**Maintained by:** [@JoshLongmire](https://github.com/JoshLongmire)  
**Contributors:** See [GitHub Contributors](https://github.com/JoshLongmire/Weather-Station-1-ESP32/graphs/contributors)  
**License:** PolyForm Noncommercial 1.0.0 (code) | CC BY-4.0 (docs/images)

