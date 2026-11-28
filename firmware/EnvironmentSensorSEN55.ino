
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
// EnvironmentSensorSEN55 - Version 5.5
// SparkFun Thing Plus ESP32-WROOM
// Independent sensor recovery
// Separate FreeRTOS HTTPS task
// Clock-aligned reporting
// =====================================================

constexpr uint32_t SENSOR_INTERVAL_MS = 5000;
constexpr uint32_t REPORT_INTERVAL_MS =
    TELEMETRY_FREQUENCY_IN_SECONDS * 1000UL;

constexpr uint32_t MAX_DATA_AGE_MS = 10000;
constexpr uint32_t FIRST_READING_TIMEOUT_MS = 20000;
constexpr uint32_t WIFI_RETRY_MS = 30000;
constexpr uint32_t HTTP_TIMEOUT_MS = 5000;

constexpr uint8_t MAX_FAILURES = 3;

constexpr uint8_t SEN55_ADDRESS = 0x69;
constexpr uint8_t SCD41_ADDRESS = 0x62;
constexpr uint8_t OLED_ADDRESS = 0x3D;

// OLED layout
constexpr uint8_t LEFT_LABEL = 2;
constexpr uint8_t LEFT_VALUE = 22;
constexpr uint8_t RIGHT_LABEL = 56;
constexpr uint8_t RIGHT_VALUE = 76;

// =====================================================
// Device objects
// =====================================================

SensirionI2CSen5x sen55;
SCD4x scd41;
Qwiic1in3OLED oled;

// =====================================================
// Sensor health
// =====================================================

struct SensorHealth {
  bool running = false;
  bool verified = false;
  uint8_t failures = 0;
  uint32_t startedAt = 0;
  uint32_t lastValidAt = 0;
};

SensorHealth senHealth;
SensorHealth scdHealth;

bool oledOK = false;

// =====================================================
// Sensor readings
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

// =====================================================
// Reporting snapshot
// =====================================================

struct ReportSnapshot {
  SENReading sen;
  SCDReading scd;

  float peakVOC = NAN;
  float peakPM25 = NAN;
  float peakPM10 = NAN;

  time_t reportTime = 0;
};

ReportSnapshot displaySnapshot;

// Current interval maxima
float peakVOC = NAN;
float peakPM25 = NAN;
float peakPM10 = NAN;

// =====================================================
// FreeRTOS network task
// =====================================================

QueueHandle_t uploadQueue = nullptr;
TaskHandle_t uploadTaskHandle = nullptr;

// =====================================================
// Timers
// =====================================================

uint32_t lastSensorPoll = 0;
uint32_t lastWiFiAttempt = 0;
uint32_t lastLocalReport = 0;

time_t lastReportedSlot = 0;

bool ntpStarted = false;
bool clockWasReady = false;

// =====================================================
// I2C
// =====================================================

