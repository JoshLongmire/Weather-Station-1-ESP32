#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>
#include <Adafruit_VEML7700.h>
#include <ElegantOTA.h>
#include "time.h"
#include <esp_sleep.h>
#include <ESPmDNS.h>     // Provides the global `MDNS` instance
#include <esp_system.h>  // for esp_restart()
#include <RTClib.h>      // DS3231 RTC driver
#include "driver/rtc_io.h"
#include <math.h>  // for isfinite()
// for logs page rendering
#include <vector>
//#include <Adafruit_SGP40.h>        // 🌬 VOC sensor
// —— Optional: Sensirion SCD41 CO2 sensor (SCD4x family) ——
#ifndef ENABLE_SCD41
#define ENABLE_SCD41 1
#endif
#if ENABLE_SCD41
#if __has_include(<SensirionI2CScd4x.h>)
#include <SensirionI2CScd4x.h>
#define HAVE_SCD4X_LIB 1
#elif __has_include(<SensirionI2CSCD4X.h>)
#include <SensirionI2CSCD4X.h>
#define HAVE_SCD4X_LIB 1
#else
#warning "SCD41 enabled but SensirionI2CScd4x header not found. CO2 will be disabled."
#undef ENABLE_SCD41
#define ENABLE_SCD41 0
#endif
#endif

// —— Optional: SDS011 PM2.5/PM10 sensor support ——
#ifndef ENABLE_SDS011
#define ENABLE_SDS011 1
#endif

// —— Optional: Bosch BSEC2 library for BME680 IAQ/eCO2/bVOC — disabled for Adafruit_BME680 only mode ——
#ifndef ENABLE_BSEC2
#define ENABLE_BSEC2 0
#endif

// —— Optional: Hall anemometer (wind) ——
#ifndef ENABLE_WIND
#define ENABLE_WIND 1
#endif


// —— Pin & Address Definitions ——
//#define LED_PIN            2
// Pin for DS3231 SQW/INT connected to ESP32 RTC GPIO
//#define FAN_PIN 12  // choose any free GPIO--------------------------------------un// when installed
#define RTC_INT_PIN GPIO_NUM_2
#define SD_CS 5         // change to your CS pin
#define SD_SCK_PIN 12   // VSPI default SCK
#define SD_MISO_PIN 13  // VSPI default MISO
#define SD_MOSI_PIN 11  // VSPI default MOSI
#define I2C_SDA 8
#define I2C_SCL 9
#define ADC_BATTERY_PIN 4
#if ENABLE_SDS011
// SDS011 on HW UART2 (default RX=16, TX=17 unless remapped)
#define SDS_RX_PIN 16
#define SDS_TX_PIN 17
// Duty cycle settings for DAY mode
#define SDS_DUTY_ON_MS 120000UL   // 2 minutes ON before log
#define SDS_DUTY_OFF_MS 480000UL  // 8 minutes OFF after log (example)
#define SDS_DUTY_WARMUP_MS 30000UL  // 30 s warm-up per ON cycle for stable readings
#if __has_include(<SdsDustSensor.h>)
#include <SdsDustSensor.h>
#define HAVE_SDS_LIB 1
#endif
#endif
// Rain gauge tipping bucket (reed switch to GND)
#define RAIN_PIN 18
// Hall anemometer wind sensor input (adjust to your wiring)
#define WIND_PIN 7
// UV sensor GUVA-S12SD analog output
#define UV_PIN 6
//#define MQ135_PIN          34       // ADC1_CH6 (analog gas)
//#define MICS5524_PIN       35       // ADC1_CH7 (analog gas)
//#define SOLAR_IN_PIN       32       // ADC1_CH4 (DFRobot Solar IN)+
//#define SOLAR_OUT_PIN      33       // ADC1_CH5 (DFRobot Solar OUT)
#define BME280_ADDRESS 0x76

// —— Deep‐Sleep & Serve Windows (seconds) ——
#define DEEP_SLEEP_SECONDS 600  // 10 min between wakes
#define UPTIME_STARTUP 1800UL   // first‐boot: serve for 30 min
#define UPTIME_CONFIG 120       // subsequent wakes: 2 min



// —— Voltage Divider (Ohms) ——
const float R1 = 100000.0;
const float R2 = 100000.0;
const float VOLT_DIVIDER = (R1 + R2) / R2;
RTC_DS3231 rtc;  // RTC instance

// ==== Day/Night Light Control Globals ====
enum PowerMode { MODE_DAY,
                 MODE_NIGHT };
PowerMode powerMode = MODE_NIGHT;

uint16_t LUX_ENTER_DAY = 1600;    // enter continuous awake
uint16_t LUX_EXIT_DAY = 1400;     // exit to deep-sleep cycling
const uint32_t DWELL_MS = 30000;  // 30 s dwell
const uint32_t SAMPLE_INTERVAL_MS = 2000;

const uint8_t LUX_BUF_SIZE = DWELL_MS / SAMPLE_INTERVAL_MS;  // 15 samples
uint32_t luxBuffer[LUX_BUF_SIZE];
uint8_t luxIndex = 0;
uint8_t luxCount = 0;
uint32_t luxSum = 0;

uint32_t lastSampleMillis = 0;
uint32_t conditionStartMillis = 0;
bool trackingBright = false;

uint32_t LOG_INTERVAL_MS = 600000UL;  // 10 min
uint32_t nextLogMillis = 0;

// Save number of wakeups across deep sleep
RTC_DATA_ATTR uint32_t bootCount = 0;
// Persist the last scheduled alarm time (unix seconds) across deep sleep
RTC_DATA_ATTR uint32_t lastAlarmUnix = 0;
// Persist last successful SD log time across deep sleep
RTC_DATA_ATTR uint32_t lastSdLogUnix = 0;

// —— Globals ——
WebServer server(80);
Preferences preferences;
DynamicJsonDocument wifiConfig(4096);
Adafruit_BME680 bme;  // BME680: temp, hum, pressure, gas
//Adafruit_SGP40        sgp;
Adafruit_VEML7700 lightMeter;
#if ENABLE_SCD41 && defined(HAVE_SCD4X_LIB)
SensirionI2CScd4x scd4x;   // SCD41 instance
bool scd41Ok = false;      // health flag
uint16_t scd41Co2Ppm = 0;  // ppm
float scd41TempC = 0.0f;   // °C
float scd41Rh = 0.0f;      // %RH
unsigned long scd41LastPollMs = 0;
#else
bool scd41Ok = false;
uint16_t scd41Co2Ppm = 0;
float scd41TempC = 0.0f;
float scd41Rh = 0.0f;
#endif
#if ENABLE_SDS011
// —— SDS011 state ——
HardwareSerial& sdsSerial = Serial2;
bool sdsPresent = false;
bool sdsAwake = false;
unsigned long sdsWarmupUntilMs = 0;  // millis when warm-up ends
unsigned long sdsLastPacketMs = 0;   // last valid data timestamp
float sdsPm25 = 0.0f;                // µg/m³
float sdsPm10 = 0.0f;                // µg/m³
// Accumulators for averaging while awake and warmed
uint32_t sdsAccumPm25RawSum = 0;  // sum of raw PM2.5 values (10x µg/m³)
uint32_t sdsAccumPm10RawSum = 0;  // sum of raw PM10 values (10x µg/m³)
uint32_t sdsAccumCount = 0;       // number of frames accumulated
// Small parser buffer for raw frames
uint8_t sdsBuf[10];
uint8_t sdsBufPos = 0;
// Duty cycling state (DAY mode and during config window)
bool sdsDutyOn = false;
unsigned long sdsDutyNextToggleMs = 0;
#if defined(HAVE_SDS_LIB)
SdsDustSensor sds(sdsSerial);
#endif
// Auto-sleep deadline relative to wake (ms); used for 120s serve window alignment
unsigned long sdsAutoSleepAtMs = 0;

// —— SDS011 raw command frames (vendor protocol) ——
// 19-byte frames: AA B4 06 01 <01=wake|00=sleep> ... FF FF <sum> AB
static const uint8_t SDS_WAKE_CMD[19]  = {0xAA,0xB4,0x06,0x01,0x01,0,0,0,0,0,0,0,0,0,0,0xFF,0xFF,0x06,0xAB};
static const uint8_t SDS_SLEEP_CMD[19] = {0xAA,0xB4,0x06,0x01,0x00,0,0,0,0,0,0,0,0,0,0,0xFF,0xFF,0x05,0xAB};

static inline void sdsSendFrame(const uint8_t* frame) {
  sdsSerial.write(frame, 19);
  sdsSerial.flush();
}

static inline void sdsEnsureWake() {
#if defined(HAVE_SDS_LIB)
  sds.wakeup();
#endif
  sdsSendFrame(SDS_WAKE_CMD);
  sdsAwake = true;
  sdsWarmupUntilMs = millis() + SDS_DUTY_WARMUP_MS;
}

static inline void sdsEnsureSleep() {
#if defined(HAVE_SDS_LIB)
  sds.sleep();
#endif
  sdsSendFrame(SDS_SLEEP_CMD);
  sdsAwake = false;
}
#endif
unsigned long startMillis;
unsigned long serveWindow;
static bool loggedThisWake = false;
const uint8_t STATUS_LED_PIN = 37;
const uint32_t BLINK_INTERVAL = 500;  // blink every 500 ms
unsigned long lastBlink = 0;
bool blinkState = LOW;
const uint32_t BLINK_INTERVAL_FAST = 100;  // fast blink when attempting to connect
// Battery calibration multiplier to align ADC reading to actual battery voltage
float batCal = 1.08f;  // configurable via /config
// Track RTC availability after setup to avoid reinitializing on each /live request
bool rtcOkFlag = false;
// Non-blocking LED pulse override (used by blinkStatus without delays)
volatile bool ledPulseActive = false;
unsigned long ledPulseIntervalMs = 0;
unsigned long ledPulseLastToggle = 0;
int ledPulseRemainingToggles = 0;  // total on/off edges remaining

// Track last successful SD log timestamp for dashboard display
char lastSdLogTime[32] = "N/A";
// Track whether we've started the post-config short (2 min) window
bool postConfigStarted = false;
// Night window scheduled logging (~35s in) to allow sensor warm-up
unsigned long scheduledNightLogMs = 0;
bool nightLogDone = false;
// —— Rain gauge state ——
volatile uint32_t rainTipCount = 0;
volatile uint32_t rainTipTimesMs[128];
volatile uint8_t rainTipHead = 0;
volatile uint8_t rainTipSize = 0;
volatile uint32_t lastRainTipMs = 0;
const uint32_t RAIN_DEBOUNCE_MS = 150;
// Critical section guard for ISR/task access
portMUX_TYPE rainMux = portMUX_INITIALIZER_UNLOCKED;

// —— Wind anemometer (Hall) state ——
#if ENABLE_WIND
volatile uint32_t windPulseTimesMs[128];
volatile uint8_t windPulseHead = 0;
volatile uint8_t windPulseSize = 0;
volatile uint32_t lastWindPulseMs = 0;
const uint32_t WIND_DEBOUNCE_MS = 5;  // ms
// Critical section for wind ring buffer
portMUX_TYPE windMux = portMUX_INITIALIZER_UNLOCKED;
// Calibration constants (per provided geometry and PPR=2, CUP_FACTOR=3)
const float WIND_MPH_PER_HZ = 1.52870388047f;
const float WIND_KMH_PER_HZ = 2.46020633977f;
const float WIND_MPS_PER_HZ = 0.68339064994f;
#endif

// —— 1-hour rolling average wind speed (mph), sampled once per minute ——
#if ENABLE_WIND
float windAvg1hMph[60] = { 0 };
uint8_t windAvg1hIndex = 0;
uint8_t windAvg1hCount = 0;
float windAvg1hSum = 0.0f;
unsigned long windAvg1hLastSampleMs = 0;
#endif

// —— BSEC2 globals ——
// —— BSEC2 disabled: keep placeholders for /live compatibility ——
bool bsecOk = false;  // still expose health in /live
float iaq = 0.0f, eco2 = 0.0f, bvoc = 0.0f;

// ISR for tipping bucket; keep it IRAM safe and fast
void IRAM_ATTR rainIsr() {
  uint32_t nowMs = (uint32_t)(esp_timer_get_time() / 1000ULL);
  portENTER_CRITICAL_ISR(&rainMux);
  if (nowMs - lastRainTipMs >= RAIN_DEBOUNCE_MS) {
    lastRainTipMs = nowMs;
    rainTipCount++;
    rainTipTimesMs[rainTipHead] = nowMs;
    rainTipHead = (rainTipHead + 1) % 128;
    if (rainTipSize < 128) rainTipSize++;
  }
  portEXIT_CRITICAL_ISR(&rainMux);
}
#if ENABLE_WIND
// ISR for Hall anemometer pulses; IRAM safe and fast
void IRAM_ATTR windIsr() {
  uint32_t nowMs = (uint32_t)(esp_timer_get_time() / 1000ULL);
  portENTER_CRITICAL_ISR(&windMux);
  if (nowMs - lastWindPulseMs >= WIND_DEBOUNCE_MS) {
    lastWindPulseMs = nowMs;
    windPulseTimesMs[windPulseHead] = nowMs;
    windPulseHead = (windPulseHead + 1) % 128;
    if (windPulseSize < 128) windPulseSize++;
  }
  portEXIT_CRITICAL_ISR(&windMux);
}
#endif
// Boot start time (unix seconds) for the current boot session
time_t bootStartUnix = 0;

// HTML and URL helpers used in handleRoot()
static String htmlEscape(const String& in) {
  String out;
  out.reserve(in.length() * 12 / 10 + 4);
  for (size_t i = 0; i < in.length(); ++i) {
    char c = in[i];
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c; break;
    }
  }
  return out;
}

static String urlEncode(const String& s) {
  const char* hex = "0123456789ABCDEF";
  String out;
  out.reserve(s.length() * 3);
  for (size_t i = 0; i < s.length(); ++i) {
    unsigned char c = (unsigned char)s[i];
    bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
    if (safe) out += (char)c;
    else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

// Sanitize an mDNS host label (without .local)
static String sanitizeMdnsHost(const String& in) {
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); ++i) {
    char c = (char)in[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') {
      out += c;
    } else if (c == ' ' || c == '_') {
      out += '-';
    }
  }
  while (out.length() && out[0] == '-') out.remove(0, 1);
  while (out.length() && out[out.length() - 1] == '-') out.remove(out.length() - 1);
  if (out.length() == 0) out = "weatherstation1";
  if (out.length() > 31) out = out.substring(0, 31);
  return out;
}

// Compute SD used bytes recursively as a fallback when FS does not expose usedBytes()
uint64_t computeSdUsedBytesRecursive(File dir) {
  uint64_t total = 0;
  if (!dir) return 0;
  File entry = dir.openNextFile();
  while (entry) {
    if (entry.isDirectory()) {
      total += computeSdUsedBytesRecursive(entry);
    } else {
      total += static_cast<uint64_t>(entry.size());
    }
    entry = dir.openNextFile();
  }
  return total;
}
uint64_t computeSdUsedBytes() {
  File root = SD.open("/");
  if (!root) return 0;
  uint64_t total = computeSdUsedBytesRecursive(root);
  root.close();
  return total;
}

// ===== Derived weather metrics =====
static inline float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
float computeDewPointC(float Tc, float RH) {
  RH = clampf(RH, 1.0f, 100.0f);
  const float a = 17.62f, b = 243.12f;
  float gamma = log(RH / 100.0f) + (a * Tc) / (b + Tc);
  return (b * gamma) / (a - gamma);
}
float computeHeatIndexF(float Tf, float RH) {
  // Rothfusz regression; clamp to valid ranges
  Tf = clampf(Tf, -40.0f, 150.0f);
  RH = clampf(RH, 0.0f, 100.0f);
  float HI = -42.379f + 2.04901523f * Tf + 10.14333127f * RH
             - 0.22475541f * Tf * RH - 6.83783e-3f * Tf * Tf - 5.481717e-2f * RH * RH
             + 1.22874e-3f * Tf * Tf * RH + 8.5282e-4f * Tf * RH * RH - 1.99e-6f * Tf * Tf * RH * RH;
  return HI;
}
float computeMSLP_hPa(float P_hPa, float Tc, float altitudeM) {
  // Barometric formula
  return P_hPa * powf(1.0f - (0.0065f * altitudeM) / (Tc + 273.15f + 0.0065f * altitudeM), -5.257f);
}

// Approximate wet-bulb temperature (°C) from air temp (°C) and RH (%) using Stull (2011)
// Valid for typical weather ranges; average error ~0.3 °C
float computeWetBulbC(float Tc, float RH) {
  RH = clampf(RH, 1.0f, 100.0f);
  float term1 = Tc * atanf(0.151977f * sqrtf(RH + 8.313659f));
  float term2 = atanf(Tc + RH);
  float term3 = atanf(RH - 1.676331f);
  float term4 = 0.00391838f * powf(RH, 1.5f) * atanf(0.023101f * RH);
  return term1 + term2 - term3 + term4 - 4.686035f;
}

// ---- Additional derived meteorology utilities ----
static inline float saturationVaporPressure_hPa(float Tc) {
  // Magnus (Alduchov & Eskridge) over water
  const float a = 17.62f, b = 243.12f;
  return 6.112f * expf((a * Tc) / (b + Tc));
}

static inline float vaporPressure_hPa(float Tc, float RH) {
  RH = clampf(RH, 0.0f, 100.0f);
  return (RH * 0.01f) * saturationVaporPressure_hPa(Tc);
}

// Absolute humidity (g/m^3) from T (°C) and RH (%)
float computeAbsoluteHumidity_gm3(float Tc, float RH) {
  float e_hPa = vaporPressure_hPa(Tc, RH);
  float Tk = Tc + 273.15f;
  if (Tk < 0.01f) Tk = 0.01f;
  return 216.7f * (e_hPa / Tk);
}

// Mixing ratio (g/kg) from T (°C), RH (%) and pressure (hPa)
float computeMixingRatio_gPerKg(float Tc, float RH, float P_hPa) {
  float e = vaporPressure_hPa(Tc, RH);
  float denom = (P_hPa - e);
  if (denom < 0.001f) denom = 0.001f;
  return 621.97f * (e / denom);
}

