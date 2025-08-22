
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
// for logs page rendering
#include <vector>
//#include <Adafruit_SGP40.h>        // 🌬 VOC sensor


// —— Pin & Address Definitions ——
//#define LED_PIN            2
// Pin for DS3231 SQW/INT connected to ESP32 RTC GPIO
//#define FAN_PIN 12  // choose any free GPIO--------------------------------------un// when installed
#define RTC_INT_PIN GPIO_NUM_2
#define SD_CS 5         // change to your CS pin
#define SD_SCK_PIN 18   // VSPI default SCK
#define SD_MISO_PIN 19  // VSPI default MISO
#define SD_MOSI_PIN 23  // VSPI default MOSI
#define I2C_SDA 21
#define I2C_SCL 22
#define ADC_BATTERY_PIN 35
// Rain gauge tipping bucket (reed switch to GND)
#define RAIN_PIN 27
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

const uint16_t LUX_ENTER_DAY = 1600;  // enter continuous awake
const uint16_t LUX_EXIT_DAY = 1400;   // exit to deep-sleep cycling
const uint32_t DWELL_MS = 30000;      // 30 s dwell
const uint32_t SAMPLE_INTERVAL_MS = 2000;

const uint8_t LUX_BUF_SIZE = DWELL_MS / SAMPLE_INTERVAL_MS;  // 15 samples
uint16_t luxBuffer[LUX_BUF_SIZE];
uint8_t luxIndex = 0;
uint8_t luxCount = 0;
uint32_t luxSum = 0;

uint32_t lastSampleMillis = 0;
uint32_t conditionStartMillis = 0;
bool trackingBright = false;

const uint32_t LOG_INTERVAL_MS = 600000UL;  // 10 min
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
Adafruit_BME680 bme; // BME680: temp, hum, pressure, gas
//Adafruit_SGP40        sgp;
Adafruit_VEML7700 lightMeter;
unsigned long startMillis;
unsigned long serveWindow;
static bool loggedThisWake = false;
const uint8_t STATUS_LED_PIN = 4;
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
// —— Rain gauge state ——
volatile uint32_t rainTipCount = 0;
volatile uint32_t rainTipTimesMs[128];
volatile uint8_t  rainTipHead = 0;
volatile uint8_t  rainTipSize = 0;
volatile uint32_t lastRainTipMs = 0;
const uint32_t RAIN_DEBOUNCE_MS = 150;
// Critical section guard for ISR/task access
portMUX_TYPE rainMux = portMUX_INITIALIZER_UNLOCKED;

// ISR for tipping bucket; keep it IRAM safe and fast
void IRAM_ATTR rainIsr(){
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
// Boot start time (unix seconds) for the current boot session
time_t bootStartUnix = 0;

// HTML and URL helpers used in handleRoot()
static String htmlEscape(const String& in){
  String out; out.reserve(in.length()*12/10 + 4);
  for (size_t i = 0; i < in.length(); ++i) {
    char c = in[i];
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default:   out += c;       break;
    }
  }
  return out;
}

