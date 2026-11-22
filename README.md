# EnvironmentSensorSen55

ESP32-based bedroom air-quality monitoring system using the SparkFun Air Quality Combo (Sensirion SEN55 + SCD41), a Qwiic OLED, ThingSpeak for historical telemetry, and GitHub Pages for visualisation.

**Project status:** Working prototype. Sensor readings, OLED output, two ThingSpeak uploads, and the public dashboard have been demonstrated. The final acrylic mounting plate and backup battery are pending.

## Live dashboard

**[Bedroom Air Quality Dashboard](https://pluralitas.github.io/EnvironmentSensorSen55/)**

Data is stored in two public ThingSpeak channels:

- [Bedroom Air Quality — 3591940](https://thingspeak.mathworks.com/channels/3591940)
- [Bedroom Air Quality - 2 — 3591941](https://thingspeak.mathworks.com/channels/3591941)

The browser-based dashboard reads ThingSpeak directly. The ESP32 does **not** communicate with GitHub.

## Architecture

```text
SEN55 + SCD41 (SparkFun Air Quality Combo)
                  |
             Qwiic I2C
                  |
          ESP32 Thing Plus -------- Qwiic OLED 1.3"
                  |
             Wi-Fi / HTTPS
                  |
        ThingSpeak (2 channels)
                  |
           Public Read API
                  |
       GitHub Pages dashboard
                  |
         Browser / CSV export
```

The physical Qwiic chain is **ESP32 → Air Quality Combo → OLED**. The OLED is optional: if removed before reboot, the ESP32 should continue collecting and uploading data, provided the shared I2C bus is healthy.

## Hardware

| Component | Description |
| --- | --- |
| Controller | [SparkFun ESP32 Thing Plus (WRL-15663)](https://www.sparkfun.com/products/15663), ESP32-WROOM, Micro-USB version |
| Air-quality board | [SparkFun Air Quality Combo](https://www.electronics-lab.com/news/sparkfuns-new-qwiic-enabled-indoor-air-quality-sensor-combines-scd41-and-sen55-sensors/), SEN55 + SCD41 |
| Display | [SparkFun Qwiic OLED 1.3-inch (LCD-23453)](https://www.sparkfun.com/products/23453), 128 × 64 |
| Backup battery | 3.7 V, 1800 mAh LiPo |
| Power | USB power, currently via powered hub |

### I2C wiring

| Signal/device | Value |
| --- | --- |
| SDA | GPIO23 |
| SCL | GPIO22 |
| SEN55 address | `0x69` |
| SCD41 address | `0x62` |
| OLED address | `0x3D` |

These pin assignments were confirmed using an I2C scanner on the actual board. All three devices share the Qwiic bus.

### Acrylic mounting plate

The provisional plate measures **90 × 50 × 3 mm**. Four mounting holes are used for the Air Quality Combo, with **Ø3.4 mm** clearance for M3 hardware and 30 mm standoffs. The OLED is mounted above the plate on short standoffs; the ESP32 board is attached using double-sided tape. Verify the hole pattern and hardware clearances with a 1:1 paper print before laser cutting.

## Firmware

Main sketch: `EnvironmentSensorSEN55.ino` (working baseline **V5.4**).

### Behaviour

- Poll the SEN55 and SCD41 every **5 seconds**.
- Keep the **newest valid** measurements; do not queue older sensor readings for later reporting.
- Track the observed interval maxima for VOC Index, PM2.5 and PM10.
- Use NTP to align reporting to clock-minute boundaries (default: **60 seconds**).
- Refresh the OLED and upload the **same frozen snapshot** to ThingSpeak.
- Send **at most one HTTPS POST per channel per reporting cycle**, omitting unavailable values.
- Recover failed SEN55 and SCD41 devices independently, with recovery attempts at reporting boundaries.
- Continue local sensing if Wi-Fi or ThingSpeak is unavailable. Missing cloud uploads are not buffered for later replay.
- Use `thingspeak_ca.h` for TLS certificate verification; NTP also supplies the time needed for certificate validity checks. ThingSpeak assigns its own receipt timestamps.

### OLED layout

The 128 × 64 OLED shows two columns with consistent label-to-value offsets:

```text
CO2 785    VOC 112/280
TMP 33.1   P1  2.8
RH  65.4   P2  2.9/8.5
NOx 1      P4  2.9
           P10 2.9/9.1
WIFI OK
```

Values are illustrative. For VOC, PM2.5 and PM10, the format is **newest / observed interval maximum**. `NIL` indicates unavailable or stale measurements. `WIFI LOST` replaces `WIFI OK` when disconnected. The firmware may shorten displayed numbers to fit the screen.

### Arduino environment

- Arduino IDE: **2.3.11**
- ESP32 Arduino core used during testing: **3.3.12**
- Libraries: **Sensirion I2C SEN5x**, **Sensirion Core**, **SparkFun SCD4x Arduino Library**, and **SparkFun Qwiic OLED Arduino Library**.
- Serial Monitor: **115200 baud**.

## Configuration and credentials

The firmware uses three local files:

```text
EnvironmentSensorSEN55/
├── EnvironmentSensorSEN55.ino
├── config_settings.h          # PRIVATE: do not commit
└── thingspeak_ca.h            # Public CA certificate
```

The repository should publish **`config_settings.example.h`** with placeholders. Copy it to `config_settings.h` in your Arduino sketch directory and set your own Wi-Fi credentials and ThingSpeak Write API Keys.

The primary user-adjustable timing setting is:

```cpp
#define TELEMETRY_FREQUENCY_IN_SECONDS 60
```

The five-second sensor polling interval is defined internally in the `.ino`.

**Security:** Never commit the real `config_settings.h`. Ensure `.gitignore` includes `**/config_settings.h`. Rotate credentials that have been disclosed or committed. The public ThingSpeak channel IDs are not secrets; **Write API Keys are secrets**.

## ThingSpeak field mapping

### Channel 1 — Bedroom Air Quality (`3591940`)

| Field | Measurement | Units/source |
| --- | --- | --- |
| 1 | CO2 | ppm, SCD41 |
| 2 | Temperature | °C, SEN55 |
| 3 | Relative humidity | %, SEN55 |
| 4 | NOx Index | index, SEN55 |
| 5 | VOC Index | index, SEN55 |
| 6 | PM1 | µg/m³, SEN55 |
| 7 | PM2.5 | µg/m³, SEN55 |
| 8 | PM10 | µg/m³, SEN55 |

### Channel 2 — Bedroom Air Quality - 2 (`3591941`)

| Field | Measurement | Units/source |
| --- | --- | --- |
| 1 | PM4 | µg/m³, SEN55 |
| 2 | VOC Index maximum | index, observed during reporting interval |
| 3 | PM2.5 maximum | µg/m³, observed during reporting interval |
| 4 | PM10 maximum | µg/m³, observed during reporting interval |

Each channel stores its own timestamp and entry ID. The two requests are sequential and may have slightly different receipt times. Entry IDs are independent between channels and should not be treated as a shared join key.

## Dashboard

The static GitHub Pages dashboard is hosted from `dashboard/` using the GitHub Actions workflow at `.github/workflows/deploy-pages.yml`.

Planned/supported controls include selectable measurements, adjustable time ranges, interactive charts, and CSV export of the selected measurements. The dashboard fetches data from the two public ThingSpeak APIs; it does not store telemetry itself.

**CSV validation outstanding:** Check that exported timestamps are properly quoted or formatted without embedded commas, and that PM column names clearly indicate `ugm3`. Verify long-range pagination, joining across channels, and original one-minute resolution before relying on large exports.

## Current validation

- [x] I2C scanner detected OLED `0x3D`, SCD41 `0x62`, SEN55 `0x69` on SDA GPIO23 / SCL GPIO22.
- [x] OLED displays sensor readings.
- [x] Clock-aligned reporting observed in Serial Monitor.
- [x] ThingSpeak accepted uploads to both channels (positive entry IDs).
- [x] GitHub Pages deployment succeeded and displayed readings.
- [x] CSV download was exercised.
- [ ] Validate CSV format and channel merging thoroughly.
- [ ] Test permanent SEN55 and SCD41 failure/recovery scenarios.
- [ ] Test extended Wi-Fi outages and reconnect behaviour.
- [ ] Assess long-term reliability of sensor polling during HTTPS requests.
- [ ] Fabricate and fit the acrylic mounting plate.
- [ ] Verify LiPo polarity, charging compatibility, and install the backup battery.

## Notes and limitations

- SEN55 VOC/NOx indices are **indices**, not measurements of individual perfume, incense or fragrance chemicals. PM measurements are useful for haze and smoke trends but cannot identify a pollution source by themselves.
- A five-second polling interval can miss shorter peaks. Interval maxima reflect the largest **observed sensor values**, not guaranteed instantaneous physical peaks.
- A shared I2C electrical fault can affect both sensors, even though software recovery is logically independent.
- The working firmware is a prototype; recovery from a sensor that never supplies an initial valid reading, and strict non-blocking acquisition during HTTPS requests, still need improvement.
- Public air-quality data can reveal patterns of room occupancy or daily routine. Avoid publishing a precise home address.

## Repository

[pluralitas/EnvironmentSensorSen55](https://github.com/pluralitas/EnvironmentSensorSen55)

Contributions and updates can be tracked through GitHub commits and issues. Hardware CAD files, photos and test results can be added as the build progresses.