// Specific humidity (g/kg) from T (°C), RH (%) and pressure (hPa)
float computeSpecificHumidity_gPerKg(float Tc, float RH, float P_hPa) {
  // q (kg/kg) = 0.622 e / (P - 0.378 e)
  float e = vaporPressure_hPa(Tc, RH);
  float denom = (P_hPa - 0.378f * e);
  if (denom < 0.001f) denom = 0.001f;
  float q = (0.622f * e) / denom;  // kg/kg
  return q * 1000.0f;              // g/kg
}

// Vapor Pressure Deficit (kPa) from T (°C) and RH (%)
float computeVpd_kPa(float Tc, float RH) {
  // Use Tetens in kPa directly for stability at high temps
  float es_kPa = 0.6108f * expf((17.27f * Tc) / (Tc + 237.3f));
  float ea_kPa = es_kPa * clampf(RH, 0.0f, 100.0f) * 0.01f;
  float vpd = es_kPa - ea_kPa;
  return vpd < 0.0f ? 0.0f : vpd;
}

// Air density (kg/m^3) from T (°C), RH (%) and pressure (hPa)
float computeAirDensity_kgm3(float Tc, float RH, float P_hPa) {
  const float Rd = 287.058f;  // J/(kg·K)
  const float Rv = 461.495f;  // J/(kg·K)
  float Tk = Tc + 273.15f;
  if (Tk < 0.01f) Tk = 0.01f;
  float e_hPa = vaporPressure_hPa(Tc, RH);
  float e_Pa = e_hPa * 100.0f;
  float P_Pa = P_hPa * 100.0f;
  float Pd = P_Pa - e_Pa;
  if (Pd < 0.0f) Pd = 0.0f;
  float rho = (Pd / (Rd * Tk)) + (e_Pa / (Rv * Tk));
  return rho;
}

// Humidex (°C) from T (°C) and RH (%)
float computeHumidexC(float Tc, float RH) {
  // Compute dew point then vapor pressure (hPa) via Clausius–Clapeyron
  float Td = computeDewPointC(Tc, RH);
  float e = 6.11f * expf(5417.7530f * (1.0f / 273.16f - 1.0f / (273.15f + Td)));
  return Tc + 0.5555f * (e - 10.0f);
}

// Wind chill (°C) from T (°C) and wind speed (km/h). Returns T when out of range.
float computeWindChillC(float Tc, float windSpeedKmh) {
  if (Tc > 10.0f || windSpeedKmh < 4.8f) return Tc;
  float v016 = powf(windSpeedKmh, 0.16f);
  return 13.12f + 0.6215f * Tc - 11.37f * v016 + 0.3965f * Tc * v016;
}

// WBGT (shade, °C) from T (°C) and RH (%) using Stull Tw approximation
float computeWbgtShadeC(float Tc, float RH) {
  float Tw = computeWetBulbC(Tc, RH);
  return 0.7f * Tw + 0.3f * Tc;
}

// Improved UV index calculation with calibration constants
// index = clamp((uv_mV - offset_mV) * slope_idx_per_mV, 0 .. 20)
float computeUvIndexFromMilliVolts(float uvMilliVolts, float slopeIdxPerMilliVolt, float offsetMilliVolts) {
  float idx = (uvMilliVolts - offsetMilliVolts) * slopeIdxPerMilliVolt;
  if (idx < 0.0f) idx = 0.0f;
  if (idx > 20.0f) idx = 20.0f;  // practical cap
  return idx;
}

// Default calibration wrapper (tweak constants from field calibration if desired)
static const float UV_CAL_SLOPE_IDX_PER_mV = 0.00125f;  // ≈ 0.125 idx per 100 mV
static const float UV_CAL_OFFSET_mV = 0.0f;             // baseline offset
float computeUvIndexCalibrated(float uvMilliVolts) {
  return computeUvIndexFromMilliVolts(uvMilliVolts, UV_CAL_SLOPE_IDX_PER_mV, UV_CAL_OFFSET_mV);
}

// BSEC2 constants will be available from bsec2.h when library is present

// ===== Config persisted in Preferences ('app' namespace) =====
struct AppConfig {
  float altitudeM;  // meters
  bool tempF;       // true=F, false=C
  float batCal;     // battery calibration multiplier
  bool time12h;     // use 12-hour clock
  // Tunables for behavior
  float luxEnterDay;        // lux threshold to enter DAY mode
  float luxExitDay;         // lux threshold to exit DAY mode
  uint16_t logIntervalMin;  // CSV logging interval during day (minutes)
  uint16_t sleepMinutes;    // deep sleep duration between wakes (minutes)
  float trendThresholdHpa;  // pressure delta threshold (hPa) for trend
  bool rainUnitInches;      // true=in/h, false=mm/h
  String mdnsHost;          // mDNS hostname label (no .local)
  // SDS011 duty preset: "off", "pre1", "pre2", "pre5", "cont"
  String sdsMode;
  // Verbose serial debugging
  bool debugVerbose;
};
Preferences appPrefs;
AppConfig appCfg = { 0.0f, true, 1.08f, false,
                     1600.0f, 1400.0f, 10, 10, 0.6f, false, String("weatherstation1"),
                     String("pre2"), false };

void loadAppConfig() {
  appPrefs.begin("app", false);
  String cfg = appPrefs.getString("cfg", "");
  if (cfg.length()) {
    StaticJsonDocument<320> d;
    if (deserializeJson(d, cfg) == DeserializationError::Ok) {
      appCfg.altitudeM = d["altitude_m"] | appCfg.altitudeM;
      appCfg.tempF = d["temp_unit"] ? (String((const char*)d["temp_unit"]) == "F") : appCfg.tempF;
      // pressure unit setting removed; always plot/show both
      appCfg.batCal = d["bat_cal"] | appCfg.batCal;
      appCfg.time12h = d["time_12h"] | appCfg.time12h;
      appCfg.luxEnterDay = d["lux_enter_day"] | appCfg.luxEnterDay;
      appCfg.luxExitDay = d["lux_exit_day"] | appCfg.luxExitDay;
      appCfg.logIntervalMin = d["log_interval_min"] | appCfg.logIntervalMin;
      appCfg.sleepMinutes = d["sleep_minutes"] | appCfg.sleepMinutes;
      appCfg.trendThresholdHpa = d["trend_threshold_hpa"] | appCfg.trendThresholdHpa;
      appCfg.rainUnitInches = d["rain_unit"] ? (String((const char*)d["rain_unit"]) == "in") : appCfg.rainUnitInches;
      if (d["mdns_host"]) appCfg.mdnsHost = String((const char*)d["mdns_host"]);
      if (d["sds_mode"]) appCfg.sdsMode = String((const char*)d["sds_mode"]);
      appCfg.debugVerbose = d["debug_verbose"] | appCfg.debugVerbose;
    }
  }
  // Apply to globals
  batCal = appCfg.batCal;
  // Apply tunables
  LUX_ENTER_DAY = (uint16_t)appCfg.luxEnterDay;
  LUX_EXIT_DAY = (uint16_t)appCfg.luxExitDay;
  LOG_INTERVAL_MS = (uint32_t)appCfg.logIntervalMin * 60000UL;
  // Trend threshold is used by classifier
}

void saveAppConfig() {
  StaticJsonDocument<320> d;
  d["altitude_m"] = appCfg.altitudeM;
  d["temp_unit"] = appCfg.tempF ? "F" : "C";
  // pressure unit removed; keep both in JSON/UI
  d["bat_cal"] = appCfg.batCal;
  d["time_12h"] = appCfg.time12h;
  d["lux_enter_day"] = appCfg.luxEnterDay;
  d["lux_exit_day"] = appCfg.luxExitDay;
  d["log_interval_min"] = appCfg.logIntervalMin;
  d["sleep_minutes"] = appCfg.sleepMinutes;
  d["trend_threshold_hpa"] = appCfg.trendThresholdHpa;
  d["rain_unit"] = appCfg.rainUnitInches ? "in" : "mm";
  d["mdns_host"] = appCfg.mdnsHost.c_str();
  d["sds_mode"] = appCfg.sdsMode.c_str();
  d["debug_verbose"] = appCfg.debugVerbose;
  String out;
  serializeJson(d, out);
  appPrefs.putString("cfg", out);
}

// ===== Pressure history (hourly) persisted across deep sleep =====
RTC_DATA_ATTR float pressureHourly_hPa[13] = { 0 };
RTC_DATA_ATTR uint32_t pressureHourlyUnix[13] = { 0 };
RTC_DATA_ATTR uint8_t pressureHourlyCount = 0;
RTC_DATA_ATTR uint8_t pressureHourlyHead = 0;  // points to next write position

// —— Persist last known PM readings across deep sleep ——
#if ENABLE_SDS011
RTC_DATA_ATTR float sdsLastPm25_ugm3 = 0.0f;
RTC_DATA_ATTR float sdsLastPm10_ugm3 = 0.0f;
RTC_DATA_ATTR uint32_t sdsLastPmUnix = 0;
#endif

void updatePressureHistory(float P_hPa, uint32_t nowUnix) {
  // store one sample per hour boundary
  if (pressureHourlyCount == 0) {
    pressureHourly_hPa[0] = P_hPa;
    pressureHourlyUnix[0] = nowUnix;
    pressureHourlyCount = 1;
    pressureHourlyHead = 1;
    return;
  }
  uint32_t lastUnix = pressureHourlyUnix[(pressureHourlyHead + 12) % 13];
  if (nowUnix - lastUnix >= 3600) {
    pressureHourly_hPa[pressureHourlyHead] = P_hPa;
    pressureHourlyUnix[pressureHourlyHead] = nowUnix;
    pressureHourlyHead = (pressureHourlyHead + 1) % 13;
    if (pressureHourlyCount < 13) pressureHourlyCount++;
  }
}

bool getPressureDelta(float hours, float currentP, float* outDelta) {
  if (pressureHourlyCount < 2) return false;
  uint32_t nowUnix = pressureHourlyUnix[(pressureHourlyHead + 12) % 13];
  uint32_t targetAge = (uint32_t)(hours * 3600.0f);
  // find the oldest sample that is at least targetAge old
  int idx = (pressureHourlyHead + 12) % 13;  // last stored
  for (int i = 0; i < pressureHourlyCount; ++i) {
    int j = (pressureHourlyHead + 12 - i + 13) % 13;
    if (nowUnix - pressureHourlyUnix[j] >= targetAge) {
      idx = j;
      break;
    }
  }
  float past = pressureHourly_hPa[idx];
  *outDelta = currentP - past;
  return true;
}

const char* classifyTrendFromDelta(float delta) {
  const float threshold = appCfg.trendThresholdHpa;  // hPa, from config
  if (delta > threshold) return "Rising";
  if (delta < -threshold) return "Falling";
  return "Steady";
}

const char* zambrettiSimple(float mslp_hPa, const char* trend) {
  // Very simplified mapping
  if (strcmp(trend, "Rising") == 0) {
    if (mslp_hPa >= 1025) return "Settled Fine";
    if (mslp_hPa >= 1015) return "Fine";
    return "Fair";
  } else if (strcmp(trend, "Falling") == 0) {
    if (mslp_hPa >= 1020) return "Change";
    if (mslp_hPa >= 1005) return "Unsettled";
    return "Rain";
  }
  // Steady
  if (mslp_hPa >= 1020) return "Fine";
  if (mslp_hPa >= 1005) return "Fair";
  return "Change";
}

// Simple general forecast derived from multiple sensors
// Uses pressure, trend, humidity, temperature, rain rate, and lux to
// provide a concise human-readable summary for the main dashboard.
static const char* generalForecastFromSensors(float tempF, float hum,
                                              float mslp_hPa, const char* trend,
                                              float rainRateMmH, float lux,
                                              float uvIndex) {
  // Immediate rain condition has highest precedence
  if (rainRateMmH > 0.05f) return "Raining now";

  // Humidity and falling pressure → showers likely
  if (strcmp(trend, "Falling") == 0) {
    if (mslp_hPa < 1008.0f && hum > 75.0f) return "Rain likely";
    if (mslp_hPa < 1005.0f) return "Unsettled / Showers";
    return "Cloudy / Chance of rain";
  }

  // Rising pressure generally improving
  if (strcmp(trend, "Rising") == 0) {
    if (mslp_hPa > 1018.0f) {
      if (uvIndex >= 8.0f && lux > 20000.0f) return "Sunny / High UV";
      return "Improving / Fair";
    }
    return (uvIndex >= 8.0f ? "Improving / High UV" : "Improving");
  }

  // Steady pressure: qualify by humidity/temperature
  if (mslp_hPa >= 1015.0f) {
    if (hum > 75.0f) return "Humid but fair";
    return "Fair";
  }

  // Otherwise, neutral
  if (hum > 85.0f) return "Humid / Overcast";
  if (uvIndex >= 8.0f && lux > 20000.0f) return "High UV / Sunny";
  return "Neutral";
}

// —— Air quality and risk helpers for richer forecast ——
static const char* aqiCategoryFromPm25(float pm25ugm3) {
  if (!isfinite(pm25ugm3) || pm25ugm3 < 0.0f) return "Unknown";
  if (pm25ugm3 <= 12.0f) return "Good";
  if (pm25ugm3 <= 35.4f) return "Moderate";
  if (pm25ugm3 <= 55.4f) return "USG"; // Unhealthy for Sensitive Groups
  if (pm25ugm3 <= 150.4f) return "Unhealthy";
  if (pm25ugm3 <= 250.4f) return "Very Unhealthy";
  return "Hazardous";
}

static const char* uvRiskCategory(float uvIndex) {
  if (!isfinite(uvIndex) || uvIndex < 0.0f) return "Unknown";
  if (uvIndex < 3.0f) return "Low";
  if (uvIndex < 6.0f) return "Moderate";
  if (uvIndex < 8.0f) return "High";
  if (uvIndex < 11.0f) return "Very High";
  return "Extreme";
}

static String buildForecastDetail(float tempF, float hum,
                                  float mslp_hPa, const char* trend,
                                  float pm25ugm3, float uvIndex,
                                  float windMph) {
  String out;
  out.reserve(112);
  // Air quality
  out += "Air: ";
  out += aqiCategoryFromPm25(pm25ugm3);
  // UV risk
  out += " | UV: ";
  out += uvRiskCategory(uvIndex);
  // Wind descriptor
  const char* windLabel = "Calm";
  if (windMph >= 25.0f) windLabel = "Very Windy";
  else if (windMph >= 15.0f) windLabel = "Windy";
  else if (windMph >= 8.0f) windLabel = "Breezy";
  else if (windMph >= 1.0f) windLabel = "Light";
  out += " | Wind: ";
  out += windLabel;
  out += " (";
  out += String(windMph, 0);
  out += " mph)";
  // Humidity note
  if (isfinite(hum)) {
    if (hum >= 85.0f) out += " | Humid";
    else if (hum <= 30.0f) out += " | Dry";
  }
  // Pressure tendency hint
  if (trend && strcmp(trend, "Falling") == 0 && mslp_hPa < 1008.0f) {
    out += " | Falling P";
  }
  return out;
}

void blinkStatus(int times, int durationMs) {
  pinMode(STATUS_LED_PIN, OUTPUT);
  // Schedule a non-blocking pulse sequence handled in updateStatusLed()
  ledPulseActive = true;
  ledPulseIntervalMs = (unsigned long)durationMs;
  ledPulseLastToggle = 0;                // force immediate toggle on next update
  ledPulseRemainingToggles = times * 2;  // on+off per cycle
}
float readLux() {
  float lx = lightMeter.readLux();
  if (lx < 0 || isnan(lx)) lx = 0;
  return lx;
}

// Read GUVA-S12SD analog output and return millivolts (mV)
float readUvMilliVolts() {
  analogSetPinAttenuation(UV_PIN, ADC_11db);
  long acc = 0;
  const int N = 16;
  for (int i = 0; i < N; i++) {
    acc += analogRead(UV_PIN);
    delay(2);
  }
  int raw = acc / N;
  float vAdc = raw * (3.3f / 4095.0f);
  return vAdc * 1000.0f;  // mV
}

// Crude UV Index estimate from GUVA-S12SD mV output
// Linearly map 50..1000 mV → 0..11+ and clamp
float computeUvIndexFromMv(float uvMv) {
  if (!isfinite(uvMv)) return 0.0f;
  float x = (uvMv - 50.0f) / (1000.0f - 50.0f) * 11.0f;
  if (x < 0.0f) x = 0.0f;
  if (x > 11.0f) x = 11.0f;
  return x;
}

