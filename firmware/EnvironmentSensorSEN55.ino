
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <math.h>

#include <SensirionI2CSen5x.h>
#include <SparkFun_SCD4x_Arduino_Library.h>
#include <SparkFun_Qwiic_OLED.h>
#include <res/qw_fnt_5x7.h>

#include "config_settings.h"
#include "thingspeak_ca.h"

// =====================================================
// EnvironmentSensorSEN55 - Version 5.4
// Clock-synchronised reporting
// =====================================================

const uint32_t SENSOR_INTERVAL_MS = 5000;
const uint32_t REPORT_INTERVAL_MS =
    TELEMETRY_FREQUENCY_IN_SECONDS * 1000UL;

const uint32_t MAX_DATA_AGE_MS = 10000;
const uint32_t WIFI_RETRY_MS = 30000;
const uint32_t HTTP_TIMEOUT_MS = 3000;

const uint8_t MAX_FAILURES = 3;

const uint8_t SEN55_ADDRESS = 0x69;
const uint8_t SCD41_ADDRESS = 0x62;
const uint8_t OLED_ADDRESS = 0x3D;

const uint8_t LEFT_LABEL = 2;
const uint8_t LEFT_VALUE = 22;
const uint8_t RIGHT_LABEL = 56;
const uint8_t RIGHT_VALUE = 76;

SensirionI2CSen5x sen55;
SCD4x scd41;
Qwiic1in3OLED oled;

bool senOK = false;
bool scdOK = false;
bool oledOK = false;

uint8_t senFailures = 0;
uint8_t scdFailures = 0;

uint32_t lastSensorPoll = 0;
uint32_t lastWiFiAttempt = 0;
uint32_t lastLocalReport = 0;

bool ntpStarted = false;

// Clock-aligned scheduling
time_t lastReportedSlot = 0;
bool clockWasReady = false;

// =====================================================
// Measurement structures
// =====================================================

struct SENReading {
  float pm1 = NAN;
  float pm25 = NAN;
  float pm4 = NAN;
  float pm10 = NAN;
  float humidity = NAN;
  float temperature = NAN;
  float voc = NAN;
  float nox = NAN;

  uint32_t timestamp = 0;
  bool valid = false;
};

struct SCDReading {
  float co2 = NAN;
  float temperature = NAN;
  float humidity = NAN;

  uint32_t timestamp = 0;
  bool valid = false;
};

SENReading latestSEN;
SCDReading latestSCD;

SENReading reportSEN;
SCDReading reportSCD;

float peakVOC = NAN;
float peakPM25 = NAN;
float peakPM10 = NAN;

float reportPeakVOC = NAN;
float reportPeakPM25 = NAN;
float reportPeakPM10 = NAN;

// =====================================================
// I2C
// =====================================================