bool devicePresent(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

// =====================================================
// Wi-Fi
// =====================================================

void startWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  lastWiFiAttempt = millis();

  Serial.println("WiFi: connecting");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void maintainWiFi(uint32_t now) {
  if (WiFi.status() == WL_CONNECTED) {
    if (!ntpStarted) {
      configTime(
        0, 0,
        "pool.ntp.org",
        "time.google.com"
      );

      ntpStarted = true;
      Serial.println("NTP: started");
    }

    return;
  }

  if ((uint32_t)(now - lastWiFiAttempt)
      >= WIFI_RETRY_MS) {

    Serial.println("WiFi: reconnecting");

    WiFi.disconnect();
    startWiFi();
  }
}

bool clockReady() {
  return time(nullptr) > 1760000000;
}

// =====================================================
// Health management
// =====================================================

void markFailed(
  SensorHealth &health,
  const char *name
) {
  if (health.failures < MAX_FAILURES) {
    health.failures++;
  }

  if (health.failures >= MAX_FAILURES) {
    health.running = false;
    health.verified = false;

    Serial.print(name);
    Serial.println(": FAILED");
  }
}

void markHealthy(SensorHealth &health) {
  health.running = true;
  health.verified = true;
  health.failures = 0;
  health.lastValidAt = millis();
}

bool sensorOverdue(
  const SensorHealth &health,
  uint32_t now
) {
  if (!health.running) return false;

  if (!health.verified) {
    return (uint32_t)(now - health.startedAt)
      > FIRST_READING_TIMEOUT_MS;
  }

  return (uint32_t)(now - health.lastValidAt)
    > MAX_DATA_AGE_MS;
}

// =====================================================
// Freshness
// =====================================================

bool senFresh(uint32_t now) {
  return senHealth.running &&
         senHealth.verified &&
         latestSEN.valid &&
         (uint32_t)(now - latestSEN.timestamp)
         <= MAX_DATA_AGE_MS;
}

bool scdFresh(uint32_t now) {
  return scdHealth.running &&
         scdHealth.verified &&
         latestSCD.valid &&
         (uint32_t)(now - latestSCD.timestamp)
         <= MAX_DATA_AGE_MS;
}

// =====================================================
// Maxima
// =====================================================

void updatePeak(float value, float &peak) {
  if (!isfinite(value)) return;

  if (!isfinite(peak) || value > peak) {
    peak = value;
  }
}

// =====================================================
// SEN55
// =====================================================

bool startSEN55() {
  if (!devicePresent(SEN55_ADDRESS)) {
    return false;
  }

  sen55.begin(Wire);

  uint16_t error = sen55.deviceReset();

  if (error != 0) return false;

  delay(1000);

  error = sen55.startMeasurement();

  if (error != 0) return false;

  senHealth = SensorHealth();
  senHealth.running = true;
  senHealth.startedAt = millis();

  latestSEN = SENReading();

  Serial.println("SEN55: measurement started");

  return true;
}

void readSEN55() {
  if (!senHealth.running) return;

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
    Serial.print("SEN55 read error: ");
    Serial.println(error);

    markFailed(senHealth, "SEN55");
    return;
  }

  // Require at least one valid physical reading.
  if (!isfinite(reading.pm25)) {
    markFailed(senHealth, "SEN55");
    return;
  }

  reading.timestamp = millis();
  reading.valid = true;

  // SEN55 may report unavailable gas indices
  // during conditioning.
  if (!isfinite(reading.voc) ||
      reading.voc <= 0) {
    reading.voc = NAN;
  }

  if (!isfinite(reading.nox) ||
      reading.nox <= 0) {
    reading.nox = NAN;
  }

  latestSEN = reading;
  markHealthy(senHealth);

  updatePeak(reading.voc, peakVOC);
  updatePeak(reading.pm25, peakPM25);
  updatePeak(reading.pm10, peakPM10);
}

// =====================================================
// SCD41
// =====================================================

bool startSCD41() {
  if (!devicePresent(SCD41_ADDRESS)) {
    return false;
  }

  if (!scd41.begin(Wire)) {
    return false;
  }

  scdHealth = SensorHealth();
  scdHealth.running = true;
  scdHealth.startedAt = millis();

  latestSCD = SCDReading();

  Serial.println("SCD41: measurement started");

  return true;
}

void readSCD41() {
  if (!scdHealth.running) return;

  if (!scd41.readMeasurement()) {
    if (sensorOverdue(scdHealth, millis())) {
      markFailed(scdHealth, "SCD41");
    }
    return;
  }

  uint16_t newCO2 = scd41.getCO2();

  if (newCO2 == 0) {
    markFailed(scdHealth, "SCD41");
    return;
  }

  SCDReading reading;

  reading.co2 = newCO2;
  reading.temperature = scd41.getTemperature();
  reading.humidity = scd41.getHumidity();

  reading.timestamp = millis();
  reading.valid = true;

  latestSCD = reading;
  markHealthy(scdHealth);
}

// =====================================================
// Poll both sensors
// =====================================================

void pollSensors() {
  if (senHealth.running) {
    readSEN55();

    if (sensorOverdue(senHealth, millis())) {
      markFailed(senHealth, "SEN55");
    }
  }

  if (scdHealth.running) {
    readSCD41();
  }
}

// =====================================================
// Independent recovery
// =====================================================