// Unified status LED updater:
// - AP mode: solid ON
// - Connected STA: 500 ms blink
// - Attempting/Not connected: 100 ms blink
void updateStatusLed() {
  unsigned long now = millis();

  // Highest precedence: non-blocking pulse override
  if (ledPulseActive) {
    if (ledPulseLastToggle == 0 || now - ledPulseLastToggle >= ledPulseIntervalMs) {
      blinkState = !blinkState;
      digitalWrite(STATUS_LED_PIN, blinkState);
      ledPulseLastToggle = now;
      if (--ledPulseRemainingToggles <= 0) {
        ledPulseActive = false;
        blinkState = LOW;
        digitalWrite(STATUS_LED_PIN, LOW);
      }
    }
    return;
  }

  if (WiFi.getMode() & WIFI_AP) {
    digitalWrite(STATUS_LED_PIN, HIGH);
    return;
  }

  // In MODE_DAY: fast blink only when not connected; slow blink when connected
  if (powerMode == MODE_DAY) {
    if (WiFi.status() == WL_CONNECTED) {
      static unsigned long lastToggleDaySlow = 0;
      if (now - lastToggleDaySlow >= BLINK_INTERVAL) {
        blinkState = !blinkState;
        digitalWrite(STATUS_LED_PIN, blinkState);
        lastToggleDaySlow = now;
      }
    } else {
      // Calm behavior when disconnected in day mode: LED OFF
      blinkState = LOW;
      digitalWrite(STATUS_LED_PIN, LOW);
    }
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    static unsigned long lastToggleSlow = 0;
    if (now - lastToggleSlow >= BLINK_INTERVAL) {
      blinkState = !blinkState;
      digitalWrite(STATUS_LED_PIN, blinkState);
      lastToggleSlow = now;
    }
    return;
  }

  // Calm behavior when disconnected outside day mode: LED OFF
  blinkState = LOW;
  digitalWrite(STATUS_LED_PIN, LOW);
}
// ——— prepareDeepSleep(): wake on EXT0 (RTC if available) and always on ESP timer ———
void prepareDeepSleep(uint32_t wakeAfterSeconds) {
#if ENABLE_SDS011
  // Put SDS011 to sleep to extend sensor life
  if (sdsPresent && sdsAwake) {
    sdsEnsureSleep();
  }
#endif
#if ENABLE_SCD41 && defined(HAVE_SCD4X_LIB)
  if (scd41Ok) {
    scd4x.stopPeriodicMeasurement();
  }
#endif
  if (rtcOkFlag) {
    DateTime now = rtc.now();
    DateTime alarmT = now + TimeSpan(0, 0, wakeAfterSeconds / 60, 0);

    // clear any pending alarm flag
    rtc.clearAlarm(1);

    // program new alarm to fire when minute matches
    rtc.setAlarm1(alarmT, DS3231_A1_Minute);
    rtc.writeSqwPinMode(DS3231_OFF);

    // disable the 32 kHz output if you were using it
    rtc.disable32K();

    // clear the alarm flag again so the next wake is fresh
    rtc.clearAlarm(1);

    Serial.printf("⏰ RTC sleep, wake at %02d:%02d\n",
                  alarmT.hour(), alarmT.minute());

    // record last scheduled alarm time
    lastAlarmUnix = alarmT.unixtime();

    // wake on DS3231 SQW/INT pin going LOW
    esp_sleep_enable_ext0_wakeup(RTC_INT_PIN, LOW);
  } else {
    Serial.println("⏰ RTC unavailable → using timer wake only");
    lastAlarmUnix = 0;
  }

  // Safety fallback timer wake (always enabled)
  esp_sleep_enable_timer_wakeup((wakeAfterSeconds + 60) * 1000000ULL);

  // Ensure LED is off before entering deep sleep
  digitalWrite(STATUS_LED_PIN, LOW);
  esp_deep_sleep_start();
}



void performLogging() {
  unsigned long now = millis();
  // Only log when nextLogMillis (scheduled time) has arrived. Initial 0 schedules immediately.
  if ((long)(now - nextLogMillis) < 0) {
#if ENABLE_SDS011
    // Ensure SDS011 is ON and warming within the last minutes before the next log, per preset
    unsigned long timeToNext = (nextLogMillis > now) ? (nextLogMillis - now) : 0;
    if (sdsPresent) {
      // Determine pre-log ON window from preset
      unsigned long preOn = SDS_DUTY_ON_MS; // default 2 min
      if (appCfg.sdsMode == "off") preOn = 0;
      else if (appCfg.sdsMode == "pre1") preOn = 60000UL;
      else if (appCfg.sdsMode == "pre2") preOn = 120000UL;
      else if (appCfg.sdsMode == "pre5") preOn = 300000UL;
      else if (appCfg.sdsMode == "cont") preOn = 0xFFFFFFFFUL; // always on while awake

      if (timeToNext <= preOn) {
        if (!sdsAwake) {
          sdsEnsureWake();
          // reset accumulators on wake
          sdsAccumPm25RawSum = 0;
          sdsAccumPm10RawSum = 0;
          sdsAccumCount = 0;
          // If we're in the 2-minute serve window, schedule auto-sleep
          if (serveWindow == UPTIME_CONFIG) {
            sdsAutoSleepAtMs = millis() + 115000UL; // warm 30s + 85s sample ≈ 115s total
          } else {
            sdsAutoSleepAtMs = 0;
          }
        }
      } else {
        if (sdsAwake) {
          sdsEnsureSleep();
        }
      }
    }
#endif
    return;
  }
  nextLogMillis = now + LOG_INTERVAL_MS;

  // —— read sensors (BME680) ——
  float Tc = 0.0f, T = 0.0f, H = 0.0f, P = 0.0f, gasKOhm = 0.0f;

  // BSEC2 disabled, use regular BME680
  bme.performReading();
  Tc = bme.temperature;
  H = bme.humidity;
  P = bme.pressure / 100.0f;
  gasKOhm = bme.gas_resistance / 1000.0f;

  T = Tc * 9.0f / 5.0f + 32.0f;
  float L = readLux();
  float uvMv = readUvMilliVolts();
  float uvIdx = computeUvIndexFromMv(uvMv);
  analogSetPinAttenuation(ADC_BATTERY_PIN, ADC_11db);
  long acc = 0;
  const int N = 8;
  for (int i = 0; i < N; i++) {
    acc += analogRead(ADC_BATTERY_PIN);
    delay(2);
  }
  int raw = acc / N;
  float vAdc = raw * (3.3f / 4095.0f);
  float Vbat = vAdc * VOLT_DIVIDER * batCal;

  // —— timestamp —— (respect 12/24-hour preference)
  struct tm tminfo;
  char timestr[24] = "0000-00-00 00:00:00";
  time_t nowUnixTs = 0;
  if (getLocalTime(&tminfo)) {
    nowUnixTs = mktime(&tminfo);
    if (appCfg.time12h) {
      strftime(timestr, sizeof(timestr), "%Y-%m-%d %I:%M:%S %p", &tminfo);
    } else {
      strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", &tminfo);
    }
  }

  // Derived point-in-time metrics for logging
  float dewC = computeDewPointC(Tc, H);
  float dewF = dewC * 9.0f / 5.0f + 32.0f;
  float hiF = computeHeatIndexF(T, H);
  // Pressure trend from history
  time_t nowUnix = time(nullptr);
  updatePressureHistory(P, (uint32_t)nowUnix);
  float d3 = 0, d6 = 0, d12 = 0;
  bool h3 = false, h6 = false, h12 = false;
  h3 = getPressureDelta(3.0f, P, &d3);
  h6 = getPressureDelta(6.0f, P, &d6);
  h12 = getPressureDelta(12.0f, P, &d12);
  const char* trend = classifyTrendFromDelta(h3 ? d3 : (h6 ? d6 : (h12 ? d12 : 0)));

  // —— compute rain rate (last hour) once for CSV and debug ——
  const float MM_PER_TIP = 0.2794f;
  uint32_t nowMs = millis();
  uint32_t tipsLastHour = 0;
  {
    uint8_t sizeCopy, headCopy;
    uint32_t timesCopy[128];
    portENTER_CRITICAL(&rainMux);
    sizeCopy = rainTipSize;
    headCopy = rainTipHead;
    for (uint8_t i = 0; i < sizeCopy; i++) {
      int idx = (headCopy + 128 - 1 - i) % 128;
      timesCopy[i] = rainTipTimesMs[idx];
    }
    portEXIT_CRITICAL(&rainMux);
    for (uint8_t i = 0; i < sizeCopy; i++) {
      if (nowMs - timesCopy[i] <= 3600000UL) tipsLastHour++;
      else break;
    }
  }

  // —— compute wind speed (Hz → mph) from Hall ring buffer ——
#if ENABLE_WIND
  float windHz_log = 0.0f;
  {
    uint8_t sizeCopyW, headCopyW;
    uint32_t timesCopyW[128];
    uint32_t lastPulseW;
    portENTER_CRITICAL(&windMux);
    sizeCopyW = windPulseSize;
    headCopyW = windPulseHead;
    lastPulseW = lastWindPulseMs;
    for (uint8_t i = 0; i < sizeCopyW; i++) {
      int idx = (headCopyW + 128 - 1 - i) % 128;
      timesCopyW[i] = windPulseTimesMs[idx];
    }
    portEXIT_CRITICAL(&windMux);
    unsigned long nowMs2 = millis();
    const uint32_t WINDOW_MS = 1500UL;
    uint32_t cutoff = nowMs2 > WINDOW_MS ? (nowMs2 - WINDOW_MS) : 0;
    uint32_t counted = 0;
    uint32_t oldest = nowMs2;
    for (uint8_t i = 0; i < sizeCopyW; i++) {
      if (timesCopyW[i] >= cutoff) {
        counted++;
        if (timesCopyW[i] < oldest) oldest = timesCopyW[i];
      } else break;
    }
    if (counted >= 2) {
      float span = (float)(nowMs2 - oldest) / 1000.0f;
      if (span < 0.05f) span = 0.05f;
      windHz_log = ((float)counted) / span;
    } else if (counted == 1) {
      uint32_t dt = nowMs2 - timesCopyW[0];
      if (dt > 0 && dt <= WINDOW_MS) windHz_log = 1000.0f / (float)dt;
    }
  }
  float windMph_log = windHz_log * WIND_MPH_PER_HZ;
#else
  float windMph_log = 0.0f;
#endif

  // —— append to SD ——
  File f = SD.open("/logs.csv", FILE_APPEND);
  if (f) {
    // Compute MSLP and rain rate for forecast
    float mslp_hPa = computeMSLP_hPa(P, Tc, appCfg.altitudeM);
    float rainRateMmH = tipsLastHour * MM_PER_TIP;
    const char* gforecast = generalForecastFromSensors(T, H, mslp_hPa, trend, rainRateMmH, L, uvIdx);
    // CSV (extended): timestamp,temp_f,humidity,dew_f,hi_f,pressure,pressure_trend,forecast,lux,uv_mv,uv_index,voltage,voc_kohm,mslp_inHg,rain,boot_count,iaq,eco2_ppm,bvoc_ppm,pm25_ugm3,pm10_ugm3,co2_ppm,wind_mph
    float rainInHr = rainRateMmH * 0.0393700787f;
    float rainOut = appCfg.rainUnitInches ? rainInHr : rainRateMmH;
    // Append SDS011 PM values at end (µg/m³); only when sensor is ON and warmed
    bool sdsReady = sdsPresent && (millis() >= sdsWarmupUntilMs) && (sdsLastPacketMs != 0);
    float pm25Out = 0.0f, pm10Out = 0.0f;
    if (sdsReady) {
      if (sdsAccumCount > 0) {
        pm25Out = (float)sdsAccumPm25RawSum / (10.0f * (float)sdsAccumCount);
        pm10Out = (float)sdsAccumPm10RawSum / (10.0f * (float)sdsAccumCount);
      } else {
        pm25Out = sdsPm25;
        pm10Out = sdsPm10;
      }
    }
    // Do not log BSEC2 values (IAQ/eCO2/bVOC): write blank fields for these columns
    f.printf("%s,%.1f,%.1f,%.1f,%.1f,%.2f,%s,%s,%.1f,%.0f,%.1f,%.2f,%.1f,%.2f,%.2f,%lu,%s,%s,%s,%.1f,%.1f,%u,%.2f\n",
             timestr, T, H, dewF, hiF, P, trend, gforecast, L, uvMv, uvIdx, Vbat, gasKOhm, mslp_hPa * 0.0295299830714f, rainOut, (unsigned long)bootCount,
             "", "", "",
             pm25Out, pm10Out, (unsigned)scd41Co2Ppm, windMph_log);
    f.close();
    // Persist last PM values to RTC memory
#if ENABLE_SDS011
    if (sdsReady) {
      sdsLastPm25_ugm3 = pm25Out;
      sdsLastPm10_ugm3 = pm10Out;
      sdsLastPmUnix = (uint32_t)nowUnixTs;
    }
#endif
    // update in-memory and persisted last log timestamps
    lastSdLogUnix = (uint32_t)nowUnixTs;
    strncpy(lastSdLogTime, timestr, sizeof(lastSdLogTime));
    lastSdLogTime[sizeof(lastSdLogTime) - 1] = '\0';
  } else {
    Serial.println("❌ Failed to open log file");
  }

  // —— always print for debugging ——
  // Condensed log line including key sensors; avoids excessive serial spam
  Serial.printf("[LOG] %s T=%.1f%s H=%.1f%% P=%.2fhPa Lux=%.0f UV=%.1f Batt=%.2fV VOC=%.1fkΩ MSLP=%.2finHg Rain=%.2f %s PM25=%.1f PM10=%.1f CO2=%.0f Wind=%.1f mph\n",
                timestr,
                appCfg.tempF ? T : Tc,
                appCfg.tempF ? "F" : "C",
                H,
                P,
                L,
                uvIdx,
                Vbat,
                gasKOhm,
                computeMSLP_hPa(P, Tc, appCfg.altitudeM) * 0.0295299830714f,
                appCfg.rainUnitInches ? (tipsLastHour * 0.2794f * 0.0393700787f) : (tipsLastHour * 0.2794f),
                appCfg.rainUnitInches ? "in/h" : "mm/h",
                (sdsPresent && sdsAwake && millis() >= sdsWarmupUntilMs) ? sdsPm25 : 0.0f,
                (sdsPresent && sdsAwake && millis() >= sdsWarmupUntilMs) ? sdsPm10 : 0.0f,
                (int)scd41Co2Ppm,
#if ENABLE_WIND
                windMph_log
#else
                0.0f
#endif
                );
}



// —— Function Prototypes ——
bool connectToWifi();
void startAPMode();
void setupOTA();
void setupServerRoutes();
void handleRoot();
void handleLive();
void handleDownload();
void handleViewLogs();
void handleAdd();
void handleDel();
void handleReset();
void handleSleep();
void performLogging();                             // prototype
float readLux();                                   // prototype (if readLux defined later)
void updateDayNightState();                        // prototype
void prepareDeepSleep(uint32_t wakeAfterSeconds);  // prototype
void logBootEvent();                               // prototype


void updateDayNightState() {
  uint32_t now = millis();
  if (now - lastSampleMillis >= SAMPLE_INTERVAL_MS) {
    lastSampleMillis = now;
    uint32_t v = (uint32_t)readLux();
    if (luxCount < LUX_BUF_SIZE) {
      luxBuffer[luxIndex] = v;
      luxSum += v;
      luxIndex = (luxIndex + 1) % LUX_BUF_SIZE;
      luxCount++;
    } else {
      if (luxIndex >= LUX_BUF_SIZE) luxIndex = 0;  // ensure in-range before access
      luxSum -= luxBuffer[luxIndex];
      luxBuffer[luxIndex] = v;
      luxSum += v;
      luxIndex = (luxIndex + 1) % LUX_BUF_SIZE;
    }
  }
  if (!luxCount) return;
  float avgLux = (float)luxSum / luxCount;
  bool brightCond = (avgLux >= LUX_ENTER_DAY);
  bool darkCond = (avgLux <= LUX_EXIT_DAY);

  // During the initial 30 min config window, do not allow day/night
  // transitions or early deep-sleep decisions. Sampling continues so
  // UI can show live data, but decisions are deferred until after.
  bool startupWindowActive = (serveWindow == UPTIME_STARTUP) && ((now - startMillis) < (serveWindow * 1000UL));
  if (startupWindowActive) {
    return;
  }

  if (powerMode == MODE_NIGHT) {
    if (brightCond) {
      // Track sustained brightness to enter DAY
      if (!trackingBright) {
        trackingBright = true;
        conditionStartMillis = now;
      } else if (now - conditionStartMillis >= DWELL_MS) {
        powerMode = MODE_DAY;
        Serial.println("[MODE] -> DAY");
      }
    } else if (darkCond) {
      // Still dark during a night wake → let 2-minute decision window handle sleep
      if (trackingBright || conditionStartMillis == 0) {
        trackingBright = false;
        conditionStartMillis = now;
      }
    } else {
      // Neither condition sustained
      conditionStartMillis = 0;
    }
  } else {  // MODE_DAY
    if (darkCond) {
      if (trackingBright || conditionStartMillis == 0) {
        trackingBright = false;
        conditionStartMillis = now;
      } else if (now - conditionStartMillis >= DWELL_MS) {
        Serial.println("[MODE] -> NIGHT (sleep)");
        powerMode = MODE_NIGHT;
        performLogging();                      // final sunset log
        prepareDeepSleep(DEEP_SLEEP_SECONDS);  // never returns
      }
    } else conditionStartMillis = 0;
  }
}

void restartHandler() {
  String html;
  html += "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'/>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'/>";
  html += "<title>Restarting…</title>";
  html += "<style>body{background:#121212;color:#eee;font-family:Arial,sans-serif;margin:0;padding:16px;}";
  html += ".wrap{max-width:700px;margin:40px auto;text-align:center;}h2{margin:8px 0 12px;}p{opacity:.8;}";
  html += ".spinner{margin:22px auto;width:36px;height:36px;border:4px solid #333;border-top-color:#3d85c6;border-radius:50%;animation:spin 1s linear infinite;}@keyframes spin{to{transform:rotate(360deg);}}";
  html += "a.btn,button.btn{display:inline-block;background:#444;padding:10px 12px;border-radius:6px;color:#eee;text-decoration:none;margin:12px 6px 0 0;}";
  html += "</style></head><body><div class='wrap'>";
  html += "<h2>Restarting device, please wait…</h2>";
  html += "<div class='spinner'></div>";  
  html += "<p>This page will try to reconnect automatically.</p>";
  html += "<script>setTimeout(function(){location.href='/'},8000);</script>";
  html += "</div></body></html>";
  server.send(200, "text/html", html);
  delay(1200);    // allow TCP flush and page render
  ESP.restart();  // soft reboot
}


void setup() {
  // —— Serial & Status LED ——
  Serial.begin(115200);
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);
  pinMode(RTC_INT_PIN, INPUT_PULLUP);
  rtc_gpio_pullup_en(RTC_INT_PIN);
  pinMode(RAIN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(RAIN_PIN), rainIsr, FALLING);
#if ENABLE_WIND
  pinMode(WIND_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(WIND_PIN), windIsr, FALLING);
#endif
  //pinMode(FAN_PIN, OUTPUT);
  //digitalWrite(FAN_PIN, HIGH);  // fan on when you wake -------------------------------------For fan code when installed

  // Initialize I²C bus before accessing any I²C peripherals (RTC, sensors)
  Wire.begin(I2C_SDA, I2C_SCL);
