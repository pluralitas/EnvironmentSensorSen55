#ifndef CONFIG_SETTINGS_H
#define CONFIG_SETTINGS_H

// EnvironmentSensorSEN55 configuration

#define DEVICE_ID "esp32_envsens"

// Wi-Fi
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// ThingSpeak
#define THINGSPEAK_CHANNEL_1_ID 3591940
#define THINGSPEAK_CHANNEL_1_WRITE_KEY "YOUR_WRITE_KEY_1"

#define THINGSPEAK_CHANNEL_2_ID 3591941
#define THINGSPEAK_CHANNEL_2_WRITE_KEY "YOUR_WRITE_KEY_2"

// Reporting interval (seconds)
#define TELEMETRY_FREQUENCY_IN_SECONDS 60

// SparkFun Thing Plus ESP32-WROOM Qwiic pins
#define I2C_SDA_PIN 23
#define I2C_SCL_PIN 22

#endif