static String urlEncode(const String& s){
  const char* hex = "0123456789ABCDEF";
  String out; out.reserve(s.length()*3);
  for (size_t i = 0; i < s.length(); ++i) {
    unsigned char c = (unsigned char)s[i];
    bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
    if (safe) out += (char)c; else { out += '%'; out += hex[c >> 4]; out += hex[c & 0x0F]; }
  }
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
static inline float clampf(float v, float lo, float hi){ return v < lo ? lo : (v > hi ? hi : v); }
float computeDewPointC(float Tc, float RH){
  RH = clampf(RH, 1.0f, 100.0f);
  const float a = 17.62f, b = 243.12f;
  float gamma = log(RH/100.0f) + (a*Tc)/(b+Tc);
  return (b*gamma)/(a-gamma);
}
float computeHeatIndexF(float Tf, float RH){
  // Rothfusz regression; clamp to valid ranges
  Tf = clampf(Tf, -40.0f, 150.0f); RH = clampf(RH, 0.0f, 100.0f);
  float HI = -42.379f + 2.04901523f*Tf + 10.14333127f*RH
           - 0.22475541f*Tf*RH - 6.83783e-3f*Tf*Tf - 5.481717e-2f*RH*RH
           + 1.22874e-3f*Tf*Tf*RH + 8.5282e-4f*Tf*RH*RH - 1.99e-6f*Tf*Tf*RH*RH;
  return HI;
}
float computeMSLP_hPa(float P_hPa, float Tc, float altitudeM){
  // Barometric formula
  return P_hPa * powf(1.0f - (0.0065f * altitudeM) / (Tc + 273.15f + 0.0065f * altitudeM), -5.257f);
}

// Approximate wet-bulb temperature (°C) from air temp (°C) and RH (%) using Stull (2011)
// Valid for typical weather ranges; average error ~0.3 °C
float computeWetBulbC(float Tc, float RH){
  RH = clampf(RH, 1.0f, 100.0f);
  float term1 = Tc * atanf(0.151977f * sqrtf(RH + 8.313659f));
  float term2 = atanf(Tc + RH);
  float term3 = atanf(RH - 1.676331f);
  float term4 = 0.00391838f * powf(RH, 1.5f) * atanf(0.023101f * RH);
  return term1 + term2 - term3 + term4 - 4.686035f;
}

// ===== Config persisted in Preferences ('app' namespace) =====
struct AppConfig { float altitudeM; bool tempF; bool pressureInHg; float batCal; bool time12h; };
Preferences appPrefs;
AppConfig appCfg = { 0.0f, true, false, 1.08f, false };

void loadAppConfig(){
  appPrefs.begin("app", false);
  String cfg = appPrefs.getString("cfg", "");
  if (cfg.length()) {
    StaticJsonDocument<256> d; if (deserializeJson(d, cfg)==DeserializationError::Ok){
      appCfg.altitudeM    = d["altitude_m"] | appCfg.altitudeM;
      appCfg.tempF        = d["temp_unit"]  ? (String((const char*)d["temp_unit"]) == "F") : appCfg.tempF;
      appCfg.pressureInHg = d["pressure_unit"] ? (String((const char*)d["pressure_unit"]) == "inHg") : appCfg.pressureInHg;
      appCfg.batCal       = d["bat_cal"] | appCfg.batCal;
      appCfg.time12h      = d["time_12h"] | appCfg.time12h;
    }
  }
  // Apply to globals
  batCal = appCfg.batCal;
}

void saveAppConfig(){
  StaticJsonDocument<256> d;
  d["altitude_m"]    = appCfg.altitudeM;
  d["temp_unit"]     = appCfg.tempF ? "F" : "C";
  d["pressure_unit"] = appCfg.pressureInHg ? "inHg" : "hPa";
  d["bat_cal"]       = appCfg.batCal;
  d["time_12h"]      = appCfg.time12h;
  String out; serializeJson(d, out);
  appPrefs.putString("cfg", out);
}

// ===== Pressure history (hourly) persisted across deep sleep =====
RTC_DATA_ATTR float pressureHourly_hPa[13] = {0};
RTC_DATA_ATTR uint32_t pressureHourlyUnix[13] = {0};
RTC_DATA_ATTR uint8_t pressureHourlyCount = 0;
RTC_DATA_ATTR uint8_t pressureHourlyHead = 0; // points to next write position

void updatePressureHistory(float P_hPa, uint32_t nowUnix){
  // store one sample per hour boundary
  if (pressureHourlyCount == 0) {
    pressureHourly_hPa[0] = P_hPa; pressureHourlyUnix[0] = nowUnix; pressureHourlyCount = 1; pressureHourlyHead = 1; return;
  }
  uint32_t lastUnix = pressureHourlyUnix[(pressureHourlyHead + 12) % 13];
  if (nowUnix - lastUnix >= 3600) {
    pressureHourly_hPa[pressureHourlyHead] = P_hPa;
    pressureHourlyUnix[pressureHourlyHead] = nowUnix;
    pressureHourlyHead = (pressureHourlyHead + 1) % 13;
    if (pressureHourlyCount < 13) pressureHourlyCount++;
  }
}

bool getPressureDelta(float hours, float currentP, float* outDelta){
  if (pressureHourlyCount < 2) return false;
  uint32_t nowUnix = pressureHourlyUnix[(pressureHourlyHead + 12) % 13];
  uint32_t targetAge = (uint32_t)(hours * 3600.0f);
  // find the oldest sample that is at least targetAge old
  int idx = (pressureHourlyHead + 12) % 13; // last stored
  for (int i = 0; i < pressureHourlyCount; ++i) {
    int j = (pressureHourlyHead + 12 - i + 13) % 13;
    if (nowUnix - pressureHourlyUnix[j] >= targetAge) { idx = j; break; }
  }
  float past = pressureHourly_hPa[idx];
  *outDelta = currentP - past;
  return true;
}

const char* classifyTrendFromDelta(float delta){
  const float threshold = 0.6f; // hPa
  if (delta > threshold) return "Rising";
  if (delta < -threshold) return "Falling";
  return "Steady";
}

const char* zambrettiSimple(float mslp_hPa, const char* trend){
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

void blinkStatus(int times, int durationMs) {
  pinMode(STATUS_LED_PIN, OUTPUT);
  // Schedule a non-blocking pulse sequence handled in updateStatusLed()
  ledPulseActive = true;
  ledPulseIntervalMs = (unsigned long)durationMs;
  ledPulseLastToggle = 0;  // force immediate toggle on next update
  ledPulseRemainingToggles = times * 2;  // on+off per cycle
}
float readLux() {
  float lx = lightMeter.readLux();
  if (lx < 0 || isnan(lx)) lx = 0;
  return lx;
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
    return;
  }
  nextLogMillis = now + LOG_INTERVAL_MS;

    // —— read sensors (BME680) ——
    bme.performReading();
    float Tc = bme.temperature;
    float T = Tc * 9.0 / 5.0 + 32.0;
    float H = bme.humidity;
    float P = bme.pressure / 100.0;
    float gasKOhm = bme.gas_resistance / 1000.0f;
    float L = readLux();
    analogSetPinAttenuation(ADC_BATTERY_PIN, ADC_11db);
    long acc = 0; const int N = 8;
    for (int i=0;i<N;i++){ acc += analogRead(ADC_BATTERY_PIN); delay(2); }
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
    float dewF = dewC * 9.0f/5.0f + 32.0f;
    float hiF  = computeHeatIndexF(T, H);
    // Pressure trend from history
    time_t nowUnix = time(nullptr);
    updatePressureHistory(P, (uint32_t)nowUnix);
    float d3=0, d6=0, d12=0; bool h3=false,h6=false,h12=false;
    h3 = getPressureDelta(3.0f, P, &d3);
    h6 = getPressureDelta(6.0f, P, &d6);
    h12 = getPressureDelta(12.0f, P, &d12);
    const char* trend = classifyTrendFromDelta(h3?d3:(h6?d6:(h12?d12:0)));

    // —— append to SD ——
    File f = SD.open("/logs.csv", FILE_APPEND);
    if (f) {
      // CSV: timestamp,temp_f,humidity,dew_f,hi_f,pressure,pressure_trend,lux,voltage,voc_kohm,boot_count
      f.printf("%s,%.1f,%.1f,%.1f,%.1f,%.2f,%s,%.1f,%.2f,%.1f,%lu\n",
               timestr, T, H, dewF, hiF, P, trend, L, Vbat, gasKOhm, (unsigned long)bootCount);
      f.close();
      // update in-memory and persisted last log timestamps
      lastSdLogUnix = (uint32_t)nowUnixTs;
      strncpy(lastSdLogTime, timestr, sizeof(lastSdLogTime));
      lastSdLogTime[sizeof(lastSdLogTime)-1] = '\0';
    } else {
      Serial.println("❌ Failed to open log file");
    }

    // —— always print for debugging ——
    Serial.printf("[LOG] %s  T=%.1f°F  H=%.1f%%  L=%.1flux  Batt=%.2fV\n",
                  timestr, T, H, L, Vbat);
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
    uint16_t v = (uint16_t)readLux();
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
  bool startupWindowActive = (serveWindow == UPTIME_STARTUP) &&
                             ((now - startMillis) < (serveWindow * 1000UL));
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
  server.send(200, "text/plain", "Restarting…");
  delay(100);     // allow TCP flush
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
  //pinMode(FAN_PIN, OUTPUT);
  //digitalWrite(FAN_PIN, HIGH);  // fan on when you wake -------------------------------------For fan code when installed

  // Initialize I²C bus before accessing any I²C peripherals (RTC, sensors)
  Wire.begin(I2C_SDA, I2C_SCL);

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

  // Choose serve window based on wake reason:
  if (reason == ESP_SLEEP_WAKEUP_EXT0 || reason == ESP_SLEEP_WAKEUP_TIMER) {
    // Every wake from deep sleep gets 2 minutes
    serveWindow = UPTIME_CONFIG;  // 120 seconds
  } else {
    // Cold boot or manual reset → full startup window, followed by a 2-minute decision run
    serveWindow = UPTIME_STARTUP;  // 1800 seconds
    postConfigStarted = false;
  }
  Serial.printf("Serve window = %lus\n", serveWindow);


  // —— I²C & Sensor Initialization ——
  // BME680 init (try common I2C addresses 0x76 then 0x77)
  bool bmeOk = bme.begin(0x76);
  if (!bmeOk) bmeOk = bme.begin(0x77);
  if (!bmeOk) {
    Serial.println("❌ BME680 not found!");
  } else {
    // Configure BME680 oversampling and gas heater
    bme.setTemperatureOversampling(BME680_OS_8X);
    bme.setHumidityOversampling(BME680_OS_2X);
    bme.setPressureOversampling(BME680_OS_4X);
    bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
    bme.setGasHeater(320, 150); // 320°C for 150 ms
    Serial.println("✅ BME680 initialized");
  }
  // VEML7700 init
  if (!lightMeter.begin()) {
    Serial.println("❌ VEML7700 not found!");
  } else {
    // Configure for wide range and fast updates similar to prior BH1750
    lightMeter.setGain(VEML7700_GAIN_1);
    lightMeter.setIntegrationTime(VEML7700_IT_100MS);
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
  luxBuffer[0] = (uint16_t)firstLux;
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
  // Also record a boot event row inline (no separate function)
  {
    struct tm ti; char timestrBoot[32] = "N/A";
    if (getLocalTime(&ti)) {
      if (appCfg.time12h) strftime(timestrBoot, sizeof(timestrBoot), "%Y-%m-%d %I:%M:%S %p", &ti);
      else                strftime(timestrBoot, sizeof(timestrBoot), "%Y-%m-%d %H:%M:%S", &ti);
    }
    File fboot = SD.open("/logs.csv", FILE_APPEND);
    if (fboot) {
      // timestamp + 8 empty fields + boot_count
      fboot.printf("%s,,,,,,,,,%lu\n", timestrBoot, (unsigned long)bootCount);
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
  if (MDNS.begin("WeatherStation1")) {//--------------------------------------------Change this per device 
    Serial.println("✅ mDNS responder started");
  } else {
    Serial.println("⚠️ mDNS failed");
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
      if (String(ssid) == WiFi.SSID(i)) { visible = true; break; }
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
  server.on("/config", HTTP_GET, [](){
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
    html += "<div><label>Altitude (m)</label><input name='alt' value='" + String(appCfg.altitudeM,1) + "'></div>";
    html += "<div><label>Temp Unit</label><select name='tu'><option value='F'" + String(appCfg.tempF?" selected":"") + ">F</option><option value='C'" + String(!appCfg.tempF?" selected":"") + ">C</option></select></div>";
    html += "<div><label>Pressure Unit</label><select name='pu'><option value='hPa'" + String(!appCfg.pressureInHg?" selected":"") + ">hPa</option><option value='inHg'" + String(appCfg.pressureInHg?" selected":"") + ">inHg</option></select></div>";
    html += "<div><label>Battery Calibration</label><input name='bc' value='" + String(appCfg.batCal,2) + "'></div>";
    html += "<div><label>Time Format</label><select name='tf'><option value='24'" + String(!appCfg.time12h?" selected":"") + ">24-hour</option><option value='12'" + String(appCfg.time12h?" selected":"") + ">12-hour</option></select></div>";
    html += "</div><button type='submit'>Save Settings</button></form>";
    html += "<p><a class='btn' href='/'>← Back to Dashboard</a></p>";
    html += "</div></body></html>";
    server.send(200, "text/html", html);
  });
  server.on("/config", HTTP_POST, [](){
    if (server.hasArg("alt")) appCfg.altitudeM = server.arg("alt").toFloat();
    if (server.hasArg("tu")) appCfg.tempF = (server.arg("tu") == "F");
    if (server.hasArg("pu")) appCfg.pressureInHg = (server.arg("pu") == "inHg");
    if (server.hasArg("bc")) appCfg.batCal = server.arg("bc").toFloat();
    if (server.hasArg("tf")) appCfg.time12h = (server.arg("tf") == "12");
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
      <h4>Lux</h4>
      <div id="luxVal" class="value">--</div>
      <canvas id="luxChart"></canvas>
    </div>
    <div class="card">
      <h4>VOC (kΩ)</h4>
      <div id="vocVal" class="value">--</div>
      <canvas id="vocChart"></canvas>
    </div>
    <div class="card">
      <h4>Rain (mm/h)</h4>
      <div id="rainVal" class="value">--</div>
      <canvas id="rainChart"></canvas>
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
    </div>
    <div class="card">
      <h4>Heat Index (°F)</h4>
      <div id="hiVal" class="value">--</div>
    </div>
    <div class="card">
      <h4>Pressure Trend</h4>
      <div id="trendVal" class="value">--</div>
    </div>
    <div class="card">
      <h4>MSLP (Sea Level Pressure)</h4>
      <div id="mslpVal" class="value">--</div>
    </div>
    <div class="card">
      <h4>Wet Bulb Temp (°F)</h4>
      <div id="wbtVal" class="value">--</div>
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
      <h4>Boot Count</h4>
      <div id="bootCountVal" class="value">--</div>
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
  document.getElementById('tempVal').textContent     = o.temp.toFixed(1);
  document.getElementById('humVal').textContent      = o.hum.toFixed(1);
  document.getElementById('pressureVal').textContent = o.pressure.toFixed(2);
  document.getElementById('luxVal').textContent      = o.lux.toFixed(0);
  if (document.getElementById('vocVal')) {
    document.getElementById('vocVal').textContent      = (o.voc_kohm ?? 0).toFixed(1);
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
  document.getElementById('dewVal').textContent = (o.dew_f ?? 0).toFixed(1);
  document.getElementById('hiVal').textContent = (o.hi_f ?? 0).toFixed(1);
  document.getElementById('mslpVal').textContent = o.pressure_unit === 'inHg' ?
    (o.mslp_inHg ?? 0).toFixed(2) : (o.mslp_hPa ?? 0).toFixed(1);
  document.getElementById('wbtVal').textContent = (o.wbt_f ?? 0).toFixed(1);
  if (document.getElementById('rainVal')) {
    document.getElementById('rainVal').textContent = (o.rain_mmph ?? 0).toFixed(2);
  }
  document.getElementById('trendVal').textContent = (o.pressure_trend ?? '');
  document.getElementById('sdStatus').innerHTML    =
    o.sd_ok    ? '<span style="color:#0f0">&#10003;</span>' 
               : '<span style="color:#f00">&#10007;</span>';

  document.getElementById('rtcStatus').innerHTML   =
    o.rtc_ok   ? '<span style="color:#0f0">&#10003;</span>' 
               : '<span style="color:#f00">&#10007;</span>';

  document.getElementById('bootCountVal').textContent =
    o.boot_count;


  liveData.push({
    temp: o.temp,
    hum: o.hum,
    pressure: o.pressure,
    lux: o.lux,
    batt: o.batt,
    voc: (o.voc_kohm ?? 0),
    rain: (o.rain_mmph ?? 0)
  });
  if (liveData.length > MAX_POINTS) liveData.shift();

  ['temp','hum','pressure','lux','batt','voc','rain'].forEach(f => {
    draw(f, f + 'Chart', liveData);
  });
}

// Generic line‐drawing on a <canvas>
function draw(field, id, data) {
  const c = document.getElementById(id),
        ctx = c.getContext('2d'),
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
  // Read in °C, convert to °F
  // BME680 requires performReading() to update values
  bme.performReading();
  float Tc = bme.temperature; // °C
  float T = Tc * 9.0 / 5.0 + 32.0; // °F

  float H = bme.humidity; // %
  float P = bme.pressure / 100.0F; // hPa
  float L = readLux();
  float gasKOhm = bme.gas_resistance / 1000.0f; // BME680 gas in kΩ
  analogSetPinAttenuation(ADC_BATTERY_PIN, ADC_11db);
  long acc = 0; const int N = 8;
  for (int i=0;i<N;i++){ acc += analogRead(ADC_BATTERY_PIN); delay(2); }
  int raw = acc / N;
  float v = (raw * 3.3f / 4095.0f) * VOLT_DIVIDER * batCal;

  bool sdOk = SD.exists("/logs.csv");
  bool rtcOk = rtcOkFlag;
  uint32_t bc = bootCount;
  // Rain in last hour (mm/h) using tipping bucket math from model
  const float MM_PER_TIP = 0.2794f; // calibrate per your bucket (mm per tip)
  uint32_t nowMs = millis();
  uint32_t tipsLastHour = 0;
  // Copy volatile state under critical section to avoid races
  uint8_t sizeCopy, headCopy; uint32_t timesCopy[128];
  portENTER_CRITICAL(&rainMux);
  sizeCopy = rainTipSize; headCopy = rainTipHead;
  for (uint8_t i=0;i<sizeCopy;i++) {
    int idx = (headCopy + 128 - 1 - i) % 128;
    timesCopy[i] = rainTipTimesMs[idx];
  }
  portEXIT_CRITICAL(&rainMux);
  for (uint8_t i=0;i<sizeCopy;i++) {
    if (nowMs - timesCopy[i] <= 3600000UL) tipsLastHour++; else break;
  }
  float rainRateMmH = tipsLastHour * MM_PER_TIP;
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
  float dewF = dewC * 9.0f/5.0f + 32.0f;
  float hiF  = computeHeatIndexF(T, H);
  float mslp_hPa = computeMSLP_hPa(P, Tc, appCfg.altitudeM);
  float mslp_inHg = mslp_hPa * 0.0295299830714f;
  float wbtC = computeWetBulbC(Tc, H);
  float wbtF = wbtC * 9.0f/5.0f + 32.0f;

  // Pressure history/trend
  time_t nowUnix = time(nullptr);
  updatePressureHistory(P, (uint32_t)nowUnix);
  float d3=0, d6=0, d12=0; bool h3=false,h6=false,h12=false;
  h3 = getPressureDelta(3.0f, P, &d3);
  h6 = getPressureDelta(6.0f, P, &d6);
  h12 = getPressureDelta(12.0f, P, &d12);
  const char* trend = classifyTrendFromDelta(h3?d3:(h6?d6:(h12?d12:0)));
  const char* forecast = zambrettiSimple(mslp_hPa, trend);

  // Build JSON (temp in °F)
  StaticJsonDocument<768> doc;
  doc["temp"] = T;
  doc["hum"] = H;
  doc["pressure"] = P;
  doc["lux"] = L;
  doc["batt"] = v;
  doc["voc_kohm"] = gasKOhm;
  doc["uptime"] = up;
  doc["heap"] = heap;
  doc["flash_free_kb"] = flashFreeKB;
  doc["time"] = timestr;  // ← our new timestamp
  doc["rain_mmph"] = rainRateMmH;
  // Boot started: compute from current local time minus uptime; format as time only
  {
    struct tm tnow;
    if (getLocalTime(&tnow)) {
      time_t nowsec = mktime(&tnow);
      time_t bootUnix = nowsec - (time_t)up;
      struct tm tb; localtime_r(&bootUnix, &tb);
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
  doc["hi_f"] = hiF;
  doc["wbt_f"] = wbtF;
  doc["mslp_hPa"] = mslp_hPa;
  doc["mslp_inHg"] = mslp_inHg;
  doc["pressure_trend"] = trend;
  doc["forecast"] = forecast;
  doc["pressure_unit"] = appCfg.pressureInHg ? "inHg" : "hPa";
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
    File f = SD.open("/logs.csv", FILE_APPEND);
    if (f) {
      // Enhanced CSV with dew, heat index, pressure trend, VOC, and boot count
      float dewF_once = (computeDewPointC(Tc, H) * 9.0f/5.0f) + 32.0f;
      float hiF_once  = computeHeatIndexF(T, H);
      const char* trend_once = trend;
      float gasKOhmOnce = bme.gas_resistance / 1000.0f;
      f.printf("%s,%.1f,%.1f,%.1f,%.1f,%.2f,%s,%.1f,%.2f,%.1f,%lu\n",
               timestr, T, H, dewF_once, hiF_once, P, trend_once, L, v, gasKOhmOnce, (unsigned long)bootCount);
      f.close();
      // Persist unix timestamp as well for formatting per user preference later
      time_t nowUnixTs = time(nullptr);
      lastSdLogUnix = (uint32_t)nowUnixTs;
      strncpy(lastSdLogTime, timestr, sizeof(lastSdLogTime));
      lastSdLogTime[sizeof(lastSdLogTime)-1] = '\0';
    }
    loggedThisWake = true;  // prevent further logs until next wake
  }
}


// —— Fallback AP Mode for configuration ——
void startAPMode() {
  Serial.println("⚙️ Starting AP EnvLogger_Config");
  WiFi.mode(WIFI_AP);
  WiFi.softAP("WeatherStation1", "12345678");//--------------------------------------------Change this per device 
  Serial.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());

  // indicate AP mode with 2 slow blinks
  blinkStatus(2, 300);
}




// —— OTA Handlers ——
void setupOTA() {
  ElegantOTA.begin(&server); // ElegantOTA at /update by default
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
  const size_t MAX_LINES = 200; // show last 200 rows
  File f = SD.open("/logs.csv", "r");
  if (!f) { server.send(500, "text/plain", "Failed to open logs.csv"); return; }

  // Collect lines
  std::vector<String> lines;
  lines.reserve(MAX_LINES + 4);
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    lines.push_back(line);
    if (lines.size() > MAX_LINES + 1) { // +1 to retain header
      lines.erase(lines.begin() + 1);   // drop oldest data line but keep header at index 0
    }
  }
  f.close();

  if (lines.empty()) { server.send(200, "text/html", "<p>No data.</p>"); return; }

  // Build HTML
  String html;
  html += "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'/>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'/>";
  html += "<meta http-equiv='Cache-Control' content='no-store'/>";
  html += "<meta http-equiv='Pragma' content='no-cache'/>";
  html += "<meta http-equiv='Expires' content='0'/>";
  html += "<title>Logs</title>";
  html += "<style>body{background:#121212;color:#eee;font-family:Arial,sans-serif;margin:0;padding:16px;}";
  html += ".wrap{max-width:1000px;margin:0 auto;}h2{margin:8px 0 16px;}table{width:100%;border-collapse:collapse;background:#1e1e1e;border-radius:6px;overflow:hidden;}";
  html += "th,td{padding:10px;border-bottom:1px solid #2a2a2a;text-align:left;}th{background:#222;}tr:nth-child(even){background:#181818;}";
  html += "a.btn{display:inline-block;background:#444;padding:10px 12px;border-radius:6px;color:#eee;text-decoration:none;margin:12px 6px 0 0;}";
  html += ".filters{margin:12px 0;display:flex;gap:8px;flex-wrap:wrap;align-items:center;} .filters input,.filters select{background:#2a2a2a;color:#eee;border:none;border-radius:6px;padding:8px;} .filters button{background:#3d85c6;color:#fff;border:none;border-radius:6px;padding:8px 12px;cursor:pointer;} .filters button:hover{transform:scale(1.03);}";
  html += "</style></head><body><div class='wrap'>";
  html += "<h2>Logs</h2>";
  html += "<a class='btn' href='/'>&larr; Back</a>";
  html += "<a class='btn' href='/download'>Download CSV</a>";
  html += "<h3 style='margin:12px 0 4px;'>Filters</h3>";
  // Filter controls
  html += "<div class='filters'>";
  html += "<label>Field</label><select id='fField'>";
  html += "<option value='1'>Temp (F)</option>";
  html += "<option value='2'>Hum (%)</option>";
  html += "<option value='3'>Dew Pt (F)</option>";
  html += "<option value='4'>Heat Index (F)</option>";
  html += "<option value='5'>Pressure (hPa)</option>";
  html += "<option value='7'>Lux</option>";
  html += "<option value='8'>Battery (V)</option>";
  html += "<option value='9'>VOC (kΩ)</option>";
  html += "<option value='10'>Boot Count</option>";
  html += "</select>";
  html += "<label>Type</label><select id='fType'>";
  html += "<option value='between'>Between</option>";
  html += "<option value='gte'>&ge; Min</option>";
  html += "<option value='lte'>&le; Max</option>";
  html += "</select>";
  html += "<label title='Click to set from column minimum' style='cursor:pointer' onclick=\"setMinFromColumn()\">Min</label><input id='fMin' type='number' step='any' placeholder='min'>";
  html += "<label title='Click to set from column maximum' style='cursor:pointer' onclick=\"setMaxFromColumn()\">Max</label><input id='fMax' type='number' step='any' placeholder='max'>";
  html += "<button onclick=\"setMaxFromColumn()\" title='Fill Max from selected field'>Max</button>";
  html += "<button onclick=\"setMinFromColumn()\" title='Fill Min from selected field'>Min</button>";
  html += "<button onclick=\"applyFilter()\">Apply Filter</button>";
  html += "<button onclick=\"clearFilter()\">Clear</button>";
  html += "</div>";

  // Friendly headers
  html += "<table><thead><tr>";
  html += "<th>Time</th><th>Temp (F)</th><th>Hum (%)</th><th>Dew Pt (F)</th><th>Heat Index (F)</th><th>Pressure (hPa)</th><th>Trend</th><th>Lux</th><th>Battery (V)</th><th>VOC (kΩ)</th><th>Boot Count</th>";
  html += "</tr></thead><tbody>";

  // Skip the CSV header if present
  size_t startIdx = 0;
  if (lines[0].startsWith("timestamp,")) startIdx = 1;

  for (size_t i = startIdx; i < lines.size(); ++i) {
    String ln = lines[i];
    // Split by comma into up to 11 fields (added voc_kohm and boot_count)
    String cols[11];
    int col = 0; int from = 0; int idx;
    while (col < 10 && (idx = ln.indexOf(',', from)) >= 0) {
      cols[col++] = ln.substring(from, idx);
      from = idx + 1;
    }
    // Grab remainder
    cols[col++] = ln.substring(from);

    // If old schema (6 cols): timestamp,temp,hum,pressure,lux,voltage
    // Map to new table with blanks for dew/hi/trend
    bool isOld = (col <= 6);

    html += "<tr>";
    html += "<td>" + cols[0] + "</td>"; // already formatted timestamp
    html += "<td>" + cols[1] + "</td>"; // temp
    html += "<td>" + cols[2] + "</td>"; // hum
    if (isOld) {
      html += "<td></td><td></td>";      // dew, hi empty
      html += "<td>" + cols[3] + "</td>"; // pressure
      html += "<td></td>";               // trend empty
      html += "<td>" + cols[4] + "</td>"; // lux
      html += "<td>" + cols[5] + "</td>"; // voltage
      html += "<td></td>";                 // voc empty
      html += "<td></td>";                 // boot_count empty
    } else {
      html += "<td>" + cols[3] + "</td>"; // dew_f
      html += "<td>" + cols[4] + "</td>"; // hi_f
      html += "<td>" + cols[5] + "</td>"; // pressure
      html += "<td>" + cols[6] + "</td>"; // trend
      html += "<td>" + cols[7] + "</td>"; // lux
      html += "<td>" + cols[8] + "</td>"; // voltage
      html += "<td>" + cols[9] + "</td>"; // voc_kohm
      html += "<td>" + cols[10] + "</td>"; // boot_count
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
  html += "<script>function autoRange(){var f=parseInt(document.getElementById('fField').value,10);var mm=computeColMinMax(f);if(!mm)return;document.getElementById('fMin').value=mm.min;document.getElementById('fMax').value=mm.max;applyFilter();}</script>";
  // Initialize from URL query parameters and auto-apply if present
  html += "<script>(function(){try{var p=new URLSearchParams(location.search);if(!p)return;var fieldMap={temp:1,temperature:1,hum:2,humidity:2,dew:3,dew_f:3,hi:4,heat:4,heat_index:4,pressure:5,lux:7,bat:8,batv:8,battery:8,voc:9,boot:10,boots:10,boot_count:10};var fld=p.get('field');if(fld){var v=fieldMap[fld.toLowerCase()];if(!v&&/^[0-9]+$/.test(fld))v=parseInt(fld,10);if(v)document.getElementById('fField').value=String(v);}var type=p.get('type');if(type){var t=type.toLowerCase();if(t==='gte'||t==='lte'||t==='between'){document.getElementById('fType').value=t;}}if(p.has('min')){document.getElementById('fMin').value=p.get('min');}if(p.has('max')){document.getElementById('fMax').value=p.get('max');}if(p.has('field')||p.has('min')||p.has('max')||p.has('type')){applyFilter();}}catch(e){}})();</script>";
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
    f.println("timestamp,temp_f,humidity,dew_f,hi_f,pressure,pressure_trend,lux,voltage,voc_kohm,boot_count");
    f.close();
  } else {
    Serial.println("❌ Failed to recreate logs.csv");
  }

  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}  // ← Make sure this closing brace ends handleReset

void handleSleep() {
  server.send(200, "text/html", "<p>Sleeping for 10 minutes…</p>");
  delay(200);
  blinkStatus(3, 200);

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

  // Daily NTP resync and DS3231 drift check at ~02:00 local
  static time_t lastSync = 0;
  time_t tnow = time(nullptr);
  if (tnow != 0) {
    struct tm lt; localtime_r(&tnow, &lt);
    if (lt.tm_hour == 2 && (tnow - lastSync) > 20*3600) { // at most once per day
      Serial.println("⏳ Daily time sync");
      configTzTime("EST5EDT,M3.2.0/2,M11.1.0/2", "pool.ntp.org");
      if (rtcOkFlag) {
        struct tm ti; if (getLocalTime(&ti)) {
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

  // ─── During 30‑min config window, still perform periodic logging ───
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