#if ENABLE_SCD41 && defined(HAVE_SCD4X_LIB)
  // Quick I2C probe for SCD41 at 0x62
  Wire.beginTransmission(0x62);
  uint8_t scd41I2CErr = Wire.endTransmission();
  if (scd41I2CErr == 0) {
    Serial.println("✅ I2C address 0x62 responded (SCD41)");
  } else {
    Serial.printf("⚠️ No ACK from 0x62 (SCD41). I2C err=%u\n", scd41I2CErr);
    // Optional: show other devices present on the bus to aid debugging
    Serial.print("🔎 I2C scan: ");
    bool any = false;
    for (uint8_t addr = 1; addr < 127; addr++) {
      Wire.beginTransmission(addr);
      if (Wire.endTransmission() == 0) {
        Serial.printf("0x%02X ", addr);
        any = true;
      }
    }
    if (!any) Serial.print("(none)");
    Serial.println();
  }

  // Initialize SCD41 (SCD4x family)
  scd4x.begin(Wire);
  // Ensure we are in a clean state
  scd4x.stopPeriodicMeasurement();
  delay(5);
  // Optional: read serial to verify presence
  uint16_t sn0 = 0, sn1 = 0, sn2 = 0;
  bool present = false;
  int16_t snRet = scd4x.getSerialNumber(sn0, sn1, sn2);
  if (snRet == 0) {
    present = true;
    Serial.printf("✅ SCD41 serial: %04X-%04X-%04X\n", sn0, sn1, sn2);
  } else {
    Serial.printf("⚠️ SCD41 getSerialNumber() failed: %d\n", snRet);
  }
  // Optionally set altitude/offset here if desired
  int16_t startRet = -32768;
  if (present) startRet = scd4x.startPeriodicMeasurement();
  if (present && startRet == 0) {
    scd41Ok = true;
  } else {
    scd41Ok = false;
    Serial.printf("⚠️ SCD41 not detected or failed to start (present=%d, startRet=%d)\n", present ? 1 : 0, startRet);
  }
#endif
#if ENABLE_SDS011
  // SDS011 UART init (1 Hz frames @ 9600)
  sdsSerial.begin(9600, SERIAL_8N1, SDS_RX_PIN, SDS_TX_PIN);
  sdsPresent = true;  // assume present if UART available; adjust if needed
  // Initialize according to preset: only wake immediately when in 'cont' mode; otherwise start asleep
  {
#if defined(HAVE_SDS_LIB)
    sds.begin();
    // Prefer active continuous reporting; ignore result if unsupported
    (void)sds.setActiveReportingMode();
    (void)sds.setWorkingPeriod(0);
    if (appCfg.sdsMode == "cont") {
      sds.wakeup();
      sdsAwake = true;
      sdsWarmupUntilMs = millis() + SDS_DUTY_WARMUP_MS;
    } else {
      // For 'off' and 'pre*' presets, keep sensor asleep until the pre-log window
      sds.sleep();
      sdsAwake = false;
    }
#else
    // Library not available; reflect preset logically, physical sleep control not available
    sdsAwake = (appCfg.sdsMode == "cont");
    sdsWarmupUntilMs = sdsAwake ? (millis() + SDS_DUTY_WARMUP_MS) : 0;
#endif
  }
#endif

  // —— Wi-Fi Configuration & AP Fallback ——
  loadAppConfig();
  preferences.begin("wifi", false);
  String cfg = preferences.getString("config", "");
  if (cfg.length()) {
    DeserializationError err = deserializeJson(wifiConfig, cfg);
    if (err) {
      Serial.println("⚠️ Bad Wi-Fi config—resetting");
      wifiConfig.clear();
    }
  }
  // ensure we always have a networks array
  if (!wifiConfig.containsKey("networks")) {
    wifiConfig.createNestedArray("networks");
  }

  // now try to join; if that fails, start AP
  if (!connectToWifi()) {
    startAPMode();
  }


  // ——— in setup(): detect wake reason and pick serve window ———
  ++bootCount;
  esp_sleep_wakeup_cause_t reason = esp_sleep_get_wakeup_cause();
  Serial.print("Wakeup cause: ");
  switch (reason) {
    case ESP_SLEEP_WAKEUP_EXT0: Serial.println("RTC ALARM"); break;
    case ESP_SLEEP_WAKEUP_TIMER: Serial.println("TIMER"); break;
    case ESP_SLEEP_WAKEUP_UNDEFINED: Serial.println("COLD START 1800 WINDOW/UNDEFINED"); break;
    default: Serial.println((int)reason); break;
  }

  // —— RTC Init, 32 kHz Disable, Drift Check & Alarm Setup ——
  if (!rtc.begin()) {
    Serial.println("❌ DS3231 not found!");
    rtcOkFlag = false;
  } else {
    rtcOkFlag = true;
    // Turn off the 32 kHz output so SQW can be used for alarms
    rtc.disable32K();

    // If RTC lost power, seed from compile time and then NTP
    if (rtc.lostPower()) {
      Serial.println("⚠️ RTC lost power; setting to compile time");
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
      Serial.print("⏳ Syncing RTC from NTP…");
      configTime(0, 0, "pool.ntp.org");
      time_t nowNT = time(nullptr);
      while (nowNT < 8 * 3600 * 2) {
        setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1);
        tzset();
        delay(500);
        Serial.print('.');
        nowNT = time(nullptr);
      }
      struct tm tm;
      gmtime_r(&nowNT, &tm);
      rtc.adjust(DateTime(
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec));
      Serial.println(" ✅ RTC synced");
    }

    // Drift check against NTP‐synced system time
    DateTime rtcTime = rtc.now();
    struct tm ti;
    if (getLocalTime(&ti)) {
      time_t sys = mktime(&ti), rtcSec = rtcTime.unixtime();
      long delta = sys - rtcSec;
      if (abs(delta) > 5) {
        Serial.printf("⚠️ RTC drift = %lds\n", delta);
      }
    }
  }

  // —— Serve Window & Logging Flags —— 
  loggedThisWake = false;
  nightLogDone = false;
  scheduledNightLogMs = 0;

  // Choose serve window based on wake reason:
  if (reason == ESP_SLEEP_WAKEUP_EXT0 || reason == ESP_SLEEP_WAKEUP_TIMER) {
    // Every wake from deep sleep gets 2 minutes
    serveWindow = UPTIME_CONFIG;  // 120 seconds
    // Schedule a mid-window log ~35s after wake for sensor warm-up
    scheduledNightLogMs = millis() + 35000UL;
    // For SDS011 presets that are not continuous, wake now so we can warm/sense within the 2‑minute window
#if ENABLE_SDS011
    if (sdsPresent && appCfg.sdsMode != String("off")) {
      // Wake immediately; auto-sleep after ~115s to leave slack in the 120s window
      if (!sdsAwake) {
        sdsEnsureWake();
        sdsAccumPm25RawSum = 0;
        sdsAccumPm10RawSum = 0;
        sdsAccumCount = 0;
      }
      sdsAutoSleepAtMs = millis() + 115000UL;
    }
#endif
  } else {
    // Cold boot or manual reset → full startup window, followed by a 2-minute decision run
    serveWindow = UPTIME_STARTUP;  // 1800 seconds
    postConfigStarted = false;
  }
  Serial.printf("Serve window = %lus\n", serveWindow);


  // —— I²C & Sensor Initialization ——
  bool bmeOk = false;

//#if ENABLE_BSEC2 && defined(HAVE_BSEC2_LIB)
  // BSEC2 disabled → initialize regular Adafruit_BME680 only

  // Fallback to regular BME680 library if BSEC2 failed or is disabled
  if (!bsecOk) {
    Serial.println("🔍 Attempting regular BME680 initialization...");
    bmeOk = bme.begin(0x76);
    if (!bmeOk) bmeOk = bme.begin(0x77);
    if (!bmeOk) {
      Serial.println("❌ BME680 not found!");
    } else {
      // Configure BME680 oversampling and gas heater
      bme.setTemperatureOversampling(BME680_OS_8X);
      bme.setHumidityOversampling(BME680_OS_2X);
      bme.setPressureOversampling(BME680_OS_4X);
      bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
      bme.setGasHeater(320, 150);  // 320°C for 150 ms
      Serial.println("✅ BME680 initialized (regular library)");
    }
  }
  // VEML7700 init
  if (!lightMeter.begin()) {
    Serial.println("❌ VEML7700 not found!");
  } else {
    // Configure for maximum range (approx 0..120k lux):
    // Lowest gain and shortest integration prevent saturation in bright sun
    lightMeter.setGain(VEML7700_GAIN_1_8);
    lightMeter.setIntegrationTime(VEML7700_IT_25MS);
    Serial.println("✅ VEML7700 initialized");
  }

  // —— SD Card Mount ——
  if (!SD.begin(SD_CS)) {
    Serial.println("❌ SD Card mount failed");
  } else {
    Serial.println("✅ SD Card mounted");
  }

  // —— Initial Light Buffer & First Log ——
  float firstLux = readLux();
  luxBuffer[0] = (uint32_t)firstLux;
  luxSum = luxBuffer[0];
  luxIndex = 1;
  luxCount = 1;
  lastSampleMillis = millis();
  trackingBright = (firstLux >= LUX_ENTER_DAY);
  conditionStartMillis = trackingBright ? millis() : 0;
  // Ensure timezone/NTP config is set before first log so timestamps are correct
  configTzTime("EST5EDT,M3.2.0/2,M11.1.0/2", "pool.ntp.org");
  // Record boot time once per boot
  bootStartUnix = time(nullptr);
  performLogging();
  // Also record a boot event row only on cold start (not RTC/timer wakes)
  if (reason != ESP_SLEEP_WAKEUP_EXT0 && reason != ESP_SLEEP_WAKEUP_TIMER) {
    struct tm ti;
    char timestrBoot[32] = "N/A";
    if (getLocalTime(&ti)) {
      if (appCfg.time12h) strftime(timestrBoot, sizeof(timestrBoot), "%Y-%m-%d %I:%M:%S %p", &ti);
      else strftime(timestrBoot, sizeof(timestrBoot), "%Y-%m-%d %H:%M:%S", &ti);
    }
    File fboot = SD.open("/logs.csv", FILE_APPEND);
    if (fboot) {
      // Extended schema: timestamp,temp_f,humidity,dew_f,hi_f,pressure,pressure_trend,forecast,lux,uv_mv,uv_index,voltage,voc_kohm,mslp_inHg,rain,boot_count,iaq,eco2_ppm,bvoc_ppm
      // For boot row we leave numeric fields empty and include boot_count only
      fboot.printf("%s,,,,,,,,,,,,,,,%lu\n", timestrBoot, (unsigned long)bootCount);
      fboot.close();
    }
  }
  // Avoid double-log on first /live after setup
  loggedThisWake = true;
  nextLogMillis = millis() + LOG_INTERVAL_MS;

  // —— OTA ——
  setupOTA();

  // —— HTTP Server & mDNS ——
  setupServerRoutes();
  server.begin();
  Serial.println("✅ HTTP server started");
  {
    String host = sanitizeMdnsHost(appCfg.mdnsHost.length() ? appCfg.mdnsHost : String("weatherstation1"));
    appCfg.mdnsHost = host;
    if (MDNS.begin(host.c_str())) {
      Serial.printf("✅ mDNS responder started: %s.local\n", host.c_str());
    } else {
      Serial.println("⚠️ mDNS failed");
    }
  }


  // —— Record Start Timestamp ——
  startMillis = millis();
}

// ——— CONNECT TO SAVED NETWORKS ———
bool connectToWifi() {
  JsonArray nets = wifiConfig["networks"].as<JsonArray>();

  // Ensure clean STA state before trying
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(true, true);
  delay(200);

  Serial.println("⏳ Scanning for Wi‑Fi networks…");
  int found = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true);
  Serial.printf("🔎 Found %d networks\n", found);

  auto tryConnect = [&](const char* ssid, const char* pass) -> bool {
    Serial.printf("→ '%s' …\n", ssid);
    blinkStatus(1, 100);

    WiFi.disconnect(true, true);
    delay(150);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);

    unsigned long start = millis();
    const unsigned long timeoutMs = 15000UL;  // 15s per network
    wl_status_t st;
    do {
      st = WiFi.status();
      if (st == WL_CONNECTED) break;
      delay(200);
    } while (millis() - start < timeoutMs);

    if (WiFi.status() == WL_CONNECTED) {
      WiFi.setAutoReconnect(true);  // enable auto-reconnect after first success
      Serial.print("✅ Connected, IP: ");
      Serial.println(WiFi.localIP());
      blinkStatus(5, 100);
      return true;
    }
    Serial.printf("❌ Failed (%u)\n", (unsigned)st);
    return false;
  };

  // Pass 1: try saved networks that are currently visible, strongest first
  if (found > 0) {
    for (int i = 0; i < found; ++i) {
      String vis = WiFi.SSID(i);
      for (JsonObject net : nets) {
        const char* ssid = net["ssid"];
        const char* pass = net["pass"];
        if (vis == String(ssid)) {
          if (tryConnect(ssid, pass)) {
            WiFi.scanDelete();
            return true;
          }
        }
      }
    }
  }

  // Pass 2: try remaining saved networks even if not visible (in case of scan miss)
  for (JsonObject net : nets) {
    const char* ssid = net["ssid"];
    const char* pass = net["pass"];
    bool visible = false;
    for (int i = 0; i < found; ++i) {
      if (String(ssid) == WiFi.SSID(i)) {
        visible = true;
        break;
      }
    }
    if (!visible) {
      if (tryConnect(ssid, pass)) {
        WiFi.scanDelete();
        return true;
      }
    }
  }

  WiFi.scanDelete();
  Serial.println("⚠️ All join attempts failed.");
  return false;
}

// —— Define HTTP Routes ——
void setupServerRoutes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/live", HTTP_GET, handleLive);
  server.on("/download", HTTP_GET, handleDownload);
  server.on("/view-logs", HTTP_GET, handleViewLogs);
  // Simple config page: GET shows current, POST saves
  server.on("/config", HTTP_GET, []() {
    String html;
    html += "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'/><meta name='viewport' content='width=device-width,initial-scale=1'/>";
    html += "<title>Config Settings</title>";
    html += "<style>body{background:#121212;color:#eee;font-family:Arial,sans-serif;margin:0;padding:16px;}";
    html += ".wrap{max-width:700px;margin:0 auto;}h2{margin:8px 0 16px;}label{display:block;margin:12px 0 6px;}";
    html += "input,select,button{background:#1e1e1e;color:#eee;border:none;border-radius:6px;padding:10px;}";
    html += "button{cursor:pointer;background:#3d85c6;margin-top:16px;}a.btn{display:inline-block;background:#444;padding:10px 12px;border-radius:6px;color:#eee;text-decoration:none;margin-top:16px;}";
    html += ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:12px;}";
    html += "</style></head><body><div class='wrap'>";
    html += "<h2>Config Settings</h2>";
    html += "<form method='POST' action='/config'>";
    html += "<div class='grid'>";
    html += "<div><label>Altitude (m)</label><input name='alt' value='" + String(appCfg.altitudeM, 1) + "'><div style='opacity:.7;font-size:.9em;margin-top:4px;'>Used to compute sea‑level pressure (MSLP).</div></div>";
    html += "<div><label>Temperature Unit</label><select name='tu'><option value='F'" + String(appCfg.tempF ? " selected" : "") + ">Fahrenheit</option><option value='C'" + String(!appCfg.tempF ? " selected" : "") + ">Celsius</option></select><div style='opacity:.7;font-size:.9em;margin-top:4px;'>Affects displayed units; raw calculations include both.</div></div>";
    // Pressure unit setting removed; UI shows both Pressure (hPa) and MSLP (inHg)
    html += "<div><label>Battery Calibration</label><input name='bc' value='" + String(appCfg.batCal, 2) + "' title='Multiply measured battery voltage by this factor'><div style='opacity:.7;font-size:.9em;margin-top:4px;'>Multiplier applied to ADC‑derived pack voltage.</div></div>";
    html += "<div><label>Clock Format</label><select name='tf'><option value='24'" + String(!appCfg.time12h ? " selected" : "") + ">24‑hour</option><option value='12'" + String(appCfg.time12h ? " selected" : "") + ">12‑hour</option></select><div style='opacity:.7;font-size:.9em;margin-top:4px;'>Affects timestamps in UI and CSV.</div></div>";
    html += "<div><label>Daylight entry (lux)</label><input name='led' value='" + String(appCfg.luxEnterDay, 0) + "'><div style='opacity:.7;font-size:.9em;margin-top:4px;'>Average lux to switch to DAY (stay awake).</div></div>";
    html += "<div><label>Night entry (lux)</label><input name='lxd' value='" + String(appCfg.luxExitDay, 0) + "'><div style='opacity:.7;font-size:.9em;margin-top:4px;'>Average lux to switch back to NIGHT (deep sleep).</div></div>";
    html += "<div><label>Log interval (minutes)</label><input name='lim' value='" + String(appCfg.logIntervalMin) + "'><div style='opacity:.7;font-size:.9em;margin-top:4px;'>CSV cadence while awake (DAY mode).</div></div>";
    html += "<div><label>Sleep duration (minutes)</label><input name='slm' value='" + String(appCfg.sleepMinutes) + "'><div style='opacity:.7;font-size:.9em;margin-top:4px;'>Deep sleep duration between wakes in NIGHT mode.</div></div>";
    html += "<div><label>Pressure trend threshold (hPa)</label><input name='pth' value='" + String(appCfg.trendThresholdHpa, 1) + "'><div style='opacity:.7;font-size:.9em;margin-top:4px;'>ΔP over several hours to classify Rising/Falling.</div></div>";
    html += "<div><label>Rain unit</label><select name='ru'><option value='mm'" + String(!appCfg.rainUnitInches ? " selected" : "") + ">mm/h</option><option value='in'" + String(appCfg.rainUnitInches ? " selected" : "") + ">in/h</option></select><div style='opacity:.7;font-size:.9em;margin-top:4px;'>Affects CSV and UI rain rate.</div></div>";
    html += "<div><label>mDNS Hostname (no .local)</label><input name='mdns' value='" + htmlEscape(appCfg.mdnsHost.length() ? appCfg.mdnsHost : String("weatherstation1")) + "' title='Letters, numbers, hyphen; becomes host.local'></div>";
    html += "<div><label>Dust sensor duty</label><select name='sds'>";
    html += String("<option value='off'") + (appCfg.sdsMode == "off" ? " selected" : "") + ">Off</option>";
    html += String("<option value='pre1'") + (appCfg.sdsMode == "pre1" ? " selected" : "") + ">1 min before log</option>";
    html += String("<option value='pre2'") + (appCfg.sdsMode == "pre2" ? " selected" : "") + ">2 min before log</option>";
    html += String("<option value='pre5'") + (appCfg.sdsMode == "pre5" ? " selected" : "") + ">5 min before log</option>";
    html += String("<option value='cont'") + (appCfg.sdsMode == "cont" ? " selected" : "") + ">Continuous while awake</option>";
    html += "</select><div style='opacity:.7;font-size:.9em;margin-top:4px;'>Controls SDS011 runtime to extend lifespan.</div></div>";
    html += "<div><label>Verbose debug</label><select name='dbg'>";
    html += String("<option value='0'") + (!appCfg.debugVerbose ? " selected" : "") + ">No</option>";
    html += String("<option value='1'") + (appCfg.debugVerbose ? " selected" : "") + ">Yes</option>";
    html += "</select><div style='opacity:.7;font-size:.9em;margin-top:4px;'>Reduces serial spam when off.</div></div>";
    html += "</div><button type='submit'>Save Settings</button></form>";
    html += "<p><a class='btn' href='/'>← Back to Dashboard</a></p>";
    html += "</div></body></html>";
    server.send(200, "text/html", html);
  });
  server.on("/config", HTTP_POST, []() {
    if (server.hasArg("alt")) appCfg.altitudeM = server.arg("alt").toFloat();
    if (server.hasArg("tu")) appCfg.tempF = (server.arg("tu") == "F");
    // Pressure unit setting removed
    if (server.hasArg("bc")) appCfg.batCal = server.arg("bc").toFloat();
    if (server.hasArg("tf")) appCfg.time12h = (server.arg("tf") == "12");
    if (server.hasArg("led")) appCfg.luxEnterDay = server.arg("led").toFloat();
    if (server.hasArg("lxd")) appCfg.luxExitDay = server.arg("lxd").toFloat();
    if (server.hasArg("lim")) appCfg.logIntervalMin = (uint16_t)server.arg("lim").toInt();
    if (server.hasArg("slm")) appCfg.sleepMinutes = (uint16_t)server.arg("slm").toInt();
    if (server.hasArg("pth")) appCfg.trendThresholdHpa = server.arg("pth").toFloat();
    if (server.hasArg("ru")) appCfg.rainUnitInches = (server.arg("ru") == "in");
    // SDS011 duty
    if (server.hasArg("sds")) appCfg.sdsMode = server.arg("sds");
    // Verbose debug
    if (server.hasArg("dbg")) appCfg.debugVerbose = (server.arg("dbg") == "1");
    // mDNS host update
    if (server.hasArg("mdns")) {
      String requested = sanitizeMdnsHost(server.arg("mdns"));
      if (!requested.equalsIgnoreCase(appCfg.mdnsHost)) {
        appCfg.mdnsHost = requested;
        // Restart mDNS with new hostname
        MDNS.end();
        if (MDNS.begin(appCfg.mdnsHost.c_str())) {
          Serial.printf("✅ mDNS changed to: %s.local\n", appCfg.mdnsHost.c_str());
        } else {
          Serial.println("⚠️ mDNS restart failed after change");
        }
      }
    }
    saveAppConfig();
    server.sendHeader("Location", "/config", true);
    server.send(302, "text/plain", "");
  });
  server.on("/add", HTTP_POST, handleAdd);
  server.on("/del", HTTP_GET, handleDel);
  server.on("/reset", HTTP_POST, handleReset);
  server.on("/sleep", HTTP_POST, handleSleep);
  server.on("/restart", HTTP_POST, restartHandler);
  server.on("/restart", HTTP_GET, []() {
    server.send(200, "text/plain", "Restarting Device …");
    delay(200);
    ESP.restart();
  });
  server.onNotFound([]() {
    server.send(404, "text/plain", "404 Not Found");
  });
}