bool devicePresent(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

// =====================================================
// Wi-Fi and NTP
// =====================================================

void startWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  lastWiFiAttempt = millis();

  Serial.println("Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void maintainWiFi(uint32_t now) {
  if (WiFi.status() == WL_CONNECTED) {
    if (!ntpStarted) {
      Serial.println("Starting NTP");

      configTime(
        0, 0,
        "pool.ntp.org",
        "time.google.com"
      );

      ntpStarted = true;
    }
    return;
  }

  if ((uint32_t)(now - lastWiFiAttempt)
      >= WIFI_RETRY_MS) {
    Serial.println("WiFi reconnect attempt");
    WiFi.disconnect();
    startWiFi();
  }
}

bool clockReady() {
  return time(nullptr) > 1760000000;
}

// =====================================================
// Freshness
// =====================================================

bool senFresh(uint32_t now) {
  return senOK &&
         latestSEN.valid &&
         (uint32_t)(now - latestSEN.timestamp)
         <= MAX_DATA_AGE_MS;
}

bool scdFresh(uint32_t now) {
  return scdOK &&
         latestSCD.valid &&
         (uint32_t)(now - latestSCD.timestamp)
         <= MAX_DATA_AGE_MS;
}

// =====================================================
// Failure management
// =====================================================

void markSENFailure() {
  if (senFailures < MAX_FAILURES) senFailures++;

  if (senFailures >= MAX_FAILURES) {
    senOK = false;
    latestSEN = SENReading();
    Serial.println("SEN55 FAILED");
  }
}

void markSCDFailure() {
  if (scdFailures < MAX_FAILURES) scdFailures++;

  if (scdFailures >= MAX_FAILURES) {
    scdOK = false;
    latestSCD = SCDReading();
    Serial.println("SCD41 FAILED");
  }
}

// =====================================================
// Peak tracking
// =====================================================

void updatePeak(float value, float &peak) {
  if (!isfinite(value)) return;

  if (!isfinite(peak) || value > peak)
    peak = value;
}

// =====================================================
// SEN55
// =====================================================

void readSEN55() {
  if (!senOK) return;

  SENReading reading;

  uint16_t error = sen55.readMeasuredValues(
    reading.pm1,
    reading.pm25,
    reading.pm4,
    reading.pm10,
    reading.humidity,
    reading.temperature,
    reading.voc,
    reading.nox
  );

  if (error != 0) {
    Serial.print("SEN55 error: ");
    Serial.println(error);
    markSENFailure();
    return;
  }

  reading.timestamp = millis();
  reading.valid = true;

  latestSEN = reading;
  senFailures = 0;

  updatePeak(reading.voc, peakVOC);
  updatePeak(reading.pm25, peakPM25);
  updatePeak(reading.pm10, peakPM10);
}

// =====================================================
// SCD41
// =====================================================

void readSCD41() {
  if (!scdOK) return;

  if (!scd41.readMeasurement()) {
    if (latestSCD.valid &&
        (uint32_t)(millis() - latestSCD.timestamp)
        > MAX_DATA_AGE_MS) {
      markSCDFailure();
    }
    return;
  }

  uint16_t newCO2 = scd41.getCO2();

  if (newCO2 == 0) {
    markSCDFailure();
    return;
  }

  SCDReading reading;

  reading.co2 = newCO2;
  reading.temperature = scd41.getTemperature();
  reading.humidity = scd41.getHumidity();

  reading.timestamp = millis();
  reading.valid = true;

  latestSCD = reading;
  scdFailures = 0;
}

void pollSensors() {
  if (senOK) readSEN55();
  if (scdOK) readSCD41();
}

// =====================================================
// Independent recovery
// =====================================================

void attemptSENRecovery() {
  if (senOK) return;

  Serial.println("Attempting SEN55 recovery");

  if (!devicePresent(SEN55_ADDRESS)) {
    Serial.println("SEN55 absent");
    return;
  }

  sen55.begin(Wire);

  uint16_t error = sen55.deviceReset();

  if (error != 0) {
    Serial.println("SEN55 reset failed");
    return;
  }

  delay(1000);

  error = sen55.startMeasurement();

  if (error != 0) {
    Serial.println("SEN55 restart failed");
    return;
  }

  senFailures = 0;
  latestSEN = SENReading();
  senOK = true;

  Serial.println("SEN55 restarted");
}

void attemptSCDRecovery() {
  if (scdOK) return;

  Serial.println("Attempting SCD41 recovery");

  if (!devicePresent(SCD41_ADDRESS)) {
    Serial.println("SCD41 absent");
    return;
  }

  if (!scd41.begin(Wire)) {
    Serial.println("SCD41 restart failed");
    return;
  }

  scdFailures = 0;
  latestSCD = SCDReading();
  scdOK = true;

  Serial.println("SCD41 restarted");
}

// =====================================================
// Snapshot
// =====================================================

void captureSnapshot(uint32_t now) {
  reportSEN = SENReading();
  reportSCD = SCDReading();

  if (senFresh(now)) reportSEN = latestSEN;
  if (scdFresh(now)) reportSCD = latestSCD;

  reportPeakVOC = peakVOC;
  reportPeakPM25 = peakPM25;
  reportPeakPM10 = peakPM10;

  peakVOC = NAN;
  peakPM25 = NAN;
  peakPM10 = NAN;
}

// =====================================================
// OLED formatting
// =====================================================

void formatValue(
  float value,
  uint8_t decimals,
  char *buffer,
  size_t length
) {
  if (!isfinite(value)) {
    snprintf(buffer, length, "NIL");
  } else {
    snprintf(
      buffer, length,
      "%.*f", decimals, value
    );
  }
}

void drawValue(
  uint8_t x,
  uint8_t y,
  float value,
  uint8_t decimals
) {
  char buffer[16];

  formatValue(
    value, decimals,
    buffer, sizeof(buffer)
  );

  oled.text(x, y, buffer);
}

void drawLatestMax(
  uint8_t x,
  uint8_t y,
  float latest,
  float maximum,
  uint8_t decimals
) {
  char a[16];
  char b[16];
  char combined[36];

  formatValue(latest, decimals, a, sizeof(a));
  formatValue(maximum, decimals, b, sizeof(b));

  snprintf(
    combined, sizeof(combined),
    "%s/%s", a, b
  );

  int availableChars = (128 - x) / 6;

  if ((int)strlen(combined) > availableChars &&
      decimals > 0) {
    formatValue(latest, 0, a, sizeof(a));
    formatValue(maximum, 0, b, sizeof(b));

    snprintf(
      combined, sizeof(combined),
      "%s/%s", a, b
    );
  }

  if ((int)strlen(combined) > availableChars) {
    snprintf(combined, sizeof(combined), "%s", a);
  }

  oled.text(x, y, combined);
}

// =====================================================
// OLED update
// =====================================================

void updateOLED() {
  if (!oledOK) return;

  if (!devicePresent(OLED_ADDRESS)) {
    oledOK = false;
    Serial.println("OLED unavailable - headless mode");
    return;
  }

  oled.erase();
  oled.setFont(QW_FONT_5X7);

  const uint8_t Y0 = 0;
  const uint8_t Y1 = 10;
  const uint8_t Y2 = 20;
  const uint8_t Y3 = 30;
  const uint8_t Y4 = 40;

  // Left column
  oled.text(LEFT_LABEL, Y0, "CO2");
  drawValue(LEFT_VALUE, Y0, reportSCD.co2, 0);

  oled.text(LEFT_LABEL, Y1, "TMP");
  drawValue(LEFT_VALUE, Y1, reportSEN.temperature, 1);

  oled.text(LEFT_LABEL, Y2, "RH");
  drawValue(LEFT_VALUE, Y2, reportSEN.humidity, 1);

  oled.text(LEFT_LABEL, Y3, "NOx");
  drawValue(LEFT_VALUE, Y3, reportSEN.nox, 0);

  // Right column
  oled.text(RIGHT_LABEL, Y0, "VOC");
  drawLatestMax(
    RIGHT_VALUE, Y0,
    reportSEN.voc, reportPeakVOC, 0
  );

  oled.text(RIGHT_LABEL, Y1, "P1");
  drawValue(RIGHT_VALUE, Y1, reportSEN.pm1, 1);

  oled.text(RIGHT_LABEL, Y2, "P2");
  drawLatestMax(
    RIGHT_VALUE, Y2,
    reportSEN.pm25, reportPeakPM25, 1
  );

  oled.text(RIGHT_LABEL, Y3, "P4");
  drawValue(RIGHT_VALUE, Y3, reportSEN.pm4, 1);

  oled.text(RIGHT_LABEL, Y4, "P10");
  drawLatestMax(
    RIGHT_VALUE, Y4,
    reportSEN.pm10, reportPeakPM10, 1
  );

  if (WiFi.status() == WL_CONNECTED) {
    oled.text(2, 50, "WIFI OK");
  } else {
    oled.text(2, 50, "WIFI LOST");
  }

  oled.display();
}

// =====================================================
// Serial reporting
// =====================================================

void printValue(
  const char *label,
  float value,
  uint8_t decimals
) {
  Serial.print(label);

  if (isfinite(value))
    Serial.println(value, decimals);
  else
    Serial.println("NIL");
}

void printReport() {
  Serial.println();
  Serial.println("======= AIR QUALITY =======");

  printValue("CO2:       ", reportSCD.co2, 0);
  printValue("Temp:      ", reportSEN.temperature, 1);
  printValue("Humidity:  ", reportSEN.humidity, 1);
  printValue("NOx:       ", reportSEN.nox, 0);
  printValue("VOC:       ", reportSEN.voc, 0);

  printValue("PM1:       ", reportSEN.pm1, 1);
  printValue("PM2.5:     ", reportSEN.pm25, 1);
  printValue("PM4:       ", reportSEN.pm4, 1);
  printValue("PM10:      ", reportSEN.pm10, 1);

  Serial.println("--- Interval maxima ---");

  printValue("VOC Max:   ", reportPeakVOC, 0);
  printValue("PM2.5 Max: ", reportPeakPM25, 1);
  printValue("PM10 Max:  ", reportPeakPM10, 1);

  Serial.println("===========================");
}

// =====================================================
// ThingSpeak
// =====================================================

void appendField(
  String &body,
  uint8_t field,
  float value,
  uint8_t decimals
) {
  if (!isfinite(value)) return;

  body += "&field";
  body += String(field);
  body += "=";
  body += String(value, (unsigned int)decimals);
}

bool uploadChannel(
  uint32_t channelID,
  const char *writeKey,
  const String &fields
) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!clockReady()) {
    Serial.println("Upload skipped: clock not ready");
    return false;
  }
  if (fields.length() == 0) return false;

  WiFiClientSecure client;
  client.setCACert(THINGSPEAK_ROOT_CA);
  client.setTimeout(HTTP_TIMEOUT_MS);

  HTTPClient http;

  if (!http.begin(
      client,
      "https://api.thingspeak.com/update"
  )) {
    Serial.println("HTTPS initialization failed");
    return false;
  }

  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader(
    "Content-Type",
    "application/x-www-form-urlencoded"
  );

  String body = "api_key=";
  body += writeKey;
  body += fields;

  Serial.print("Uploading channel ");
  Serial.println(channelID);

  int httpCode = http.POST(body);

  String response = "";

  if (httpCode > 0) {
    response = http.getString();
    response.trim();
  }

  http.end();

  long entryID = response.toInt();

  if (httpCode == 200 && entryID > 0) {
    Serial.print("ThingSpeak ");
    Serial.print(channelID);
    Serial.print(" saved. Entry ID: ");
    Serial.println(entryID);
    return true;
  }

  Serial.print("ThingSpeak ");
  Serial.print(channelID);
  Serial.print(" failed. HTTP=");
  Serial.print(httpCode);
  Serial.print(" Response=");
  Serial.println(response);

  return false;
}