void attemptSensorRecovery() {
  // Called only once per reporting cycle.

  if (!senHealth.running) {
    Serial.println("SEN55: recovery attempt");

    if (!startSEN55()) {
      Serial.println("SEN55: recovery unsuccessful");
    }
  }

  if (!scdHealth.running) {
    Serial.println("SCD41: recovery attempt");

    if (!startSCD41()) {
      Serial.println("SCD41: recovery unsuccessful");
    }
  }
}

// =====================================================
// Capture latest snapshot
// =====================================================

ReportSnapshot captureSnapshot(uint32_t now) {
  ReportSnapshot snapshot;

  if (senFresh(now)) {
    snapshot.sen = latestSEN;
  }

  if (scdFresh(now)) {
    snapshot.scd = latestSCD;
  }

  snapshot.peakVOC = peakVOC;
  snapshot.peakPM25 = peakPM25;
  snapshot.peakPM10 = peakPM10;

  snapshot.reportTime = time(nullptr);

  peakVOC = NAN;
  peakPM25 = NAN;
  peakPM10 = NAN;

  return snapshot;
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
      buffer,
      length,
      "%.*f",
      decimals,
      value
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
    value,
    decimals,
    buffer,
    sizeof(buffer)
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
    combined,
    sizeof(combined),
    "%s/%s",
    a,
    b
  );

  int availableChars = (128 - x) / 6;

  if ((int)strlen(combined) > availableChars &&
      decimals > 0) {

    formatValue(latest, 0, a, sizeof(a));
    formatValue(maximum, 0, b, sizeof(b));

    snprintf(
      combined,
      sizeof(combined),
      "%s/%s",
      a,
      b
    );
  }

  if ((int)strlen(combined) > availableChars) {
    snprintf(combined, sizeof(combined), "%s", a);
  }

  oled.text(x, y, combined);
}

// =====================================================
// OLED display
// =====================================================

void updateOLED(const ReportSnapshot &snapshot) {
  if (!oledOK) return;

  if (!devicePresent(OLED_ADDRESS)) {
    oledOK = false;
    Serial.println("OLED disconnected; headless mode");
    return;
  }

  oled.erase();
  oled.setFont(QW_FONT_5X7);

  // Left column
  oled.text(LEFT_LABEL, 0, "CO2");
  drawValue(
    LEFT_VALUE, 0,
    snapshot.scd.co2, 0
  );

  oled.text(LEFT_LABEL, 10, "TMP");
  drawValue(
    LEFT_VALUE, 10,
    snapshot.sen.temperature, 1
  );

  oled.text(LEFT_LABEL, 20, "RH");
  drawValue(
    LEFT_VALUE, 20,
    snapshot.sen.humidity, 1
  );

  oled.text(LEFT_LABEL, 30, "NOx");
  drawValue(
    LEFT_VALUE, 30,
    snapshot.sen.nox, 0
  );

  // Right column
  oled.text(RIGHT_LABEL, 0, "VOC");
  drawLatestMax(
    RIGHT_VALUE, 0,
    snapshot.sen.voc,
    snapshot.peakVOC, 0
  );

  oled.text(RIGHT_LABEL, 10, "P1");
  drawValue(
    RIGHT_VALUE, 10,
    snapshot.sen.pm1, 1
  );

  oled.text(RIGHT_LABEL, 20, "P2");
  drawLatestMax(
    RIGHT_VALUE, 20,
    snapshot.sen.pm25,
    snapshot.peakPM25, 1
  );

  oled.text(RIGHT_LABEL, 30, "P4");
  drawValue(
    RIGHT_VALUE, 30,
    snapshot.sen.pm4, 1
  );

  oled.text(RIGHT_LABEL, 40, "P10");
  drawLatestMax(
    RIGHT_VALUE, 40,
    snapshot.sen.pm10,
    snapshot.peakPM10, 1
  );

  // Wi-Fi status
  oled.text(
    2, 50,
    WiFi.status() == WL_CONNECTED
      ? "WIFI OK"
      : "WIFI LOST"
  );

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

  if (isfinite(value)) {
    Serial.println(value, decimals);
  } else {
    Serial.println("NIL");
  }
}