// === Updated handleRoot(): integrates "Last Updated" card and JS update ===
void handleRoot() {
  // Prepare array of saved networks
  JsonArray jsonDoc = wifiConfig["networks"].as<JsonArray>();

  // Static part up to the <div id="cards">…
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<style>
  body {
    background:#121212; color:#eee;
    font-family:Arial,sans-serif;
    margin:0; padding:0;
    display:flex; flex-direction:column;
    align-items:center;
  }
  h2,h3 { margin:16px; }
  #refreshBtn {
    background:#3d85c6; color:#fff;
    border:none; padding:8px 16px;
    border-radius:6px; cursor:pointer;
    transition:transform .2s;
  }
  #refreshBtn:hover { transform:scale(1.05); }
  a.btn, button.btn {
    display:inline-block; background:#3d85c6; color:#fff;
    padding:10px 12px; border-radius:6px; text-decoration:none;
    margin:12px 6px 0 0; border:none; cursor:pointer;
    transition:transform .2s;
  }
  a.btn:hover, button.btn:hover { text-decoration:none; transform:scale(1.05); }
  /* Layout wrapper for legend + cards */
  #mainwrap { display:grid; grid-template-columns: 280px 1fr; gap:16px; width:90%; max-width:1200px; }
  #legend { background:#1e1e1e; padding:16px; border-radius:6px; height:fit-content; position:sticky; top:8px; align-self:start; }
  #legend h3 { margin:0 0 8px; font-size:1.05em; }
  #legend .item { margin:8px 0; }
  #legend .item b { display:block; color:#ccc; font-weight:600; margin-bottom:2px; }
  #legend .item span { color:#aaa; font-size:.92em; }
  @media (max-width: 900px){ #mainwrap{ grid-template-columns: 1fr; } #legend{ order:2; } }
  #cards {
    display:grid;
    grid-template-columns:repeat(auto-fit,minmax(180px,1fr));
    gap:12px; width:90%; max-width:1000px;
    margin:16px 0;
  }
  .card {
    background:#1e1e1e; padding:16px;
    border-radius:6px; text-align:center;
  }
  .card h4 { margin:0 0 8px; font-size:1em; }
  .card .value { font-size:1.5em; margin-bottom:8px; }
  .card canvas {
    width:100%; height:100px;
    background:#222; border-radius:4px;
  }
  #networks {
    background:#1e1e1e; padding:16px;
    border-radius:6px; width:90%; max-width:600px;
    margin-bottom:24px;
  }
  #networks form,#networks ul,#networks button {
    margin-top:12px;
  }
  input {
    padding:6px; border:none; border-radius:4px;
    margin-right:8px; background:#2a2a2a; color:#eee;
  }
  ul { list-style:none; padding:0; }
  li { margin:4px 0; }
  a { color:#3d85c6; text-decoration:none; margin-left:8px; }
  a:hover { text-decoration:underline; }
</style>
<title>ESP32 Weather Station Dashboard</title>
</head><body>
  <h2>Live Weather Station Dashboard</h2>
  <button id="refreshBtn">Refresh Now</button>
  <div id="mainwrap">
    <div id="legend">
      <h3>Legend</h3>
      <div class="item"><b>Temp</b><span>Ambient air temperature. Unit follows Config (°F/°C).</span></div>
      <div class="item"><b>Hum</b><span>Relative Humidity (%).</span></div>
      <div class="item"><b>Pressure</b><span>Station pressure (hPa). MSLP shows sea-level pressure (inHg).</span></div>
      <div class="item"><b>Lux</b><span>Illuminance (ambient light). Higher = brighter daylight.</span></div>
      <div class="item"><b>UV Index</b><span>Approximate UV index estimated from GUVA sensor.</span></div>
      <div class="item"><b>VOC (kΩ)</b><span>BME680 gas sensor proxy (higher = cleaner air, relative).</span></div>
      <div class="item"><b>PM2.5 (µg/m³)</b><span>Good ≤12; Moderate 12–35; Unhealthy (SG) 35–55; Unhealthy >55.</span></div>
      <div class="item"><b>PM10 (µg/m³)</b><span>Good ≤54; Moderate 55–154; Unhealthy (SG) 155–254; Unhealthy >254.</span></div>
      <div class="item"><b>CO₂ (ppm)</b><span>Good ≤800; Moderate 800–1200; High 1200–2000; Very High >2000.</span></div>
      <div class="item"><b>Rain</b><span>Rate in mm/h or in/h (Config). Based on tipping bucket last hour.</span></div>
      <div class="item"><b>Batt (V)</b><span>Battery voltage; header shows estimated %.</span></div>
      <div class="item"><b>Dew/Heat/Wet Bulb</b><span>Derived comfort metrics from Temp and Humidity.</span></div>
      <div class="item"><b>Trend/Forecast</b><span>Pressure trend over time and brief outlook.</span></div>
    </div>
    <div id="cards">
    <!-- Row 1 -->
    <div class="card">
      <h4>Temp (F)</h4>
      <div id="tempVal" class="value">--</div>
      <canvas id="tempChart"></canvas>
    </div>
    <div class="card">
      <h4>Hum (%)</h4>
      <div id="humVal" class="value">--</div>
      <canvas id="humChart"></canvas>
    </div>
    <div class="card">
      <h4>Pressure (hPa)</h4>
      <div id="pressureVal" class="value">--</div>
      <canvas id="pressureChart"></canvas>
    </div>
    <div class="card">
      <h4>MSLP (Sea Level Pressure)</h4>
      <div id="mslpVal" class="value">--</div>
      <canvas id="mslpChart"></canvas>
    </div>
    <div class="card">
      <h4>Lux</h4>
      <div id="luxVal" class="value">--</div>
      <canvas id="luxChart"></canvas>
    </div>
    <div class="card">
      <h4>UV Index</h4>
      <div id="uvVal" class="value">--</div>
      <canvas id="uvChart"></canvas>
    </div>
    <div class="card">
      <h4>VOC (kΩ)</h4>
      <div id="vocVal" class="value">--</div>
      <canvas id="vocChart"></canvas>
    </div>
    <div class="card">
      <h4>CO₂ (ppm)</h4>
      <div id="co2Val" class="value">--</div>
      <canvas id="co2Chart"></canvas>
    </div>
    <div class="card">
      <h4>PM2.5 (µg/m³)</h4>
      <div id="pm25Val" class="value">--</div>
      <canvas id="pm25Chart"></canvas>
    </div>
    <div class="card">
      <h4>PM10 (µg/m³)</h4>
      <div id="pm10Val" class="value">--</div>
      <canvas id="pm10Chart"></canvas>
    </div>
    <div class="card">
      <h4>Rain (mm/h)</h4>
      <div id="rainVal" class="value">--</div>
      <canvas id="rainChart"></canvas>
    </div>
    <div class="card">
      <h4>Wind (mph)</h4>
      <div id="windVal" class="value">--</div>
      <canvas id="windChart"></canvas>
    </div>
    <div class="card">
      <h4>Wind Avg 1h (mph)</h4>
      <div id="windAvgVal" class="value">--</div>
      <canvas id="windAvgChart"></canvas>
    </div>
    <div class="card">
      <h4>Batt (V)</h4>
      <div id="batVal" class="value">--</div>
      <canvas id="battChart"></canvas>
    </div>

    <!-- Row 2 -->
    <div class="card">
      <h4>Dew Point (°F)</h4>
      <div id="dewVal" class="value">--</div>
      <canvas id="dewChart"></canvas>
    </div>
    <div class="card">
      <h4>Heat Index (°F)</h4>
      <div id="hiVal" class="value">--</div>
      <canvas id="hiChart"></canvas>
    </div>
    <div class="card">
      <h4>Pressure Trend</h4>
      <div id="trendVal" class="value">--</div>
    </div>
    <div class="card">
      <h4>Forecast</h4>
      <div id="forecastVal" class="value">--</div>
      <div id="forecastDetail" style="opacity:.8;font-size:.9em;margin-top:6px;">&nbsp;</div>
    </div>
    <div class="card">
      <h4>Wet Bulb Temp (°F)</h4>
      <div id="wbtVal" class="value">--</div>
      <canvas id="wbtChart"></canvas>
    </div>
    <div class="card">
      <h4>SSID</h4>
      <div id="ssidVal" class="value">--</div>
    </div>

    <!-- Row 3 -->
    <div class="card">
      <h4>Wake Cause</h4>
      <div id="wakeVal" class="value">--</div>
    </div>
    <div class="card">
      <h4>Last Alarm</h4>
      <div id="alarmVal" class="value">--</div>
    </div>
    <div class="card">
      <h4>Last SD Log</h4>
      <div id="lastSdLog" class="value">--</div>
    </div>
    <div class="card">
      <h4>Last Updated</h4>
      <div id="lastUpdated" class="value">--</div>
    </div>
    <div class="card">
      <h4>WiFi RSSI</h4>
      <div id="rssiVal" class="value">--</div>
    </div>

    <!-- Row 4 -->
    <div class="card">
      <h4>Heap (bytes)</h4>
      <div id="heapVal" class="value">--</div>
    </div>
    <div class="card">
      <h4>Flash Free (KB)</h4>
      <div id="psramVal" class="value">--</div>
    </div>
    <div class="card">
      <h4>SD Free (KB)</h4>
      <div id="sdfreeVal" class="value">--</div>
    </div>
    <div class="card">
      <h4>Started</h4>
      <div id="bootStarted" class="value">--</div>
    </div>
    <div class="card">
      <h4>RTC OK?</h4>
      <div id="rtcStatus" class="value">--</div>
    </div>

    <!-- Row 5 -->
    <div class="card">
      <h4>Uptime</h4>
      <div id="uptimeVal" class="value">--:--:--</div>
    </div>
    <div class="card">
      <h4>SD OK?</h4>
      <div id="sdStatus" class="value">--</div>
    </div>
    <div class="card">
      <h4>BSEC2 OK?</h4>
      <div id="bsecStatus" class="value">--</div>
    </div>
    <div class="card">
      <h4>Boot Count</h4>
      <div id="bootCountVal" class="value">--</div>
    </div>
    </div>
  </div>

  <div id="networks">
    <h3>Manage Wi-Fi Networks</h3>
    <form action="/add" method="POST">
      <input name="ssid"  placeholder="SSID" required>
      <input name="pass"  placeholder="Password" required>
      <button class='btn' type="submit">Add</button>
    </form>
    <ul>")rawliteral";

  // Dynamically list saved networks
  for (JsonObject net : jsonDoc) {
    String ss = net["ssid"].as<const char*>();
    String esc = htmlEscape(ss);
    String url = urlEncode(ss);
    html += "<li>" + esc + " <a href='/del?ssid=" + url + "' onclick=\"return confirm('Delete this network?')\">Del</a></li>";
  }

  html += R"rawliteral(
    </ul>
    <a class='btn' href='/download'>Download CSV</a>
    <form action="/reset" method="POST" onsubmit="return confirm('Clear logs?')" style='display:inline-block;margin-top:12px;'>
      <button class='btn' type="submit">Clear Logs</button>
    </form>
    <a class='btn' href='/view-logs'>View Logs</a>
    <a class='btn' href='/update'>OTA Update</a>
    <form action="/sleep" method="POST" onsubmit="return confirm('Really enter deep sleep now?')" style='display:inline-block;margin-top:12px;'>
      <button class='btn' type="submit">Force Sleep</button>
    </form>
    <form action="/restart" method="POST" onsubmit="return confirm('Restart device?')" style='display:inline-block;margin-top:12px;'>
      <button class='btn' type="submit">Restart Device</button>
    </form>
    <a class='btn' href='/config'>Config Settings</a>
  </div>

<script>
let liveData = [], MAX_POINTS = 180;

// Map RSSI (dBm) to bar glyphs ▂▄▅▆▇ in 20% steps
function rssiToBars(rssi, connected){
  if (!connected) return '▂';
  let q = Math.max(0, Math.min(100, Math.round(2 * (rssi + 100)))); // -100..-50 → 0..100
  if (q >= 100) return '▂▄▅▆▇';
  if (q >= 80)  return '▂▄▅▆';
  if (q >= 60)  return '▂▄▅';
  if (q >= 40)  return '▂▄';
  return '▂';
}

// Estimate Li‑ion SoC (%) from voltage (single cell) using a piecewise linear curve
function liIonPercent(v){
  // Clamp to typical range 3.20V..4.20V
  const cl = Math.max(3.2, Math.min(4.2, v));
  // Breakpoints (V → %) based on typical resting curve
  const bp = [
    [3.20, 0], [3.30, 10], [3.40, 20], [3.50, 30], [3.60, 40],
    [3.70, 50], [3.80, 60], [3.90, 70], [4.00, 80], [4.10, 90], [4.20, 100]
  ];
  for (let i = 1; i < bp.length; i++){
    const [v0,p0] = bp[i-1], [v1,p1] = bp[i];
    if (cl <= v1){
      const t = (cl - v0) / (v1 - v0);
      return Math.round(p0 + t * (p1 - p0));
    }
  }
  return 100;
}

// Fetch live readings and update UI + charts
async function fetchLive() {
  const o = await (await fetch('/live')).json();
  const isF = (o.temp_unit === 'F');
  document.getElementById('tempVal').textContent     = (o.temp ?? (isF ? o.temp_f : o.temp_c)).toFixed(1);
  try {
    const tempHeader = document.getElementById('tempVal').parentElement.querySelector('h4');
    if (tempHeader) tempHeader.textContent = `Temp (${isF ? 'F' : 'C'})`;
  } catch (e) { /* noop */ }
  document.getElementById('humVal').textContent      = o.hum.toFixed(1);
  document.getElementById('pressureVal').textContent = o.pressure.toFixed(2);
  document.getElementById('luxVal').textContent      = o.lux.toFixed(0);
  if (document.getElementById('uvVal')) {
    document.getElementById('uvVal').textContent = (o.uv_index ?? 0).toFixed(1);
  }
  if (document.getElementById('vocVal')) {
    document.getElementById('vocVal').textContent      = (o.voc_kohm ?? 0).toFixed(1);
  }
  // IAQ/eCO2/bVOC removed
  if (document.getElementById('co2Val')) {
    document.getElementById('co2Val').textContent     = (o.co2_ppm ?? 0).toFixed(0);
  }
  // IAQ/eCO2/bVOC removed
  if (document.getElementById('pm25Val')) {
    // Persist last nonzero PM2.5 value while sensor sleeps/warms
    window.__lastPm25 = (typeof window.__lastPm25 === 'number') ? window.__lastPm25 : 0;
    const pm25Now = (o.sds_awake && o.sds_warm) ? (o.pm25_ugm3 ?? 0) : window.__lastPm25;
    if (o.sds_awake && o.sds_warm && (o.pm25_ugm3 ?? 0) > 0) window.__lastPm25 = o.pm25_ugm3;
    document.getElementById('pm25Val').textContent = Number(pm25Now).toFixed(1);
  }
  if (document.getElementById('pm10Val')) {
    // Persist last nonzero PM10 value while sensor sleeps/warms
    window.__lastPm10 = (typeof window.__lastPm10 === 'number') ? window.__lastPm10 : 0;
    const pm10Now = (o.sds_awake && o.sds_warm) ? (o.pm10_ugm3 ?? 0) : window.__lastPm10;
    if (o.sds_awake && o.sds_warm && (o.pm10_ugm3 ?? 0) > 0) window.__lastPm10 = o.pm10_ugm3;
    document.getElementById('pm10Val').textContent = Number(pm10Now).toFixed(1);
  }
  document.getElementById('batVal').textContent      = o.batt.toFixed(2);

  // Update Batt (V) header with estimated %
  try {
    const pct = liIonPercent(o.batt ?? 0);
    const battHeader = document.getElementById('batVal').parentElement.querySelector('h4');
    if (battHeader && isFinite(pct)) battHeader.textContent = `Batt (V)(${pct}%)`;
  } catch (e) { /* noop */ }

  let s = o.uptime;
  let h = Math.floor(s/3600), m = Math.floor((s%3600)/60), sec = s%60;
  document.getElementById('uptimeVal').textContent =
    `${String(h).padStart(2,'0')}:${String(m).padStart(2,'0')}:${String(sec).padStart(2,'0')}`;

  document.getElementById('heapVal').textContent  = o.heap;
  document.getElementById('psramVal').textContent = (o.flash_free_kb ?? 0);
  document.getElementById('lastUpdated').textContent = o.time;
  document.getElementById('bootStarted').textContent = (o.boot_started ?? '');
  document.getElementById('lastSdLog').textContent = (o.last_sd_log ?? '');
  document.getElementById('rssiVal').textContent = rssiToBars(o.rssi ?? -100, (o.ssid ?? '').length > 0);
  document.getElementById('ssidVal').textContent = (o.ssid ?? '');
  document.getElementById('wakeVal').textContent = (o.wakeup_cause_text ?? '');
  document.getElementById('alarmVal').textContent = (o.last_alarm ?? '');
  document.getElementById('sdfreeVal').textContent = (o.sd_free_kb ?? 0);
  document.getElementById('dewVal').textContent = (isF ? (o.dew_f ?? 0) : (o.dew_c ?? 0)).toFixed(1);
  document.getElementById('hiVal').textContent  = (isF ? (o.hi_f  ?? 0) : (o.hi_c  ?? 0)).toFixed(1);
  document.getElementById('mslpVal').textContent = (o.mslp_inHg ?? 0).toFixed(2);
  document.getElementById('wbtVal').textContent = (isF ? (o.wbt_f ?? 0) : (o.wbt_c ?? 0)).toFixed(1);
  try {
    const dewHdr = document.getElementById('dewVal').parentElement.querySelector('h4');
    if (dewHdr) dewHdr.textContent = `Dew Point (°${isF ? 'F' : 'C'})`;
    const hiHdr = document.getElementById('hiVal').parentElement.querySelector('h4');
    if (hiHdr) hiHdr.textContent = `Heat Index (°${isF ? 'F' : 'C'})`;
    const wbtHdr = document.getElementById('wbtVal').parentElement.querySelector('h4');
    if (wbtHdr) wbtHdr.textContent = `Wet Bulb Temp (°${isF ? 'F' : 'C'})`;
  } catch (e) { /* noop */ }
  if (document.getElementById('rainVal')) {
    const useIn = (o.rain_unit === 'in/h');
    const val = useIn ? (o.rain_inph ?? (o.rain_mmph ?? 0) * 0.0393700787) : (o.rain_mmph ?? 0);
    document.getElementById('rainVal').textContent = val.toFixed(2);
    try {
      const rainHdr = document.getElementById('rainVal').parentElement.querySelector('h4');
      if (rainHdr) rainHdr.textContent = `Rain (${useIn ? 'in/h' : 'mm/h'})`;
    } catch (e) { /* noop */ }
  }
  if (document.getElementById('windVal')) {
    document.getElementById('windVal').textContent = Number(o.wind_mph ?? 0).toFixed(1);
  }
  if (document.getElementById('windAvgVal')) {
    document.getElementById('windAvgVal').textContent = Number(o.wind_avg_mph_1h ?? 0).toFixed(1);
  }
  document.getElementById('trendVal').textContent = (o.pressure_trend ?? '');
  if (document.getElementById('forecastVal')) {
    document.getElementById('forecastVal').textContent = (o.general_forecast ?? '');
  }
  if (document.getElementById('forecastDetail')) {
    const base = (o.forecast ?? o.general_forecast ?? '');
    const det = (o.forecast_detail ?? '');
    document.getElementById('forecastDetail').textContent = det || base;
  }
  document.getElementById('sdStatus').innerHTML    =
    o.sd_ok    ? '<span style="color:#0f0">&#10003;</span>' 
               : '<span style="color:#f00">&#10007;</span>';

  document.getElementById('rtcStatus').innerHTML   =
    o.rtc_ok   ? '<span style="color:#0f0">&#10003;</span>' 
               : '<span style="color:#f00">&#10007;</span>';

  // BSEC status removed

  document.getElementById('bootCountVal').textContent =
    o.boot_count;


  liveData.push({
    temp: o.temp,
    hum: o.hum,
    pressure: o.pressure,
    mslp: o.mslp_inHg ?? o.mslp_hPa,
    dew: isF ? (o.dew_f ?? 0) : (o.dew_c ?? 0),
    hi:  isF ? (o.hi_f  ?? 0) : (o.hi_c  ?? 0),
    wbt: isF ? (o.wbt_f ?? 0) : (o.wbt_c ?? 0),
    lux: o.lux,
    uv: (o.uv_index ?? 0),
    batt: o.batt,
    voc: (o.voc_kohm ?? 0),
    co2: (o.co2_ppm ?? 0),
    pm25: (o.pm25_ugm3 ?? 0),
    pm10: (o.pm10_ugm3 ?? 0),
    rain: (o.rain_mmph ?? 0),
    wind: (o.wind_mph ?? 0),
    wind_avg: (o.wind_avg_mph_1h ?? 0),
    // IAQ/eCO2/bVOC removed
  });
  if (liveData.length > MAX_POINTS) liveData.shift();

  // Only push fresh PM points when awake+warmed; else repeat last value by not changing it
  ['temp','hum','pressure','mslp','dew','hi','wbt','lux','uv','batt','voc','co2','rain','wind'].forEach(f => {
    draw(f, f + 'Chart', liveData);
  });
  draw('wind_avg', 'windAvgChart', liveData);
  if (o.sds_awake && o.sds_warm) {
    draw('pm25', 'pm25Chart', liveData);
    draw('pm10', 'pm10Chart', liveData);
  }
}

// Generic line‐drawing on a <canvas>
function draw(field, id, data) {
  const c = document.getElementById(id);
  if (!c || !c.getContext) return; // Gracefully skip missing canvases
  const ctx = c.getContext('2d'),
        w = c.width, h = c.height, pad = 20;
  ctx.clearRect(0, 0, w, h);
  if (!data.length) return;
  const vals = data.map(d => d[field]);
  const min  = Math.min(...vals), max = Math.max(...vals);
  ctx.beginPath();
  data.forEach((d,i) => {
    const x = pad + (i/(data.length-1))*(w-2*pad),
          y = pad + (1-((d[field]-min)/(max-min||1)))*(h-2*pad);
    i ? ctx.lineTo(x,y) : ctx.moveTo(x,y);
  });
  ctx.strokeStyle = '#3d85c6';
  ctx.lineWidth = 2;
  ctx.stroke();
}

window.onload = () => {
  fetchLive();
  setInterval(fetchLive, 2000);
  document.getElementById('refreshBtn').onclick = fetchLive;
};
</script>
</body></html>
)rawliteral";

  server.send(200, "text/html", html);
}