void uploadThingSpeak() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("ThingSpeak skipped: WiFi offline");
    return;
  }

  String channel1 = "";

  appendField(channel1, 1, reportSCD.co2, 0);
  appendField(channel1, 2, reportSEN.temperature, 2);
  appendField(channel1, 3, reportSEN.humidity, 2);
  appendField(channel1, 4, reportSEN.nox, 1);
  appendField(channel1, 5, reportSEN.voc, 1);
  appendField(channel1, 6, reportSEN.pm1, 2);
  appendField(channel1, 7, reportSEN.pm25, 2);
  appendField(channel1, 8, reportSEN.pm10, 2);

  String channel2 = "";

  appendField(channel2, 1, reportSEN.pm4, 2);
  appendField(channel2, 2, reportPeakVOC, 1);
  appendField(channel2, 3, reportPeakPM25, 2);
  appendField(channel2, 4, reportPeakPM10, 2);

  // Maximum one request per channel per cycle.
  if (channel1.length() > 0) {
    uploadChannel(
      THINGSPEAK_CHANNEL_1_ID,
      THINGSPEAK_CHANNEL_1_WRITE_KEY,
      channel1
    );
  }

  if (channel2.length() > 0) {
    uploadChannel(
      THINGSPEAK_CHANNEL_2_ID,
      THINGSPEAK_CHANNEL_2_WRITE_KEY,
      channel2
    );
  }
}