void printReport(const ReportSnapshot &snapshot) {
  Serial.println();
  Serial.println("======= AIR QUALITY V5.5 =======");

  printValue("CO2:       ", snapshot.scd.co2, 0);
  printValue("Temp:      ", snapshot.sen.temperature, 1);
  printValue("Humidity:  ", snapshot.sen.humidity, 1);
  printValue("NOx:       ", snapshot.sen.nox, 0);
  printValue("VOC:       ", snapshot.sen.voc, 0);

  printValue("PM1:       ", snapshot.sen.pm1, 1);
  printValue("PM2.5:     ", snapshot.sen.pm25, 1);
  printValue("PM4:       ", snapshot.sen.pm4, 1);
  printValue("PM10:      ", snapshot.sen.pm10, 1);

  Serial.println("--- Interval maxima ---");

  printValue("VOC Max:   ", snapshot.peakVOC, 0);
  printValue("PM2.5 Max: ", snapshot.peakPM25, 1);
  printValue("PM10 Max:  ", snapshot.peakPM10, 1);

  Serial.println("--- Sensor status ---");

  Serial.print("SEN55: ");
  Serial.println(
    senHealth.verified ? "RUNNING" : "UNAVAILABLE"
  );

  Serial.print("SCD41: ");
  Serial.println(
    scdHealth.verified ? "RUNNING" : "UNAVAILABLE"
  );

  Serial.println("===============================");
}

// =====================================================
// ThingSpeak fields
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

  body += String(
    value,
    (unsigned int)decimals
  );
}

// =====================================================
// HTTPS upload
// =====================================================

bool uploadChannel(
  uint32_t channelID,
  const char *writeKey,
  const String &fields
) {
  if (fields.isEmpty()) return false;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("ThingSpeak: WiFi unavailable");
    return false;
  }

  if (!clockReady()) {
    Serial.println("ThingSpeak: clock unavailable");
    return false;
  }

  WiFiClientSecure client;

  client.setCACert(THINGSPEAK_ROOT_CA);
  client.setTimeout(HTTP_TIMEOUT_MS);

  HTTPClient http;

  if (!http.begin(
    client,
    "https://api.thingspeak.com/update"
  )) {
    Serial.println("ThingSpeak: HTTPS init failed");
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

  int httpCode = http.POST(body);

  String response;

  if (httpCode > 0) {
    response = http.getString();
    response.trim();
  }

  http.end();

  long entryID = response.toInt();

  if (httpCode == 200 && entryID > 0) {
    Serial.printf(
      "ThingSpeak %lu saved. Entry ID: %ld\n",
      (unsigned long)channelID,
      entryID
    );

    return true;
  }

  Serial.printf(
    "ThingSpeak %lu failed. HTTP=%d\n",
    (unsigned long)channelID,
    httpCode
  );

  return false;
}

// =====================================================
// Upload frozen snapshot
// =====================================================

void uploadSnapshot(const ReportSnapshot &snapshot) {
  String channel1;
  String channel2;

  // Channel 1
  appendField(channel1, 1, snapshot.scd.co2, 0);
  appendField(channel1, 2, snapshot.sen.temperature, 2);
  appendField(channel1, 3, snapshot.sen.humidity, 2);
  appendField(channel1, 4, snapshot.sen.nox, 1);
  appendField(channel1, 5, snapshot.sen.voc, 1);
  appendField(channel1, 6, snapshot.sen.pm1, 2);
  appendField(channel1, 7, snapshot.sen.pm25, 2);
  appendField(channel1, 8, snapshot.sen.pm10, 2);

  // Channel 2
  appendField(channel2, 1, snapshot.sen.pm4, 2);
  appendField(channel2, 2, snapshot.peakVOC, 1);
  appendField(channel2, 3, snapshot.peakPM25, 2);
  appendField(channel2, 4, snapshot.peakPM10, 2);

  // At most one attempt per channel per snapshot.
  if (!channel1.isEmpty()) {
    uploadChannel(
      THINGSPEAK_CHANNEL_1_ID,
      THINGSPEAK_CHANNEL_1_WRITE_KEY,
      channel1
    );
  }

  if (!channel2.isEmpty()) {
    uploadChannel(
      THINGSPEAK_CHANNEL_2_ID,
      THINGSPEAK_CHANNEL_2_WRITE_KEY,
      channel2
    );
  }
}