// —— Live JSON Endpoint + CSV Logging ——
// —— Live JSON Endpoint + CSV Logging ——
void handleLive() {
  // Read sensor data - use BSEC2 if available, otherwise fall back to regular BME680
  float Tc = 0.0f, T = 0.0f, H = 0.0f, P = 0.0f, gasKOhm = 0.0f;

  // BSEC2 disabled, use regular BME680
  if (appCfg.debugVerbose) Serial.println("🔍 Using regular BME680 library...");
  bme.performReading();
  Tc = bme.temperature;
  H = bme.humidity;
  P = bme.pressure / 100.0f;
  gasKOhm = bme.gas_resistance / 1000.0f;
  iaq = 0.0f;
  eco2 = 0.0f;
  bvoc = 0.0f;

  // Convert temperature to Fahrenheit
  T = Tc * 9.0f / 5.0f + 32.0f;

  float L = readLux();
  float uvMv = readUvMilliVolts();
  float uvIdx = computeUvIndexFromMv(uvMv);
  analogSetPinAttenuation(ADC_BATTERY_PIN, ADC_11db);
  long acc = 0;
  const int N = 8;
  for (int i = 0; i < N; i++) {
    acc += analogRead(ADC_BATTERY_PIN);
    delay(2);
  }
  int raw = acc / N;
  float v = (raw * 3.3f / 4095.0f) * VOLT_DIVIDER * batCal;

  bool sdOk = SD.exists("/logs.csv");
  bool rtcOk = rtcOkFlag;
  uint32_t bc = bootCount;
  // Rain in last hour (mm/h) using tipping bucket math from model
  const float MM_PER_TIP = 0.2794f;  // calibrate per your bucket (mm per tip)
  uint32_t nowMs = millis();
  uint32_t tipsLastHour = 0;
  // Copy volatile state under critical section to avoid races
  uint8_t sizeCopy, headCopy;
  uint32_t timesCopy[128];
  portENTER_CRITICAL(&rainMux);
  sizeCopy = rainTipSize;
  headCopy = rainTipHead;
  for (uint8_t i = 0; i < sizeCopy; i++) {
    int idx = (headCopy + 128 - 1 - i) % 128;
    timesCopy[i] = rainTipTimesMs[idx];
  }
  portEXIT_CRITICAL(&rainMux);
  for (uint8_t i = 0; i < sizeCopy; i++) {
    if (nowMs - timesCopy[i] <= 3600000UL) tipsLastHour++;
    else break;
  }
  float rainRateMmH = tipsLastHour * MM_PER_TIP;
  // Wind speed from Hall pulses
#if ENABLE_WIND
  float windHz = 0.0f;
  bool windOk = false;
  {
    uint8_t sizeCopy, headCopy;
    uint32_t timesCopy[128];
    uint32_t lastPulse;
    portENTER_CRITICAL(&windMux);
    sizeCopy = windPulseSize;
    headCopy = windPulseHead;
    lastPulse = lastWindPulseMs;
    for (uint8_t i = 0; i < sizeCopy; i++) {
      int idx = (headCopy + 128 - 1 - i) % 128;
      timesCopy[i] = windPulseTimesMs[idx];
    }
    portEXIT_CRITICAL(&windMux);
    unsigned long nowMs2 = millis();
    const uint32_t WINDOW_MS = 1500UL;
    uint32_t cutoff = nowMs2 > WINDOW_MS ? (nowMs2 - WINDOW_MS) : 0;
    uint32_t counted = 0;
    uint32_t oldest = nowMs2;
    for (uint8_t i = 0; i < sizeCopy; i++) {
      if (timesCopy[i] >= cutoff) {
        counted++;
        if (timesCopy[i] < oldest) oldest = timesCopy[i];
      } else break;
    }
    if (counted >= 2) {
      float span = (float)(nowMs2 - oldest) / 1000.0f;
      if (span < 0.05f) span = 0.05f;
      windHz = ((float)counted) / span;
    } else if (counted == 1) {
      uint32_t dt = nowMs2 - timesCopy[0];
      if (dt > 0 && dt <= WINDOW_MS) windHz = 1000.0f / (float)dt;
    }
    windOk = (nowMs2 - lastPulse) < 5000UL;
  }
  float windMps = windHz * WIND_MPS_PER_HZ;
  float windKmh = windHz * WIND_KMH_PER_HZ;
  float windMph = windHz * WIND_MPH_PER_HZ;
#else
  float windHz = 0.0f, windMps = 0.0f, windKmh = 0.0f, windMph = 0.0f; bool windOk = false;
#endif
  // WiFi diagnostics
  long rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  String ssid = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : String("");
  // Wake cause diagnostics
  esp_sleep_wakeup_cause_t wc = esp_sleep_get_wakeup_cause();
  const char* wcText = "ColdStart/Config";
  switch (wc) {
    case ESP_SLEEP_WAKEUP_EXT0: wcText = "RTC ALARM"; break;
    case ESP_SLEEP_WAKEUP_TIMER: wcText = "TIMER"; break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD: wcText = "TOUCH"; break;
    case ESP_SLEEP_WAKEUP_ULP: wcText = "ULP"; break;
    case ESP_SLEEP_WAKEUP_UNDEFINED: wcText = "ColdStart/Config"; break;
    default: break;
  }
  // Last scheduled alarm time (from RTC_DATA_ATTR)
  char alarmBuf[24] = "N/A";
  if (lastAlarmUnix != 0) {
    time_t la = (time_t)lastAlarmUnix;
    struct tm tma;
    localtime_r(&la, &tma);
    if (appCfg.time12h) {
      strftime(alarmBuf, sizeof(alarmBuf), "%Y-%m-%d %I:%M:%S %p", &tma);
    } else {
      strftime(alarmBuf, sizeof(alarmBuf), "%Y-%m-%d %H:%M:%S", &tma);
    }
  }
  // SD free space (KB) — estimate if card exposes total bytes via SD.cardSize()
  uint32_t sdFreeKB = 0;
  uint64_t cardBytes = SD.cardSize();
  if (cardBytes > 0) {
    uint64_t used = computeSdUsedBytes();
    if (used <= cardBytes) {
      sdFreeKB = static_cast<uint32_t>((cardBytes - used) / 1024ULL);
    }
  }

  // Timestamp (respects 12/24-hour preference)
  struct tm ti;
  char timestr[32];
  if (getLocalTime(&ti)) {
    if (appCfg.time12h) {
      strftime(timestr, sizeof(timestr), "%Y-%m-%d %I:%M:%S %p", &ti);
    } else {
      strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", &ti);
    }
  } else {
    strcpy(timestr, "N/A");
  }

  // Uptime & memory
  unsigned long up = millis() / 1000;
  size_t heap = ESP.getFreeHeap();
  // Replace PSRAM with Flash free space (KB)
  uint64_t flashTotal = ESP.getFlashChipSize();
  uint32_t flashFreeKB = 0;
  if (flashTotal > 0) {
    // Heuristic: use sketch space as proxy for free (depends on partition)
    size_t sketchSize = ESP.getSketchSize();
    size_t sketchSpace = ESP.getFreeSketchSpace();
    flashFreeKB = (uint32_t)((sketchSpace > sketchSize ? sketchSpace - sketchSize : sketchSpace) / 1024UL);
  }

  // Derived metrics
  float dewC = computeDewPointC(Tc, H);
  float dewF = dewC * 9.0f / 5.0f + 32.0f;
  float hiF = computeHeatIndexF(T, H);
  float mslp_hPa = computeMSLP_hPa(P, Tc, appCfg.altitudeM);
  float mslp_inHg = mslp_hPa * 0.0295299830714f;
  float wbtC = computeWetBulbC(Tc, H);
  float wbtF = wbtC * 9.0f / 5.0f + 32.0f;

  // Pressure history/trend
  time_t nowUnix = time(nullptr);
  updatePressureHistory(P, (uint32_t)nowUnix);
  float d3 = 0, d6 = 0, d12 = 0;
  bool h3 = false, h6 = false, h12 = false;
  h3 = getPressureDelta(3.0f, P, &d3);
  h6 = getPressureDelta(6.0f, P, &d6);
  h12 = getPressureDelta(12.0f, P, &d12);
  const char* trend = classifyTrendFromDelta(h3 ? d3 : (h6 ? d6 : (h12 ? d12 : 0)));
  const char* forecast = zambrettiSimple(mslp_hPa, trend);
  const char* generalForecast = generalForecastFromSensors(T, H, mslp_hPa, trend, rainRateMmH, L, uvIdx);
  // Compose detailed forecast using PM2.5, UV, and wind
#if ENABLE_WIND
  float windForDetailMph = windMph;
#else
  float windForDetailMph = 0.0f;
#endif
#if ENABLE_SDS011
  float pm25ForDetail = 0.0f;
  {
    bool sds_warm = millis() >= sdsWarmupUntilMs;
    if (sdsPresent) {
      if (sdsAccumCount > 0) pm25ForDetail = (float)sdsAccumPm25RawSum / (10.0f * (float)sdsAccumCount);
      else if (sdsAwake && sds_warm) pm25ForDetail = sdsPm25;
      else if (!sdsAwake && sdsLastPmUnix != 0 && (millis() / 1000UL) < 180UL) pm25ForDetail = sdsLastPm25_ugm3;
    }
  }
#else
  float pm25ForDetail = 0.0f;
#endif
  String forecastDetail = buildForecastDetail(T, H, mslp_hPa, trend, pm25ForDetail, uvIdx, windForDetailMph);

  // Build JSON (temp unit per user setting)
  StaticJsonDocument<896> doc;
  doc["temp_f"] = T;
  doc["temp_c"] = Tc;
  doc["temp_unit"] = appCfg.tempF ? "F" : "C";
  doc["temp"] = appCfg.tempF ? T : Tc;
  doc["hum"] = H;
  doc["pressure"] = P;
  doc["lux"] = L;
  doc["uv_mv"] = uvMv;
  doc["uv_index"] = uvIdx;
  doc["batt"] = v;
  doc["voc_kohm"] = gasKOhm;
  // BSEC2 metrics removed from /live
// SCD41 metrics
#if ENABLE_SCD41 && defined(HAVE_SCD4X_LIB)
  doc["scd41_ok"] = scd41Ok;
  doc["co2_ppm"] = (int)scd41Co2Ppm;
  doc["scd41_tc"] = scd41TempC;
  doc["scd41_rh"] = scd41Rh;
#endif
  doc["uptime"] = up;
  doc["heap"] = heap;
  doc["flash_free_kb"] = flashFreeKB;
  doc["time"] = timestr;  // ← our new timestamp
  doc["rain_mmph"] = rainRateMmH;
  doc["rain_inph"] = rainRateMmH * 0.0393700787f;
  doc["rain_unit"] = appCfg.rainUnitInches ? "in/h" : "mm/h";
#if ENABLE_WIND
  doc["wind_hz"] = windHz;
  doc["wind_mps"] = windMps;
  doc["wind_kmh"] = windKmh;
  doc["wind_mph"] = windMph;
  doc["wind_ok"] = windOk;
  // 1-hour rolling average (mph)
  float windAvg1h = 0.0f;
  if (windAvg1hCount > 0) windAvg1h = windAvg1hSum / (float)windAvg1hCount;
  doc["wind_avg_mph_1h"] = windAvg1h;
#else
  doc["wind_hz"] = 0.0f;
  doc["wind_mps"] = 0.0f;
  doc["wind_kmh"] = 0.0f;
  doc["wind_mph"] = 0.0f;
  doc["wind_ok"] = false;
#endif
#if ENABLE_SDS011
  // SDS011 fields
  bool sds_warm = millis() >= sdsWarmupUntilMs;
  float pm25Live = 0.0f, pm10Live = 0.0f;
  if (sdsPresent) {
    if (sdsAccumCount > 0) {
      pm25Live = (float)sdsAccumPm25RawSum / (10.0f * (float)sdsAccumCount);
      pm10Live = (float)sdsAccumPm10RawSum / (10.0f * (float)sdsAccumCount);
    } else if (sdsAwake && sds_warm) {
      pm25Live = sdsPm25;
      pm10Live = sdsPm10;
    } else if (!sdsAwake && sdsLastPmUnix != 0 && (millis() / 1000UL) < 180UL) {
      // During the brief serve window right after wake, fall back to last recorded values for UI continuity
      pm25Live = sdsLastPm25_ugm3;
      pm10Live = sdsLastPm10_ugm3;
    }
  }
  doc["pm25_ugm3"] = pm25Live;
  doc["pm10_ugm3"] = pm10Live;
  doc["sds_ok"] = sdsPresent;
  doc["sds_awake"] = (bool)sdsAwake;
  doc["sds_warm"] = (bool)(sdsAwake && sds_warm);
  doc["sds_auto_sleep_ms_left"] = (sdsAutoSleepAtMs && sdsAwake) ? (long)max(0L, (long)(sdsAutoSleepAtMs - millis())) : 0;
#endif
  // Boot started: compute from current local time minus uptime; format as time only
  {
    struct tm tnow;
    if (getLocalTime(&tnow)) {
      time_t nowsec = mktime(&tnow);
      time_t bootUnix = nowsec - (time_t)up;
      struct tm tb;
      localtime_r(&bootUnix, &tb);
      char bootStr[32];
      if (appCfg.time12h) {
        strftime(bootStr, sizeof(bootStr), "%I:%M:%S %p", &tb);
      } else {
        strftime(bootStr, sizeof(bootStr), "%H:%M:%S", &tb);
      }
      doc["boot_started"] = bootStr;
    } else {
      doc["boot_started"] = "N/A";
    }
  }
  doc["sd_ok"] = sdOk;
  doc["rtc_ok"] = rtcOk;
  doc["boot_count"] = bc;
  doc["rssi"] = rssi;
  doc["ssid"] = ssid;
  doc["wakeup_cause"] = (int)wc;
  doc["wakeup_cause_text"] = wcText;
  doc["last_alarm"] = alarmBuf;
  doc["sd_free_kb"] = sdFreeKB;
  doc["dew_f"] = dewF;
  doc["dew_c"] = dewC;
  doc["hi_f"] = hiF;
  doc["hi_c"] = (hiF - 32.0f) * 5.0f / 9.0f;
  doc["wbt_f"] = wbtF;
  doc["wbt_c"] = wbtC;
  doc["mslp_hPa"] = mslp_hPa;
  doc["mslp_inHg"] = mslp_inHg;
  doc["pressure_trend"] = trend;
  doc["forecast"] = forecast;
  doc["general_forecast"] = generalForecast;
  doc["forecast_detail"] = forecastDetail;
  doc["aqi_category"] = aqiCategoryFromPm25(pm25ForDetail);
  // pressure unit removed; UI shows both
  // Last SD log time (persisted across deep sleep); format now per preference
  if (lastSdLogUnix != 0) {
    time_t lsu = (time_t)lastSdLogUnix;
    struct tm tsl;
    localtime_r(&lsu, &tsl);
    char buf[32];
    if (appCfg.time12h) {
      strftime(buf, sizeof(buf), "%Y-%m-%d %I:%M:%S %p", &tsl);
    } else {
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tsl);
    }
    doc["last_sd_log"] = buf;
  } else {
    doc["last_sd_log"] = lastSdLogTime;
  }

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);

  // --- only log once per wake cycle ---
  if (!loggedThisWake) {
#if ENABLE_SDS011
    // If SDS011 is present, delay the single per-wake log until warm-up (30s)
    if (sdsPresent && millis() < sdsWarmupUntilMs) {
      return;  // wait until next /live poll after warm-up to write CSV
    }
#endif
    File f = SD.open("/logs.csv", FILE_APPEND);
    if (f) {
      // Write unified 14-column schema: timestamp,temp_f,humidity,dew_f,hi_f,pressure,pressure_trend,forecast,lux,voltage,voc_kohm,mslp_inHg,rain,boot_count
      float dewF_once = (computeDewPointC(Tc, H) * 9.0f / 5.0f) + 32.0f;
      float hiF_once = computeHeatIndexF(T, H);
      float mslp_hPa_once = computeMSLP_hPa(P, Tc, appCfg.altitudeM);
      float mslp_inHg_once = mslp_hPa_once * 0.0295299830714f;
      // Compute rain rate like performLogging
      const float MM_PER_TIP = 0.2794f;
      uint32_t nowMs2 = millis();
      uint32_t tipsLastHour2 = 0;
      uint8_t sizeCopy2, headCopy2;
      uint32_t timesCopy2[128];
      portENTER_CRITICAL(&rainMux);
      sizeCopy2 = rainTipSize;
      headCopy2 = rainTipHead;
      for (uint8_t i = 0; i < sizeCopy2; i++) {
        int idx = (headCopy2 + 128 - 1 - i) % 128;
        timesCopy2[i] = rainTipTimesMs[idx];
      }
      portEXIT_CRITICAL(&rainMux);
      for (uint8_t i = 0; i < sizeCopy2; i++) {
        if (nowMs2 - timesCopy2[i] <= 3600000UL) tipsLastHour2++;
        else break;
      }
      float rainRateMmH_once = tipsLastHour2 * MM_PER_TIP;
      float rainOut_once = appCfg.rainUnitInches ? (rainRateMmH_once * 0.0393700787f) : rainRateMmH_once;
      const char* gforecast_once = generalForecastFromSensors(T, H, mslp_hPa_once, trend, rainRateMmH_once, L, uvIdx);
      // gasKOhm is already read above in the sensor reading section
      float pm25Out = 0.0f, pm10Out = 0.0f;
      if (sdsPresent && millis() >= sdsWarmupUntilMs) {
        if (sdsAccumCount > 0) {
          pm25Out = (float)sdsAccumPm25RawSum / (10.0f * (float)sdsAccumCount);
          pm10Out = (float)sdsAccumPm10RawSum / (10.0f * (float)sdsAccumCount);
        } else {
          pm25Out = sdsPm25;
          pm10Out = sdsPm10;
        }
      }
      // Do not log BSEC2 values (IAQ/eCO2/bVOC) in per-/live log either
      f.printf("%s,%.1f,%.1f,%.1f,%.1f,%.2f,%s,%s,%.1f,%.0f,%.1f,%.2f,%.1f,%.2f,%.2f,%lu,%s,%s,%s,%.1f,%.1f,%u,%.2f\n",
               timestr, T, H, dewF_once, hiF_once, P, trend, gforecast_once, L, uvMv, uvIdx, v, gasKOhm, mslp_inHg_once, rainOut_once, (unsigned long)bootCount,
               "", "", "",
               pm25Out, pm10Out, (unsigned)scd41Co2Ppm, windMph);
      f.close();
      // Persist unix timestamp as well for formatting per user preference later
      time_t nowUnixTs = time(nullptr);
      lastSdLogUnix = (uint32_t)nowUnixTs;
      strncpy(lastSdLogTime, timestr, sizeof(lastSdLogTime));
      lastSdLogTime[sizeof(lastSdLogTime) - 1] = '\0';
    }
    loggedThisWake = true;  // prevent further logs until next wake
  }
}