// =====================================================
// Reporting cycle
// =====================================================

void performReport() {
  // Obtain newest available sensor measurements.
  pollSensors();

  uint32_t snapshotTime = millis();

  captureSnapshot(snapshotTime);

  // Local reporting first.
  printReport();
  updateOLED();

  // Same snapshot sent to both channels.
  uploadThingSpeak();

  // Independent recovery, once per report cycle.
  if (!senOK) attemptSENRecovery();
  if (!scdOK) attemptSCDRecovery();
}

// =====================================================
// Clock-aligned scheduling
// =====================================================

void maintainReporting(uint32_t now) {
  if (clockReady()) {
    time_t currentTime = time(nullptr);

    // Round down to reporting boundary.
    // With a 60-second interval, these are
    // HH:MM:00 UTC, also aligned in Singapore.
    time_t currentSlot =
      (currentTime / TELEMETRY_FREQUENCY_IN_SECONDS) *
      TELEMETRY_FREQUENCY_IN_SECONDS;

    if (!clockWasReady) {
      // Do not upload immediately upon first
      // NTP sync. Wait for the next boundary.
      lastReportedSlot = currentSlot;
      clockWasReady = true;

      Serial.println(
        "Clock synchronized; awaiting next boundary"
      );
      return;
    }

    if (currentSlot > lastReportedSlot) {
      // Only one report, even if multiple
      // boundaries were missed.
      lastReportedSlot = currentSlot;
      lastLocalReport = now;

      struct tm timeInfo;
      gmtime_r(&currentTime, &timeInfo);

      Serial.printf(
        "Scheduled report: %02d:%02d:%02d UTC\n",
        timeInfo.tm_hour,
        timeInfo.tm_min,
        timeInfo.tm_sec
      );

      performReport();
    }

  } else {
    // No valid clock: maintain local reporting
    // without cloud uploads.
    clockWasReady = false;

    if ((uint32_t)(now - lastLocalReport)
        >= REPORT_INTERVAL_MS) {

      lastLocalReport = now;

      pollSensors();
      captureSnapshot(millis());
      printReport();
      updateOLED();

      if (!senOK) attemptSENRecovery();
      if (!scdOK) attemptSCDRecovery();
    }
  }
}