// =====================================================
// FreeRTOS upload worker
// =====================================================

void thingSpeakTask(void *parameter) {
  ReportSnapshot snapshot;

  for (;;) {
    if (xQueueReceive(
      uploadQueue,
      &snapshot,
      portMAX_DELAY
    ) == pdTRUE) {

      uploadSnapshot(snapshot);
    }
  }
}

// =====================================================
// Reporting
// =====================================================

void performReport(bool allowUpload) {
  // Final poll before snapshot.
  pollSensors();

  ReportSnapshot snapshot =
    captureSnapshot(millis());

  displaySnapshot = snapshot;

  printReport(snapshot);
  updateOLED(snapshot);

  // Send only if the network task is idle.
  if (allowUpload &&
      uploadQueue != nullptr &&
      WiFi.status() == WL_CONNECTED) {

    // Do not overwrite an existing pending report.
    if (uxQueueMessagesWaiting(uploadQueue) == 0) {
      if (xQueueSend(
        uploadQueue,
        &snapshot,
        0
      ) != pdTRUE) {
        Serial.println("Upload skipped: queue busy");
      }
    } else {
      Serial.println("Upload skipped: previous pending");
    }
  }

  // Recover independently, once per report.
  attemptSensorRecovery();
}

// =====================================================
// Clock-aligned scheduler
// =====================================================

void maintainReporting(uint32_t now) {
  if (clockReady()) {
    time_t currentTime = time(nullptr);

    time_t currentSlot =
      (currentTime / TELEMETRY_FREQUENCY_IN_SECONDS) *
      TELEMETRY_FREQUENCY_IN_SECONDS;

    if (!clockWasReady) {
      lastReportedSlot = currentSlot;
      clockWasReady = true;

      Serial.println("Clock synchronized");
      return;
    }

    if (currentSlot > lastReportedSlot) {
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

      performReport(true);
    }

  } else {
    clockWasReady = false;

    // Local-only reporting while NTP is unavailable.
    if ((uint32_t)(now - lastLocalReport)
        >= REPORT_INTERVAL_MS) {

      lastLocalReport = now;
      performReport(false);
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
  Serial.println("Version 5.5");
  Serial.print("Device: ");
  Serial.println(DEVICE_ID);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000);
  Wire.setTimeOut(50);

  // SEN55
  if (!startSEN55()) {
    Serial.println("SEN55 unavailable at startup");
  }

  // SCD41
  if (!startSCD41()) {
    Serial.println("SCD41 unavailable at startup");
  }

  // OLED is optional
  if (devicePresent(OLED_ADDRESS) &&
      oled.begin(Wire)) {

    oledOK = true;
    Serial.println("OLED started");

    updateOLED(displaySnapshot);

  } else {
    Serial.println("OLED unavailable; headless mode");
  }

  // Network task
  uploadQueue = xQueueCreate(
    1,
    sizeof(ReportSnapshot)
  );

  if (uploadQueue != nullptr) {
    BaseType_t result = xTaskCreate(
      thingSpeakTask,
      "ThingSpeak",
      12288,
      nullptr,
      1,
      &uploadTaskHandle
    );

    if (result != pdPASS) {
      Serial.println("Network task creation failed");
      vQueueDelete(uploadQueue);
      uploadQueue = nullptr;
    }
  } else {
    Serial.println("Upload queue creation failed");
  }

  // Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  startWiFi();

  lastSensorPoll = millis();
  lastLocalReport = millis();

  Serial.println("Sensor interval: 5 seconds");

  Serial.printf(
    "Report interval: %lu seconds\n",
    (unsigned long)TELEMETRY_FREQUENCY_IN_SECONDS
  );

  Serial.println("Initialization complete");
}

// =====================================================
// Main loop
// =====================================================

void loop() {
  uint32_t now = millis();

  maintainWiFi(now);

  if ((uint32_t)(now - lastSensorPoll)
      >= SENSOR_INTERVAL_MS) {

    lastSensorPoll = now;
    pollSensors();
  }

  maintainReporting(millis());

  delay(20);
}