// —— Fallback AP Mode for configuration ——
void startAPMode() {
  Serial.println("⚙️ Starting AP EnvLogger_Config");
  WiFi.mode(WIFI_AP);
  WiFi.softAP("WeatherStation1", "12345678");  //--------------------------------------------Change this per device
  Serial.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());

  // indicate AP mode with 2 slow blinks
  blinkStatus(2, 300);
}




// —— OTA Handlers ——
void setupOTA() {
  ElegantOTA.begin(&server);  // ElegantOTA at /update by default
  // Enable basic auth for OTA
  ElegantOTA.setAuth("weatherstation1", "12345678");
  Serial.println("✅ ElegantOTA ready at /update (auth enabled)");
}

// —— Download stored logs ——
void handleDownload() {
  if (!SD.exists("/logs.csv")) {
    server.send(404, "text/plain", "No logs found.");
    return;
  }
  File f = SD.open("/logs.csv", "r");

  server.sendHeader("Content-Disposition",
                    "attachment; filename=logs.csv");
  server.streamFile(f, "text/csv");
  f.close();
}

// —— Render logs in a styled HTML table ——
void handleViewLogs() {
  if (!SD.exists("/logs.csv")) {
    server.send(404, "text/plain", "No logs found.");
    return;
  }

  // Read file into memory and keep only last N lines
  const size_t MAX_LINES = 200;  // show last 200 rows
  File f = SD.open("/logs.csv", "r");
  if (!f) {
    server.send(500, "text/plain", "Failed to open logs.csv");
    return;
  }

  // Collect lines
  std::vector<String> lines;
  lines.reserve(MAX_LINES + 4);
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    lines.push_back(line);
    if (lines.size() > MAX_LINES + 1) {  // +1 to retain header
      lines.erase(lines.begin() + 1);    // drop oldest data line but keep header at index 0
    }
  }
  f.close();

  if (lines.empty()) {
    server.send(200, "text/html", "<p>No data.</p>");
    return;
  }

  // Build HTML
  String html;
  html += "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'/>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'/>";
  html += "<meta http-equiv='Cache-Control' content='no-store'/>";
  html += "<meta http-equiv='Pragma' content='no-cache'/>";
  html += "<meta http-equiv='Expires' content='0'/>";
  html += "<title>Logs</title>";
  html += "<style>body{background:#121212;color:#eee;font-family:Arial,sans-serif;margin:0;padding:16px;}";
  html += ".wrap{max-width:1000px;margin:0 auto;}h2{margin:8px 0 16px;}table{width:90%;margin:0 auto;border-collapse:collapse;background:#1e1e1e;border-radius:6px;overflow:hidden;}";
  html += "th,td{padding:10px;border-bottom:1px solid #2a2a2a;text-align:left;}th{background:#222;}tr:nth-child(even){background:#181818;}";
  html += "a.btn,button.btn{display:inline-block;background:#444;padding:10px 12px;border-radius:6px;color:#eee;text-decoration:none;margin:12px 6px 0 0;}";
  html += ".filters{margin:12px 0;display:flex;gap:12px;flex-wrap:wrap;align-items:flex-end;} .filters .group{display:flex;gap:8px;align-items:center;} .filters input,.filters select{background:#2a2a2a;color:#eee;border:none;border-radius:6px;padding:8px;} .filters button{background:#3d85c6;color:#fff;border:none;border-radius:6px;padding:8px 12px;cursor:pointer;} .filters button:hover{transform:scale(1.03);}";
  html += "</style></head><body><div class='wrap'>";
  html += "<h2>Logs</h2>";
  html += "<a class='btn' href='/'>&larr; Back</a>";
  html += "<a class='btn' href='/download'>Download CSV</a>";
  html += "<form action='/reset' method='POST' onsubmit=\"return confirm('Clear logs?')\" style='display:inline-block;margin:12px 6px 0 0;'><button class='btn' type='submit'>Clear Logs</button></form>";
  html += "<h3 style='margin:12px 0 4px;'>Filters</h3>";
  // Filter controls
  html += "<div class='filters'>";
  html += "<div class='group'><label>Field</label><select id='fField'>";
  // Use actual numeric column indexes from the table (0=Time)
  html += "<option value='1'>Temp (F)</option>";        // numeric
  html += "<option value='2'>Hum (%)</option>";         // numeric
  html += "<option value='3'>Dew Pt (F)</option>";      // numeric
  html += "<option value='4'>Heat Index (F)</option>";  // numeric
  html += "<option value='5'>Pressure (hPa)</option>";  // numeric
  html += "<option value='8'>Lux</option>";             // numeric
  html += "<option value='9'>UV (mV)</option>";         // numeric
  html += "<option value='10'>UV Index</option>";       // numeric
  html += "<option value='11'>Battery (V)</option>";    // numeric
  html += "<option value='12'>VOC (kΩ)</option>";       // numeric
  html += "<option value='13'>MSLP (inHg)</option>";    // numeric
  html += "<option value='14'>Rain</option>";           // numeric, unit shown in header
  html += "<option value='15'>Boot Count</option>";     // numeric
  // BSEC2 metrics removed from filters
  html += "<option value='19'>PM2.5 (µg/m³)</option>";  // numeric
  html += "<option value='20'>PM10 (µg/m³)</option>";   // numeric
  html += "<option value='21'>CO2 (ppm)</option>";      // numeric
  html += "<option value='22'>Wind (mph)</option>";     // numeric
  html += "</select></div>";
  html += "<div class='group'><label>Type</label><select id='fType'>";
  html += "<option value='between'>Between</option>";
  html += "<option value='gte'>&ge; Min</option>";
  html += "<option value='lte'>&le; Max</option>";
  html += "</select></div>";
  html += "<div class='group'><label title='Click to set from column minimum' style='cursor:pointer' onclick=\"setMinFromColumn()\">Min</label><input id='fMin' type='number' step='any' placeholder='min'>";
  html += "<label title='Click to set from column maximum' style='cursor:pointer' onclick=\"setMaxFromColumn()\">Max</label><input id='fMax' type='number' step='any' placeholder='max'>";
  html += "<button onclick=\"autoRange()\" title='Set Min/Max from column'>Auto Range</button></div>";
  html += "<div class='group'><button onclick=\"applyFilter()\">Apply Filter</button><button onclick=\"clearFilter()\">Clear</button></div>";
  html += "</div>";

  // Friendly headers
  html += "<table><thead><tr>";
  String rainHdr = appCfg.rainUnitInches ? "Rain (in/h)" : "Rain (mm/h)";
  html += "<th>Time</th><th>Temp (F)</th><th>Hum (%)</th><th>Dew Pt (F)</th><th>Heat Index (F)</th><th>Pressure (hPa)</th><th>Trend</th><th>Forecast</th><th>Lux</th><th>UV (mV)</th><th>UV Index</th><th>Battery (V)</th><th>VOC (kΩ)</th><th>MSLP (inHg)</th><th>" + rainHdr + "</th><th>Boot Count</th><th>PM2.5 (µg/m³)</th><th>PM10 (µg/m³)</th><th>CO₂ (ppm)</th><th>Wind (mph)</th>";
  html += "</tr></thead><tbody>";

  // Skip the CSV header if present
  size_t startIdx = 0;
  if (lines[0].startsWith("timestamp,")) startIdx = 1;

  for (size_t i = startIdx; i < lines.size(); ++i) {
    String ln = lines[i];
    // Split by comma into up to 28 fields (supports extended schema)
    String cols[28];
    int col = 0;
    int from = 0;
    int idx;
    while (col < 24 && (idx = ln.indexOf(',', from)) >= 0) {
      cols[col++] = ln.substring(from, idx);
      from = idx + 1;
    }
    // Grab remainder
    cols[col++] = ln.substring(from);

    // If old schema (6 cols): timestamp,temp,hum,pressure,lux,voltage
    // Map to new table with blanks for dew/hi/trend
    bool isOld = (col <= 6);

    html += "<tr>";
    html += "<td>" + cols[0] + "</td>";  // already formatted timestamp
    html += "<td>" + cols[1] + "</td>";  // temp
    html += "<td>" + cols[2] + "</td>";  // hum
    if (isOld) {
      html += "<td></td><td></td>";        // dew, hi empty
      html += "<td>" + cols[3] + "</td>";  // pressure
      html += "<td></td>";                 // trend empty
      html += "<td></td>";                 // forecast empty (keeps columns aligned)
      html += "<td>" + cols[4] + "</td>";  // lux
      html += "<td>" + cols[5] + "</td>";  // voltage
      html += "<td></td>";                 // voc empty
      html += "<td></td>";                 // mslp empty
      html += "<td></td>";                 // rain empty
      html += "<td></td>";                 // boot_count empty
    } else {
      html += "<td>" + cols[3] + "</td>";                             // dew_f
      html += "<td>" + cols[4] + "</td>";                             // hi_f
      html += "<td>" + cols[5] + "</td>";                             // pressure
      html += "<td>" + cols[6] + "</td>";                             // trend
      html += "<td>" + cols[7] + "</td>";                             // forecast (after trend)
      html += "<td>" + cols[8] + "</td>";                             // lux
      html += "<td>" + cols[9] + "</td>";                             // uv_mv
      html += "<td>" + cols[10] + "</td>";                            // uv_index
      html += "<td>" + (col > 11 ? cols[11] : String("")) + "</td>";  // voltage
      html += "<td>" + (col > 12 ? cols[12] : String("")) + "</td>";  // voc_kohm
      html += "<td>" + (col > 13 ? cols[13] : String("")) + "</td>";  // mslp_inHg
      html += "<td>" + (col > 14 ? cols[14] : String("")) + "</td>";  // rain (unit per setting at write time)
      html += "<td>" + (col > 15 ? cols[15] : String("")) + "</td>";  // boot_count
      // BSEC2 columns removed from view
      html += "<td>" + (col > 19 ? cols[19] : String("")) + "</td>";  // PM2.5
      html += "<td>" + (col > 20 ? cols[20] : String("")) + "</td>";  // PM10
      html += "<td>" + (col > 21 ? cols[21] : String("")) + "</td>";  // CO2 ppm
      html += "<td>" + (col > 22 ? cols[22] : String("")) + "</td>";  // Wind mph
    }
    html += "</tr>";
  }

  html += "</tbody></table>";
  // Filtering script
  html += "<script>function applyFilter(){var f=parseInt(document.getElementById('fField').value,10);var type=document.getElementById('fType').value;var minStr=document.getElementById('fMin').value;var maxStr=document.getElementById('fMax').value;var hasMin=minStr!=='';var hasMax=maxStr!=='';var min=parseFloat(minStr);var max=parseFloat(maxStr);var tb=document.querySelector('tbody');if(!tb)return;for(var i=0;i<tb.rows.length;i++){var r=tb.rows[i];var t=r.cells[f]?parseFloat(r.cells[f].textContent):NaN;var show=true;if(isNaN(t)){show=false;}else{if(type==='between'){if(hasMin&&t<min)show=false;if(hasMax&&t>max)show=false;}else if(type==='gte'){if(!hasMin||t<min)show=false;}else if(type==='lte'){if(!hasMax||t>max)show=false;}}r.style.display=show?'':'none';}}</script>";
  html += "<script>function clearFilter(){var tb=document.querySelector('tbody');if(tb){for(var i=0;i<tb.rows.length;i++){tb.rows[i].style.display='';}}document.getElementById('fType').value='between';document.getElementById('fMin').value='';document.getElementById('fMax').value='';}</script>";
  // Quick-select helpers for min/max
  html += "<script>function computeColMinMax(f){var tb=document.querySelector('tbody');if(!tb)return null;var mn=Infinity,mx=-Infinity,has=false;for(var i=0;i<tb.rows.length;i++){var c=tb.rows[i].cells[f];if(!c)continue;var v=parseFloat(c.textContent);if(isNaN(v))continue;has=true;if(v<mn)mn=v;if(v>mx)mx=v;}if(!has)return null;return {min:mn,max:mx};}</script>";
  html += "<script>function setMinFromColumn(){var f=parseInt(document.getElementById('fField').value,10);var mm=computeColMinMax(f);if(!mm)return;document.getElementById('fMin').value=mm.min;}</script>";
  html += "<script>function setMaxFromColumn(){var f=parseInt(document.getElementById('fField').value,10);var mm=computeColMinMax(f);if(!mm)return;document.getElementById('fMax').value=mm.max;}</script>";
  html += "<script>function autoRange(){var f=parseInt(document.getElementById('fField').value,10);var mm=computeColMinMax(f);if(!mm)return;document.getElementById('fMin').value=mm.min;document.getElementById('fMax').value=mm.max;/* ensure type is between for auto */document.getElementById('fType').value='between';applyFilter();}</script>";
  // Initialize from URL query parameters and auto-apply if present
  html += "<script>(function(){try{var p=new URLSearchParams(location.search);if(!p)return;var fieldMap={temp:1,temperature:1,hum:2,humidity:2,dew:3,dew_f:3,hi:4,heat:4,heat_index:4,pressure:5,lux:8,light:8,illuminance:8,bat:9,batv:9,battery:9,voc:10,gas:10,mslp:11,slp:11,sea_level:11,rain:12,boot:13,boots:13,boot_count:13,wind:22,wind_mph:22};var fld=p.get('field');if(fld){var v=fieldMap[fld.toLowerCase()];if(!v&&/^[0-9]+$/.test(fld))v=parseInt(fld,10);if(v)document.getElementById('fField').value=String(v);}var type=p.get('type');if(type){var t=type.toLowerCase();if(t==='gte'||t==='lte'||t==='between'){document.getElementById('fType').value=t;}}if(p.has('min')){document.getElementById('fMin').value=p.get('min');}if(p.has('max')){document.getElementById('fMax').value=p.get('max');}if(p.has('field')||p.has('min')||p.has('max')||p.has('type')){applyFilter();}}catch(e){}})();</script>";
  html += "</div></body></html>";
  server.sendHeader("Cache-Control", "no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
  server.send(200, "text/html", html);
}
// —— Add a new Wi-Fi network ——
void handleAdd() {
  if (server.hasArg("ssid") && server.hasArg("pass")) {
    JsonArray nets = wifiConfig["networks"].as<JsonArray>();
    JsonObject net = nets.createNestedObject();
    net["ssid"] = server.arg("ssid");
    net["pass"] = server.arg("pass");
    String out;
    serializeJson(wifiConfig, out);
    preferences.putString("config", out);
  }
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

// —— Delete a saved network by SSID ——
void handleDel() {
  if (server.hasArg("ssid")) {
    String ssid = server.arg("ssid");
    JsonArray nets = wifiConfig["networks"].as<JsonArray>();
    for (int i = 0; i < (int)nets.size(); i++) {
      if (ssid == nets[i]["ssid"].as<const char*>()) {
        nets.remove(i);
        break;
      }
    }
    String out;
    serializeJson(wifiConfig, out);
    preferences.putString("config", out);
  }
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

// —— Clear all logs ——
void handleReset() {
  if (SD.exists("/logs.csv")) {
    SD.remove("/logs.csv");
  }

  File f = SD.open("/logs.csv", FILE_WRITE);
  if (f) {
    // Extended schema without BSEC2: uv_mv, uv_index, SDS011 PM, SCD41 CO2, and Wind (mph) at the end
    f.println("timestamp,temp_f,humidity,dew_f,hi_f,pressure,pressure_trend,forecast,lux,uv_mv,uv_index,voltage,voc_kohm,mslp_inHg,rain,boot_count,pm25_ugm3,pm10_ugm3,co2_ppm,wind_mph");
    f.close();
  } else {
    Serial.println("❌ Failed to recreate logs.csv");
  }

  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}  // ← Make sure this closing brace ends handleReset

void handleSleep() {
  String html;
  html += "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'/>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'/>";
  html += "<title>Sleeping…</title>";
  html += "<style>body{background:#121212;color:#eee;font-family:Arial,sans-serif;margin:0;padding:16px;}";
  html += ".wrap{max-width:700px;margin:40px auto;text-align:center;}h2{margin:8px 0 12px;}p{opacity:.8;}";
  html += ".spinner{margin:22px auto;width:36px;height:36px;border:4px solid #333;border-top-color:#3d85c6;border-radius:50%;animation:spin 1s linear infinite;}@keyframes spin{to{transform:rotate(360deg);}}";
  html += "</style></head><body><div class='wrap'>";
  html += "<h2>The device will go to sleep to save power</h2>";
  html += "<p>10 minutes starting now…</p>";
  html += "<div class='spinner'></div>";
  html += "</div></body></html>";
  server.send(200, "text/html", html);
  delay(1000);
  blinkStatus(3, 200);

  // Ensure optional sensors are stopped before deep sleep
#if ENABLE_SDS011
  if (sdsPresent && sdsAwake) {
#if defined(HAVE_SDS_LIB)
    sds.sleep();
#endif
    sdsAwake = false;
  }
#endif
#if ENABLE_SCD41 && defined(HAVE_SCD4X_LIB)
  if (scd41Ok) {
    scd4x.stopPeriodicMeasurement();
  }
#endif

  if (rtcOkFlag) {
    rtc.clearAlarm(1);
    DateTime t = rtc.now();
    DateTime alarmT = t + TimeSpan(0, 0, DEEP_SLEEP_SECONDS / 60, 0);
    rtc.setAlarm1(alarmT, DS3231_A1_Minute);
    rtc.writeSqwPinMode(DS3231_OFF);
    rtc.clearAlarm(1);
    lastAlarmUnix = alarmT.unixtime();
    esp_sleep_enable_ext0_wakeup(RTC_INT_PIN, LOW);
    Serial.printf("Manual sleep → wake at %02d:%02d\n", alarmT.hour(), alarmT.minute());
  } else {
    Serial.println("Manual sleep without RTC → timer wake only");
    lastAlarmUnix = 0;
  }

  // Always enable timer wake as safety
  esp_sleep_enable_timer_wakeup((DEEP_SLEEP_SECONDS + 60) * 1000000ULL);

  // Ensure LED is off before entering deep sleep from manual handler as well
  digitalWrite(STATUS_LED_PIN, LOW);
  esp_deep_sleep_start();
}


void loop() {
  unsigned long now = millis();
  // ─── Service background handlers ───
  // ArduinoOTA.handle(); // not used when using HTTPUpdateServer
  server.handleClient();
  updateStatusLed();
#if ENABLE_SCD41 && defined(HAVE_SCD4X_LIB)
  // Poll SCD41 for a new sample every ~1s (sensor outputs ~5s)
  if (scd41Ok) {
    unsigned long nowMs = millis();
    if (nowMs - scd41LastPollMs >= 1000UL) {
      scd41LastPollMs = nowMs;
      bool dataReady = false;
      int16_t drRet = scd4x.getDataReadyFlag(dataReady);
      if (drRet != 0) {
        Serial.printf("⚠️ SCD41 getDataReadyFlag() failed: %d\n", drRet);
      } else if (dataReady) {
        uint16_t co2;
        float tc, rh;
        int16_t rdRet = scd4x.readMeasurement(co2, tc, rh);
        if (rdRet == 0) {
          // Library returns degC and %RH
          if (co2 != 0xFFFF) {
            scd41Co2Ppm = co2;
            scd41TempC = tc;
            scd41Rh = rh;
          } else if (appCfg.debugVerbose) {
            Serial.println("[SCD41] Invalid measurement (CO2=0xFFFF)");
          }
        } else {
          Serial.printf("⚠️ SCD41 readMeasurement() failed: %d\n", rdRet);
        }
      }
    }
  }
#endif
#if ENABLE_WIND
  // ─── 1-hour rolling wind average sampler (once per minute) ───
  if (now - windAvg1hLastSampleMs >= 60000UL || windAvg1hLastSampleMs == 0) {
    // Compute instantaneous wind frequency using last ~1.5 s of pulses
    uint8_t sizeCopyW, headCopyW;
    uint32_t timesCopyW[128];
    portENTER_CRITICAL(&windMux);
    sizeCopyW = windPulseSize;
    headCopyW = windPulseHead;
    for (uint8_t i = 0; i < sizeCopyW; i++) {
      int idx = (headCopyW + 128 - 1 - i) % 128;
      timesCopyW[i] = windPulseTimesMs[idx];
    }
    portEXIT_CRITICAL(&windMux);
    unsigned long nowMs2 = millis();
    const uint32_t WINDOW_MS = 1500UL;
    uint32_t cutoff = nowMs2 > WINDOW_MS ? (nowMs2 - WINDOW_MS) : 0;
    uint32_t counted = 0;
    uint32_t oldest = nowMs2;
    for (uint8_t i = 0; i < sizeCopyW; i++) {
      if (timesCopyW[i] >= cutoff) {
        counted++;
        if (timesCopyW[i] < oldest) oldest = timesCopyW[i];
      } else break;
    }
    float windHz_inst = 0.0f;
    if (counted >= 2) {
      float span = (float)(nowMs2 - oldest) / 1000.0f;
      if (span < 0.05f) span = 0.05f;
      windHz_inst = ((float)counted) / span;
    } else if (counted == 1) {
      uint32_t dt = nowMs2 - timesCopyW[0];
      if (dt > 0 && dt <= WINDOW_MS) windHz_inst = 1000.0f / (float)dt;
    }
    float mph = windHz_inst * WIND_MPH_PER_HZ;
    // Update rolling 60-sample window
    if (windAvg1hCount < 60) {
      windAvg1hMph[windAvg1hIndex] = mph;
      windAvg1hSum += mph;
      windAvg1hCount++;
      windAvg1hIndex = (windAvg1hIndex + 1) % 60;
    } else {
      windAvg1hSum -= windAvg1hMph[windAvg1hIndex];
      windAvg1hMph[windAvg1hIndex] = mph;
      windAvg1hSum += mph;
      windAvg1hIndex = (windAvg1hIndex + 1) % 60;
    }
    windAvg1hLastSampleMs = now;
  }
#endif
#if ENABLE_SDS011
  // Read SDS011 frames when available; parse 10-byte packet 0xAA 0xC0 ... 0xAB
  while (sdsSerial.available()) {
    uint8_t b = (uint8_t)sdsSerial.read();
    if (sdsBufPos == 0 && b != 0xAA) continue;
    sdsBuf[sdsBufPos++] = b;
    if (sdsBufPos >= 10) {
      // Validate header/footer
      if (sdsBuf[0] == 0xAA && sdsBuf[1] == 0xC0 && sdsBuf[9] == 0xAB) {
        uint8_t sum = 0;
        for (int i = 2; i <= 7; i++) sum += sdsBuf[i];
        if (sum == sdsBuf[8]) {
          uint16_t pm25 = (uint16_t)sdsBuf[2] | ((uint16_t)sdsBuf[3] << 8);
          uint16_t pm10 = (uint16_t)sdsBuf[4] | ((uint16_t)sdsBuf[5] << 8);
          sdsPm25 = pm25 / 10.0f;
          sdsPm10 = pm10 / 10.0f;
          // Accumulate raw values to average later during wake window
          if (millis() >= sdsWarmupUntilMs) {
            sdsAccumPm25RawSum += pm25;
            sdsAccumPm10RawSum += pm10;
            sdsAccumCount++;
          }
          sdsLastPacketMs = millis();
          if (appCfg.debugVerbose && millis() >= sdsWarmupUntilMs) {
            Serial.printf("[SDS011] PM25=%.1f PM10=%.1f\n", sdsPm25, sdsPm10);
          }
        }
      }
      sdsBufPos = 0;
    }
  }
  // Auto-sleep enforcement in 2‑minute serve window, using vendor frames
  if (sdsAwake && sdsAutoSleepAtMs != 0 && millis() >= sdsAutoSleepAtMs) {
    sdsEnsureSleep();
    // Finalize averaged PM values for this wake
    if (sdsAccumCount > 0) {
      sdsPm25 = (float)sdsAccumPm25RawSum / (10.0f * (float)sdsAccumCount);
      sdsPm10 = (float)sdsAccumPm10RawSum / (10.0f * (float)sdsAccumCount);
    }
    sdsAccumPm25RawSum = 0;
    sdsAccumPm10RawSum = 0;
    sdsAccumCount = 0;
    sdsAutoSleepAtMs = 0;
  }
#endif

  // Daily NTP resync and DS3231 drift check at ~02:00 local
  static time_t lastSync = 0;
  time_t tnow = time(nullptr);
  if (tnow != 0) {
    struct tm lt;
    localtime_r(&tnow, &lt);
    if (lt.tm_hour == 2 && (tnow - lastSync) > 20 * 3600) {  // at most once per day
      Serial.println("⏳ Daily time sync");
      configTzTime("EST5EDT,M3.2.0/2,M11.1.0/2", "pool.ntp.org");
      if (rtcOkFlag) {
        struct tm ti;
        if (getLocalTime(&ti)) {
          time_t sys = mktime(&ti);
          DateTime rtcTime = rtc.now();
          long delta = (long)sys - (long)rtcTime.unixtime();
          if (abs(delta) > 5) {
            Serial.printf("⏳ Adjusting RTC drift %lds\n", delta);
            rtc.adjust(DateTime(ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday, ti.tm_hour, ti.tm_min, ti.tm_sec));
          }
        }
      }
      lastSync = tnow;
    }
  }

  // ─── AP-stuck fallback reset (always checked first) ───
  static unsigned long apResetTimer = 0;
  if (WiFi.getMode() & WIFI_AP) {
    if (apResetTimer == 0) {
      apResetTimer = now;  // first AP loop
    } else if (now - apResetTimer >= 180000UL) {
      ESP.restart();  // “stuck” fallback reboot
    }
  } else {
    apResetTimer = 0;  // left AP, clear timer
  }

  // ─── Day/Night state machine (may call deep-sleep) ───
  updateDayNightState();

  // BSEC2 disabled: no state persistence

  // ─── During 30‑min config window, duty-cycle SDS011 and perform periodic logging ───
  if (serveWindow == UPTIME_STARTUP) {
    performLogging();
  }

  // ─── DAY MODE (stay awake + periodic logging) ───
  if (powerMode == MODE_DAY) {
    performLogging();  // will only actually log every LOG_INTERVAL_MS
    return;
  }

  // ─── NIGHT MODE (brief awake → deep-sleep) ───
  // LED handled by updateStatusLed(); here we only schedule sleep
  static bool sleepScheduled = false;
  // One-time scheduled logging mid-window (~35s) to ensure sensors are ready
  if (!nightLogDone && scheduledNightLogMs != 0 && millis() >= scheduledNightLogMs) {
    // Make sure SDS011 is awake if using pre-log presets
#if ENABLE_SDS011 && defined(HAVE_SDS_LIB)
    if (sdsPresent && !sdsAwake) {
      sds.wakeup();
      sdsAwake = true;
      if (millis() + 5000UL < scheduledNightLogMs + 10000UL) {
        sdsWarmupUntilMs = millis() + SDS_DUTY_WARMUP_MS;
      }
    }
#endif
    performLogging();
    nightLogDone = true;
  }
  if (!sleepScheduled) {
    bool windowDone = (now - startMillis) > serveWindow * 1000UL;
    bool graceDone = now > 5000UL;  // give at least 5 s for your server to spin up
    if (windowDone && graceDone) {
      if (serveWindow == UPTIME_STARTUP) {
        // End of 30 min config window → start a fixed 2-minute decision run
        Serial.println("[MODE] End of config window → starting 2-minute decision run");
        startMillis = now;            // reset timer
        serveWindow = UPTIME_CONFIG;  // 120 seconds fixed
        postConfigStarted = true;
      } else {
        // Window (2 min) done → enter deep sleep
        sleepScheduled = true;
        Serial.println("⏲ Serve window over → entering deep sleep");
        performLogging();                      // final log before sleeping
        prepareDeepSleep(DEEP_SLEEP_SECONDS);  // go to sleep; wake in 600 s
      }
    }
  }
}