// =====================================================
// Setup
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("EnvironmentSensorSEN55");
  Serial.println("Version 5.4 - Clock-Aligned Reporting");
  Serial.print("Device: ");
  Serial.println(DEVICE_ID);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000);
  Wire.setTimeOut(50);

  // SEN55
  if (devicePresent(SEN55_ADDRESS)) {
    sen55.begin(Wire);
    sen55.deviceReset();
    delay(1000);

    if (sen55.startMeasurement() == 0) {
      senOK = true;
      Serial.println("SEN55 started");
    } else {
      Serial.println("SEN55 start failed");
    }
  } else {
    Serial.println("SEN55 absent");
  }

  // SCD41
  if (devicePresent(SCD41_ADDRESS)) {
    if (scd41.begin(Wire)) {
      scdOK = true;
      Serial.println("SCD41 started");
    } else {
      Serial.println("SCD41 start failed");
    }
  } else {
    Serial.println("SCD41 absent");
  }

  // OLED optional
  if (devicePresent(OLED_ADDRESS) &&
      oled.begin(Wire)) {
    oledOK = true;
    Serial.println("OLED started");
    updateOLED();
  } else {
    Serial.println("OLED unavailable; headless mode");
  }

  // Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  startWiFi();

  lastSensorPoll = millis();
  lastLocalReport = millis();

  Serial.println("Sensor polling: 5 seconds");

  Serial.print("Reporting interval: ");
  Serial.print(TELEMETRY_FREQUENCY_IN_SECONDS);
  Serial.println(" seconds");

  Serial.println("Awaiting NTP synchronization");
}

// =====================================================
// Main loop
// =====================================================

void loop() {
  uint32_t now = millis();

  maintainWiFi(now);

  // Sensor acquisition every five seconds.
  if ((uint32_t)(now - lastSensorPoll)
      >= SENSOR_INTERVAL_MS) {

    lastSensorPoll = now;
    pollSensors();
  }

  // Reporting aligned to NTP clock.
  maintainReporting(millis());

  delay(20);
}
