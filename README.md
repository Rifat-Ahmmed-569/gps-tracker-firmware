# GPS Tracker

**ESP32 GPS tracking device** firmware that reads live GPS location, stores it safely on an SD card, and sends it to the internet over **Wi-Fi** or a **4G/LTE SIM module**, with a built-in **web dashboard**, **serial console**, and **zero-data-loss offline logging**.

**Firmware v6.2**

> Files in this project:
> - `BB-TRACKER.ino` — the main firmware (Arduino sketch)
> - `tracker_types.h` — shared data structures used by the firmware
> - `dashboard_html.h` — the built-in web dashboard (HTML/CSS/JS), served directly from the ESP32
> - `secrets.example.h` — credential template. **Copy to `secrets.h` before compiling.**

<img width="1280" height="576" alt="gps" src="https://github.com/user-attachments/assets/96d25e12-3be8-4d32-8c15-c12a6f6f0a7a" />

---

## 📌 1. Project Overview

### What does this project do?

BB-TRACKER turns an **ESP32 microcontroller** into a **standalone GPS tracking device**. It:

1. Reads GPS coordinates (latitude, longitude, speed, heading, altitude, satellites) from a GPS module.
2. Stamps every reading with a timestamp from a **ranked time authority** — network time first (NTP over Wi-Fi, or the carrier's clock over cellular), GPS as fallback, then a free-running holdover. Displayed in Bangladesh time (UTC+6).
3. Tries to send that data **live**, in real time, to an MQTT server over the internet (Wi-Fi first, then a 4G SIM as backup).
4. If there is **no internet connection at all**, it never throws the data away — it safely writes every GPS point to an SD card, and automatically uploads ("syncs") all the saved points the moment the internet comes back, in the exact order they were recorded.
5. Hosts its **own Wi-Fi hotspot** and a **web dashboard** (a website you open in your phone/laptop browser) so you can configure the device, watch live status, and download logs — even with no internet.
6. Uses three LEDs to show battery level, GPS quality, and network/transmission status at a glance, without needing a screen.

### The design invariant

> **Connectivity is optional. Acquisition and preservation are not.**

Every architectural decision in this firmware follows from assuming the link is down and the sky is blocked. That assumption is why `setup()` blocks on nothing network-related, why a record is held until the broker confirms it, and why the clock trusts the SIM before the satellites.

### Real-world use case

Designed for **vehicle or asset tracking** in areas with unreliable internet — for example:
- Tracking a delivery van, rickshaw, or bus across Bangladesh where Wi-Fi/cellular coverage comes and goes.
- Logging the exact route of a vehicle even through "dead zones," then automatically catching up and sending all the missed points once signal returns.
- A battery-powered tracker that a technician can configure just by connecting to its Wi-Fi hotspot with a phone — no laptop needed.

### Expected output / behavior

- Every few seconds (configurable), the device captures a GPS point and either **publishes it live** to an MQTT broker, or **saves it to the SD card** if there's no connection.
- A **Serial Monitor** (USB, 115200) shows a live table of GPS/battery/network status, plus a text command console (type `HELP`).
- A **web dashboard** at `http://192.168.4.1` shows live data, battery %, GPS fix quality, and sync status, and lets you change settings.
- Three onboard LEDs:

  | LED | Meaning | Behavior |
  |---|---|---|
  | 🔴 Red | Battery level | Brighter = lower battery |
  | 🟡 Yellow | Network / transmission | Blinking = offline, slow "breathing" ramp = uploading a backlog, quick flash = live data just sent |
  | 🟢 Green | GPS signal quality | Brighter = better GPS fix; blinks if GPS signal is stale/lost |

<img width="1280" height="576" alt="light" src="https://github.com/user-attachments/assets/9bd0980f-58e2-4f0a-850f-e2c72d48c672" />

---

## 🧰 2. Components Required

| # | Component | Example / Model | Purpose |
|---|---|---|---|
| 1 | Microcontroller | **ESP32 Dev Board** (ESP32-WROOM-32, 38-pin or 30-pin) | Main brain — runs the firmware, Wi-Fi, dashboard |
| 2 | GPS Module | **NEO-8M / NEO-6M GPS module** (u-blox, UART) | Provides latitude/longitude/speed/time |
| 3 | Cellular Modem (optional) | **SIM7600 / A7670C module** (with a Micro-SIM data card) | Sends data over mobile network when Wi-Fi is unavailable |
| 4 | SD Card Module | **MicroSD breakout (SPI)** + MicroSD card (FAT32, 4–32 GB) | Stores GPS records offline so no data is lost |
| 5 | Battery | **3.7 V Li-ion/Li-Po battery** (18650 or a flat Li-Po pack) | Powers the device |
| 6 | Charging module | **TP4056 Li-ion charger** (with CHRG/STDBY status pins) | Charges the battery safely and reports charge state |
| 7 | LEDs | 3× **5 mm LEDs** (Red, Yellow, Green) + 220–330 Ω resistors | Status indicators |
| 8 | Voltage divider resistors | 2× **100 kΩ resistors** | Safely measure battery voltage on the ADC pin |
| 9 | Pull-up resistors | 2× **10 kΩ resistors** | Required externally for the TP4056 CHRG/STDBY sense pins |
| 10 | Push button / jumper | Any momentary switch | "Force Setup Mode" pin (GPIO 0) |
| 11 | Breadboard & jumper wires | — | For prototyping the wiring |
| 12 | USB cable | Micro-USB or USB-C | Programming + power during setup |
| 13 | Voltage Booster | 3 V–5 V booster | TP4056 + booster to power the device |

> 💡 The SIM7600/A7670C modem is **optional**. Build a Wi-Fi-only tracker by setting `ENABLE_MODEM_FUNCTIONALITY 0` (see Section 5).

### A note on the battery pack

The firmware's discharge curve targets a **4.40 V-charge high-voltage cell** (Samsung EB-BG991ABY class, 3.86 V nominal) — **not** a standard 4.20 V cell.

If you fit a standard 4.20 V 18650, edit `BATT_CURVE` and `BATT_V_FULL` or the percentage will read low across the whole range. The curve is a plain lookup table and nothing else in the firmware depends on it.

---

## 💻 3. Software Requirements

### Installing the Arduino IDE

1. Go to **https://www.arduino.cc/en/software**
2. Download the version for your OS (Windows / macOS / Linux).
3. Run the installer with default options. On Windows, allow the driver prompts (needed for CP2102/CH340 USB chips).
4. Open the IDE once to confirm it launches.

### Installing USB Drivers (if needed)

Most ESP32 dev boards use a **CP2102** or **CH340** USB-to-Serial chip. If no COM port appears:
- **CP2102 driver:** https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers
- **CH340 driver:** search "CH340 driver download" (from the chip maker, WCH).

After installing, re-plug the ESP32 and check **Device Manager** (Windows) or `ls /dev/tty.*` (macOS/Linux).

### Installing ESP32 Board Support

1. **File → Preferences** (Windows/Linux) or **Arduino IDE → Settings** (macOS).
2. In **"Additional Boards Manager URLs"**, paste:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Click **OK**.
4. **Tools → Board → Boards Manager**, search **"esp32"**, install **"esp32 by Espressif Systems"**.

Both core **2.x** and **3.x** are supported — the LEDC and task-watchdog APIs are switched on `ESP_ARDUINO_VERSION_MAJOR` at compile time.

---

## 📚 4. Library Setup

| Library | Purpose | Author |
|---|---|---|
| **TinyGPS++** | Parses NMEA sentences from the GPS module | Mikal Hart |
| **TinyGSM** | Communicates with the SIM7600/A7670C modem | Volodymyr Shymanskyy |
| **PubSubClient** | MQTT client used to publish/subscribe GPS data | Nick O'Leary |
| `WiFi.h`, `WebServer.h`, `DNSServer.h`, `Preferences.h`, `SD.h`, `SPI.h`, `FS.h`, `esp_task_wdt.h`, FreeRTOS headers | Built into the ESP32 core — no separate install | Espressif |

### Method 1 — Arduino Library Manager (recommended)

**Sketch → Include Library → Manage Libraries…** (`Ctrl+Shift+I`), then install one at a time:

- **"TinyGPS++"** by **Mikal Hart**
- **"TinyGSM"** by **Volodymyr Shymanskyy**
- **"PubSubClient"** by **Nick O'Leary**

### Method 2 — Manual ZIP

1. Download from GitHub:
   - TinyGPS++: https://github.com/mikalhart/TinyGPSPlus
   - TinyGSM: https://github.com/vshymanskyy/TinyGSM
   - PubSubClient: https://github.com/knolleary/pubsubclient
2. **Sketch → Include Library → Add .ZIP Library…**
3. Select the `.zip` — do **not** unzip it yourself.
4. Restart the IDE.

### ℹ️ You do NOT need to edit `PubSubClient.h`

Older guides tell you to hand-edit `MQTT_MAX_PACKET_SIZE`. **This firmware does not require that.**

`mqttEnsureBuffer()` calls `PubSubClient::setBufferSize()` at runtime and grows the buffer to whatever the batch needs, up to `PAYLOAD_CAP` (15360 bytes). The buffer is grow-only on purpose: PubSubClient shares one buffer between TX and RX, so shrinking it while an echo is in flight silently drops the echo and turns every ACK into a timeout.

If you have already edited the header, no harm done — the runtime call overrides it either way. But note that the commonly-quoted value of `2048` is far below what a 30-record batch needs.

If you see `Buffer grow to N bytes FAILED (heap M)` in the serial log, the ESP32 is out of heap, not misconfigured.

---

## ⚙️ 5. Arduino IDE Configuration

### Board and Port

1. Connect the ESP32 via USB.
2. **Tools → Board → esp32 → "ESP32 Dev Module"**.
3. Set:

   | Setting | Value |
   |---|---|
   | Board | ESP32 Dev Module |
   | Upload Speed | 921600 (drop to 115200 if uploads fail) |
   | Flash Frequency | 80 MHz |
   | Flash Mode | QIO |
   | Flash Size | 4 MB |
   | Partition Scheme | Default 4 MB with spiffs |
   | PSRAM | Disabled |
   | Core Debug Level | None |

4. **Tools → Port** — select the port that appeared when you plugged in the board.

### Sketch Folder Setup

Arduino requires all files to sit **together in one folder named exactly like the `.ino` file**:

```
BB-TRACKER/
├── BB-TRACKER.ino
├── tracker_types.h
├── dashboard_html.h
├── secrets.example.h
└── secrets.h          ← you create this (step below)
```

### 🔑 Create `secrets.h` — the sketch will not compile without it

Copy `secrets.example.h` to `secrets.h` in the same folder and fill in your values:

```cpp
#define DEFAULT_WIFI_SSID   "your-wifi-ssid"
#define DEFAULT_WIFI_PASS   "your-wifi-password"
#define DEFAULT_MQTT_TOPIC  "tracker/gps/your-unique-topic"
#define DEFAULT_DEVICE_ID   "00000"
#define DEFAULT_AP_PASS     "changeme8"      // WPA2 needs 8+ characters
#define DEFAULT_DEV_PIN     "1010"
#define DEFAULT_USER_PIN    "0000"
```

If it is missing you will get:

```
#error "secrets.h not found. Run: cp secrets.example.h secrets.h  then edit it."
```

`secrets.h` is gitignored and must never be committed. These are **first-boot defaults only** — a device that has already been provisioned reads its configuration from NVS and ignores them. They matter on a virgin board, after a factory reset, and after a `CONFIG_SCHEMA_VERSION` bump.

> ⚠️ **The MQTT topic is a credential.** The default broker `broker.hivemq.com` is public and unauthenticated. Anyone who knows your topic string can subscribe to your live position feed. Pick something non-obvious.

### Optional Build Flags (top of `BB-TRACKER.ino`)

```cpp
#define ENABLE_MODEM_FUNCTIONALITY 1    // 0 = Wi-Fi-only build (no SIM7600/A7670C needed)
#define ENABLE_CHARGE_SENSE        1    // 0 if you didn't wire the TP4056 CHRG/STDBY pins
#define KEEP_SENT_ARCHIVE          1    // 0 = delete files from SD once sent (saves space)
#define DEBUG_MODE                 1    // 0 = silence DBG chatter; the telemetry table stays
#define TZ_OFFSET_SECONDS  (6L * 3600L) // Timezone offset (default: Bangladesh, UTC+6)
#define BATTERY_CAL_REFERENCE_VOLTAGE 3.720f  // your multimeter reading — see Section 5.1
#define BATTERY_DEBUG              0    // 1 = print the full measurement chain at boot
```

### 5.1 Battery calibration — change one value, upload once

1. Connect the battery and let it **rest** — no charging, modem idle.
2. Measure the battery **terminals** with a multimeter.
3. Put that reading in `BATTERY_CAL_REFERENCE_VOLTAGE`.
4. Upload **once**.

On first boot the firmware measures its own uncalibrated voltage, solves the correction, and writes it to a dedicated NVS namespace (`battcal`). Every later boot loads it. You never enter it again.

**Leave the value in place afterwards.** It is compared against the reference stored in NVS, so an unchanged value means "already calibrated, do nothing" — the device will *not* re-solve itself against whatever charge state the pack happens to be in.

The correction model is `V = V_raw × gain + offset`. Gain absorbs everything that scales with the reading (resistor tolerance, ADC reference error); offset absorbs a fixed drop (protection FET, series diode). Offset defaults to `0`, so a gain-only calibration behaves exactly as before.

Calibration survives a config wipe and a schema bump. **Only an explicit factory reset erases it** — it describes the hardware fitted to this board, not your settings.

---

## 🔌 6. Circuit Diagram / Connections

> ⚠️ **Power safety first:** connect all grounds together. Check each module's voltage rating. Never connect a Li-ion battery backwards, and never connect raw battery voltage to a GPIO.

### GPS Module (NEO-8M/NEO-6M) → ESP32

| GPS Module Pin | ESP32 Pin | Notes |
|---|---|---|
| VCC | 3.3 V or 5 V (check your module) | Power |
| GND | GND | Common ground |
| **TX** | **GPIO 16** (`GPS_RX`) | GPS's TX → ESP32's RX |
| **RX** | **GPIO 17** (`GPS_TX`) | GPS's RX → ESP32's TX |

*(9600 baud — set automatically by the firmware.)*

<img width="1600" height="1200" alt="9f55a997-79d3-4c66-83a4-b2b01d4da531" src="https://github.com/user-attachments/assets/8ae3082a-202b-4b65-86a2-0029bce7970c" />

### Cellular Modem (SIM7600/A7670C) → ESP32 — optional

| Modem Pin | ESP32 Pin | Notes |
|---|---|---|
| VCC | Dedicated 4 V–5 V supply — **do NOT power from the ESP32 3.3 V pin** | Power |
| GND | GND (shared with ESP32) | Common ground |
| **TX** | **GPIO 27** (`MODEM_RX`) | Modem's TX → ESP32's RX |
| **RX** | **GPIO 26** (`MODEM_TX`) | Modem's RX → ESP32's TX |
| PWRKEY | GPIO 4 (`PWRKEY`) | Firmware pulses this to power the modem on |

> 📌 **These two lines are easy to get backwards.** The firmware defines `MODEM_RX 27` and `MODEM_TX 26`, and opens the port as `modemSerial.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX)`. The ESP32 therefore *listens* on GPIO 27 — so the **modem's TX goes to GPIO 27**, not 26. Swapping them produces a modem that powers on and answers nothing; run `SIM` on the serial console to confirm before assuming the modem is faulty.

<img width="1600" height="1200" alt="dc39632f-d017-429a-b886-d79a28427277" src="https://github.com/user-attachments/assets/4687b363-c446-44f2-84fd-dc71dc1e18d6" />

### MicroSD Card Module (SPI) → ESP32

| SD Module Pin | ESP32 Pin | Notes |
|---|---|---|
| MISO | GPIO 19 | SPI data in |
| MOSI | GPIO 23 | SPI data out |
| SCK | GPIO 18 | SPI clock |
| CS | GPIO 5 | Chip select |
| VCC | 3.3 V (or 5 V if the module has its own regulator) | Power |
| GND | GND | Common ground |

<img width="1600" height="1200" alt="f45c9082-9c53-4d86-92df-d54dfb8a917a" src="https://github.com/user-attachments/assets/f5d9df9a-c165-4909-9156-059dc23dc361" />

### LEDs → ESP32

| LED | ESP32 Pin | Wiring |
|---|---|---|
| 🔴 Red (battery) | GPIO 25 | Anode → 220–330 Ω resistor → GPIO 25; cathode → GND |
| 🟡 Yellow (network) | GPIO 33 | Same pattern |
| 🟢 Green (GPS) | GPIO 32 | Same pattern |

All three are PWM-driven (5 kHz, 12-bit) so they fade smoothly rather than just blinking.

<img width="1600" height="1200" alt="7185955f-3675-4b79-b814-a51cf3536c1e" src="https://github.com/user-attachments/assets/ab4a63d4-574a-4c0e-87cf-6055c0dec89d" />

### Battery Voltage Sensing

| Signal | ESP32 Pin | Wiring |
|---|---|---|
| Battery voltage sense | GPIO 34 (`BATT_ADC_PIN`) | Two **100 kΩ resistors in series** across the battery (+ to GND); the midpoint goes to GPIO 34. Halves the voltage so it's safe for the ADC. |

> 🔧 **Known hardware limitation.** 100 kΩ ∥ 100 kΩ presents ~50 kΩ source impedance to the ESP32's SAR ADC, which needs **under 10 kΩ** to fully charge its sample-and-hold. The firmware mitigates this with two discarded dummy reads plus a 200 µs settle before every burst — that is mitigation, not a fix. If you respin the board, add a **100 nF capacitor from the divider midpoint to ground**. It costs nothing in current and fixes the problem properly.

### TP4056 Charge Status Sensing (optional)

| TP4056 Pin | ESP32 Pin | Wiring |
|---|---|---|
| CHRG | GPIO 35 (`CHRG_SENSE_PIN`) | **External 10 kΩ pull-up to 3.3 V required.** Active LOW while charging. |
| STDBY | GPIO 39 (`FULL_SENSE_PIN`) | **External 10 kΩ pull-up required.** Active LOW when full. |

> GPIO 34/35/39 are input-only and have **no internal pull-ups**. Without external resistors both pins float and the readings are meaningless. If you're not wiring these, set `ENABLE_CHARGE_SENSE 0`.

### Force Setup Mode Button

| Function | ESP32 Pin | Wiring |
|---|---|---|
| Force Setup Mode | GPIO 0 (`FORCE_SETUP_PIN`) | Internal pull-up — connect a button between GPIO 0 and GND. Hold LOW at boot. (Usually the **BOOT** button already on your dev board.) |

### Power Notes & Precautions

- Share a **common ground** across every module.
- The cellular modem draws bursts of up to **2 A** during attach and transmit. Power it from the battery/TP4056 output directly, never through the ESP32's onboard regulator.
- Add **1000 µF** of bulk capacitance close to the modem's VBAT pins if you see random resets during modem activity.
- Check the modem's supply with a scope, not a multimeter — brownout events are milliseconds long. A sag deep enough to reset the ESP32 looks exactly like a firmware crash in the logs.

---

## 🧠 7. Code Explanation

The firmware (~5,100 lines) is organized into clear modules.

### 7.1 Configuration Flags

`#define` switches at the top let you turn features on or off **before compiling** — see Section 5.

### 7.2 Pin Definitions

All hardware pins are named constants near the top (`GPS_RX`, `SD_CS`, `LED_RED_PIN`, …), matching Section 6. Rewiring means changing only these lines.

### 7.3 `ConfigManager`

Handles **user-configurable settings** — Wi-Fi credentials, MQTT broker, device ID, GPS interval, PINs. It loads from the ESP32's non-volatile storage (**NVS**) at boot so settings survive power loss, saves back when you change them from the dashboard, and provides `factoryReset()`.

NVS is deliberately split into four namespaces so a wipe of one cannot take another:

| Namespace | Holds | Erased by |
|---|---|---|
| `tracker` | 17 config keys | Config wipe, factory reset |
| `fifo` | Sync progress, sequence counter | Factory reset |
| `sysprov` | Schema version, build, reset count | The provisioner only |
| `battcal` | Per-board battery calibration | **Factory reset only** |

### 7.4 `BatteryMonitor`

Owned by `taskSensor` alone, at a fixed 250 ms cadence. **Nothing else touches the ADC.**

```
2 dummy reads (charge S/H) → 21 samples → median → MAD outlier rejection
  → mean of survivors → divider + calibration → sanity gate [1.0, 5.0] V
  → slew limit (30 mV/sample) → EMA (τ ≈ 3 s) → curve → percent hysteresis
```

This single-owner rule is a correctness requirement, not an optimisation. In v5.2 five call sites each triggered 64 blocking ADC reads at unpredictable times, so the moving average was fed at a random rate and the displayed value depended on who asked last.

### 7.5 `LEDManager`

A pure state machine with zero `delay()`, driving all three LEDs by PWM.

### 7.6 `TimeService` — the single time authority

> ⚠️ This replaced the old `GPSManager` timestamping in v6.1. If you are reading a pre-v6.1 guide, this section is what changed.

Authority ranking, as of v6.2:

```
NETWORK (NTP / CELL)  >  GPS  >  HOLDOVER  >  UNSYNCED
```

| Source | Set by |
|---|---|
| `TSRC_NTP` | SNTP over Wi-Fi — primary |
| `TSRC_CELL` | Carrier NITZ via `AT+CCLK?` — primary |
| `TSRC_GPS` | Satellite time — fallback |
| `TSRC_HOLD` | Last valid reference, free-running on the crystal |
| `TSRC_NONE` | Unsynced — nothing has ever set the clock |

**Why network beats GPS.** GPS is ground truth *when there is a fix*. This device often has none — bag, pocket, basement car park, corrugated roof — while the SIM and Wi-Fi stay up throughout. Ranking the sometimes-absent source above the almost-always-present one made the clock hostage to the sky.

**Two failures this fixed:**
- The old code emitted a hardcoded `1970-01-01 00:00:00` for every record from boot until first fix.
- `TinyGPSDate`/`TinyGPSTime::valid` latch true on first commit and are never cleared, so after losing a fix the old code re-emitted the last parsed instant forever — plausible-looking and undetectable downstream. `TimeService` checks `age()`, refuses stale GPS, and projects forward from the last good reference.

**Unsynced is explicit.** Records with no valid time carry `ts` = `"0000-00-00 00:00:00"` — an impossible instant, chosen so no consumer can silently plot it the way `1970-01-01` was plotted.

**Cost:** four SNTP exchanges a day over Wi-Fi, and **nothing** over cellular — `AT+CCLK?` is a local query against the value the modem already holds.

The stored instant is UTC. `TZ_OFFSET_SECONDS` is applied **only** in `format()`.

### 7.7 `E2EVerifier` — end-to-end delivery verification

The "did my message *actually* arrive?" checker. The device subscribes to its own publish topic; when the broker echoes the message back it compares an FNV-1a checksum, confirming the full round trip **device → transport → broker → device**. If no echo arrives in time the record is re-queued rather than lost.

Set `ackMode = 1` to require an explicit backend confirmation on `<topic>/ack` instead of a broker echo. This is stronger — a broker echo proves the transport, not that anything ingested the data — but the backend has to implement it.

### 7.8 `SDStore` — the durable FIFO

Sole owner of the SD card. Directory layout:

```
/LOGS
/LOGS/QUEUE    pending batches
/LOGS/SENT     acknowledged batches (kept if KEEP_SENT_ARCHIVE = 1)
```

One file is one MQTT batch, capped at `batchSize` records (default and maximum 30). Progress is tracked **persistently in NVS**, so a reboot or reconnect never re-sends delivered records or skips records.

Four rules exist because of four specific data-loss bugs in v5.2:

1. **Every path is canonicalised.** `File::name()` already returns a full path on core 2.x, so naive concatenation produced `/logs//logs/...`, which `SD.open()` rejects — and the sync engine then deleted files it had never transmitted.
2. **A zero read is an error, not a completion.** A file is never deleted on a read that returned zero records.
3. **The tail file is sealed before it can be transmitted**, so sync cannot delete the file logging is still appending to.
4. **Record count comes from the byte budget, computed before serialising.** Truncation is structurally impossible rather than merely unlikely.

### 7.9 Sync Engine

Uploads the backlog the moment a connection returns, oldest first, batch by batch, verifying each batch through `E2EVerifier` before advancing. Sync progress is written **only after a verified batch**, so a failure cannot lose records and a retry cannot duplicate them.

### 7.10 Networking (Wi-Fi + MQTT + Cellular Fallback)

Tries **Wi-Fi** first; falls back to the **modem** on a separate background task so it never freezes GPS logging or the dashboard. Once online it connects to the broker and publishes live records to your topic, and self-test probes to `<topic>/diag` so the live stream stays schema-clean.

**Live payload:**

```json
{
  "username": "53384",
  "modelList": [{
    "username": "53384",
    "appCode": "DEMO",
    "latitude": 23.8103000,
    "longitude": 90.4125000,
    "platform": "IoMT",
    "visitDate": "2026-08-23 15:30:05",
    "visitTime": "2026-08-23 15:30:05",
    "networkType": "WIFI",
    "broadcastEnabled": true,
    "locationAccuracy": 1.20,
    "altitude_msl": 12.50,
    "speed_kmph": 0.00,
    "heading_deg": 0.00,
    "satellite_count": 9,
    "internet_available": true,
    "batteryPower": 87,
    "status": "live",
    "time_valid": true,
    "time_src": "NTP"
  }]
}
```

**Bulk / offline payload** — on reconnect, one whole SD file becomes one MQTT message with many records in `modelList`:

```json
{
  "username": "53384",
  "modelList": [
    {
      "username": "53384", "appCode": "DEMO",
      "latitude": 23.8207242, "longitude": 90.4222510,
      "platform": "IoMT",
      "visitDate": "2026-08-23 08:25:49", "visitTime": "2026-08-23 08:25:49",
      "networkType": "NONE", "broadcastEnabled": true,
      "locationAccuracy": 1.15, "altitude_msl": 7.40,
      "speed_kmph": 0.26, "heading_deg": 0.00, "satellite_count": 5,
      "internet_available": false, "batteryPower": 100,
      "status": "offline",
      "time_valid": true, "time_src": "CELL"
    }
    /* ... up to batchSize records (default 30) ... */
  ]
}
```

Differences between the two:

| Field | Live payload | Bulk/offline payload |
|---|---|---|
| `modelList` length | Always **1** | Up to `batchSize` (default 30) |
| `status` | `"live"` | `"offline"` |
| `internet_available` | `true` | `false` |
| `networkType` | Current transport (`WIFI` / `SIM`) | The transport **at capture** — usually `NONE`, but `WIFI`/`SIM` for a live record that was re-queued after a missing echo |
| Topic | Your configured topic | **The same topic** in this build |
| When sent | Every GPS interval, while online | Only after reconnecting, drained in order |

> 📌 **On the bulk topic.** `ConfigManager::bulkTopic()` returns `cfg.topic` unconditionally in this build — live and playback share one topic. The older `<topic>/playback` split is preserved as a commented expression on that line and `pbOnLiveTopic` still exists in config. Restore it by returning `cfg.pbOnLiveTopic ? cfg.topic : (cfg.topic + "/playback")`.

**Size budget:** `REC_JSON_MAX` 496 bytes per record, `PAYLOAD_CAP` 15360 total, `batchSize` clamped to 30. If you add a field, recompute `REC_JSON_MAX` first — v6.1's two new fields pushed a record from 427 to 468 bytes, over the old 448-byte cap, and would have silently re-armed the truncation bug.

**SD record format** (one JSON object per line, `.jsonl`):

```json
{"seq":10427,"ts":"2026-08-23 14:07:31","lat":23.7808751,"lng":90.2792371,"alt":12.40,"spd":0.00,"hdg":0.00,"sats":9,"hdop":0.98,"bat":87,"net":"WIFI","fix":1,"tsrc":2}
```

Two things must never change: `net` round-trips as the **strings** `"WIFI"`/`"SIM"`, and `tsrc` round-trips as an **integer** (`TimeSrc`). Renumbering `TimeSrc` re-labels every record already sitting on a card — append new sources at the end, never reorder, and keep `TSRC_NONE` at `0`.

### 7.11 Always-On Web Dashboard

The ESP32 always runs its own hotspot named **`BB-TRACKER-<deviceId>`** in addition to joining your Wi-Fi, so `http://192.168.4.1` is **always reachable** even with zero internet. From the dashboard you can view live position, battery, network and sync status; change settings; browse, download (CSV/JSON) or delete logs; and trigger a manual sync, restart, or factory reset.

Access is gated by two 4-digit PINs — a **user** PIN (view + basic settings) and a **developer** PIN (full access, including SD formatting and factory reset). Both defaults live in `secrets.h`.

`/api/playback_view` renders a stored batch **byte-for-byte as the broker will receive it**. When a backend rejects a batch, compare against this first — it removes serialisation from the suspect list in one step.

> 🔐 **Change the default PINs and hotspot password in `secrets.h` before deploying in the field.**

### 7.12 FreeRTOS Multitasking Architecture

Four tasks across two cores. The scheduling matters less than the ownership: **no shared resource has two writers.**

| Task | Core | Priority | Owns |
|---|---|---|---|
| `taskSensor` | 1 | 3 (high) | GPS UART + parsing, battery ADC, LEDs, record creation |
| `taskStore` | 1 | 2 | The SD FIFO — sole owner, no exceptions |
| `taskNet` | 0 | 2 | Wi-Fi, modem, MQTT, live publish, sync engine |
| `loopTask` | 1 | 1 (low) | Dashboard, captive DNS, serial console, watchdog |

All modem work lives on `taskNet` specifically because it can block for a long time — init 10 s, `waitForNetwork` 30 s, `gprsConnect` up to 60 s. On `loop()` that combination tripped the watchdog.

A **task watchdog** (60 s) reboots the device if a critical task freezes. Nothing in `setup()` blocks on the network — the device acquires, logs, and serves its dashboard before any link exists.

Serial is shared by four tasks, so every print goes through `sPrintf()`, which takes a mutex. A telemetry row can never be spliced into the middle of a console reply.

### 7.13 Serial Console Commands

| Command | What it does |
|---|---|
| `STAT` | Full live status (GPS, battery, network, sync) |
| `NET` | Connectivity manager state |
| `SD` | SD card statistics |
| `QUEUE` | FIFO state — files, tail, sync offset |
| `LS` | List all batch files (pending + sent) |
| `CAT <n\|path>` | Dump a batch file raw as JSONL |
| `PLAY <n\|path>` | Pretty-print a batch file as a table |
| `DEL <n\|path>` | Delete a batch file |
| `SYNC` | Force a sync attempt now |
| `SIM` | Staged cellular bring-up test — **does not touch Wi-Fi** |
| `TIME` | Clock source, age of the reference, next sync attempt |
| `TIME SYNC` | Force a network time sync now |
| `NETSIM OFF\|ON\|AUTO` | Simulate an outage without touching the radio |
| `TRACE ON\|OFF` | Per-record raw JSON trace |
| `VCAL` | Battery diagnostics — the full measurement chain |
| `VCAL <volts>` | One-point calibration against a multimeter reading |
| `VCAL P1 <v>` / `VCAL P2 <v>` | Two-point calibration (points must be ≥ 0.40 V apart) |
| `VCAL RESET` | Erase calibration → gain 1.0, offset 0 |
| `VCAL DIAG` | Full diagnostics including rejected-sample count |
| `TEST` | Re-run the self-test |

**Reading `VCAL` output.** Two comparison lines do the whole job:

- **ADC pin** vs a DMM at GPIO 34 → if they disagree, the **ADC** is wrong.
- **Uncalibrated** vs the battery terminals → if they disagree, the **divider** is wrong.

Don't reach for gain until you know which one is off. Gain papers over either, and the papered-over one resurfaces as a nonlinear error at a different charge state.

---

## 📤 8. Uploading Code

1. Make sure `BB-TRACKER.ino`, `tracker_types.h`, `dashboard_html.h`, and **`secrets.h`** are all in the same folder, named exactly like the `.ino` file.
2. Open `BB-TRACKER.ino` — the other files appear as tabs.
3. Connect the ESP32 via USB.
4. Select the correct **Board** and **Port** (Section 5).
5. Click **✔ Verify** first to catch errors before uploading.
6. Click **➡ Upload**.
7. If it hangs at `Connecting...`, **hold the BOOT button** while upload starts, release once "Writing at..." appears.
8. `Hard resetting via RTS pin...` means it's done.

<img width="942" height="385" alt="image" src="https://github.com/user-attachments/assets/d779ec5b-5039-467e-aa2d-b34f429f2ac4" />

### Common Upload Errors & Fixes

| Error | Fix |
|---|---|
| `secrets.h not found` | Copy `secrets.example.h` to `secrets.h` in the sketch folder and fill it in (Section 5) |
| `Failed to connect to ESP32: Timed out waiting for packet header` | Hold **BOOT** during upload; try a shorter USB cable; drop Upload Speed to 115200 |
| Port not listed in Tools → Port | Install the CP2102/CH340 driver; try a different cable or port |
| `fatal error: dashboard_html.h: No such file or directory` | All files must be in one folder whose name matches the `.ino` exactly |
| `A fatal error occurred: MD5 of file does not match` | Retry; use a better USB cable |
| Board resets in a loop after upload | Power — modem current spikes brown out weak USB ports. Use battery or a proper supply |
| Sketch too large | Set Partition Scheme to "Default 4 MB with spiffs"; disable PSRAM |

---

## ✅ 9. Output / Testing

### First Boot

Open **Tools → Serial Monitor** at **115200** and reset the board:

```
==========================================
BOTS BANGLA GPS TRACKER v6.2 (PRODUCTION)
==========================================
Serial console ready — type HELP
---------- SELF-TEST ----------
GPS UART   : OK (1234 NMEA chars)
GPS fix    : not yet (0 sats)
Clock      : UNSYNCED — awaiting network sync (or a GPS fix as fallback);
             records are marked time_valid:false until then
SD card    : OK (0 pending batch file(s), 2% used)
Battery    : 3.850V (68%) cal 1.0547 — OK
Charge     : charging=no full=no (CHRG GPIO35 / STDBY GPIO39, external 10k pull-ups REQUIRED)
E2E echo   : skipped (no MQTT yet) — re-runs on first connect
---------- SELF-TEST DONE ----------
[BOOT] SYSTEM READY — acquisition and preservation run in every state
TIME      LATITUDE      LONGITUDE      SATS  HDOP   SPEED   BAT   NETWORK   GPS   SD   SYNC       QUEUE
```

Within 30–90 seconds outdoors the GPS should acquire a fix — `GPS fix: YES` and the 🟢 green LED brightens instead of blinking.

`Clock: UNSYNCED` at boot is **normal**, not a fault. Run `TIME` to watch it resolve; it usually goes to `NTP` or `CELL` well before the GPS fix arrives.

The **E2E echo** leg needs a live MQTT session, so it doesn't fail on a device that hasn't connected yet — it re-arms and runs automatically on first connect.

### Checking the Web Dashboard

1. Connect your phone/laptop to the hotspot **`BB-TRACKER-<deviceId>`** (password = your `DEFAULT_AP_PASS`).
2. Open **http://192.168.4.1**.
3. Enter your user or developer PIN.
4. Live battery %, coordinates, satellites, and connection status update every few seconds.

### Verifying Data Reaches the Broker

1. Set your Wi-Fi credentials and broker in the dashboard.
2. Check `NET` in the serial console for `MQTT connected`.
3. Subscribe to your topic with **MQTT Explorer** or the HiveMQ WebSocket client (http://www.hivemq.com/demos/websocket-client/).
4. A new JSON message should arrive every `intervalMs` (default 5 s).

### Testing the Offline Path — the important one

This is the feature the whole architecture exists for, so test it deliberately rather than by walking into a lift:

```
NETSIM OFF     → yellow LED goes to 1 Hz square blink; records accumulate on SD
QUEUE          → watch the pending file count grow
NETSIM AUTO    → yellow goes to 1.2 s triangle ramp while the backlog drains
QUEUE          → count returns to zero, in order, with no gaps
```

`NETSIM` fakes the outage **without touching the radio**, so it reproduces identically every time. Pulling an antenna introduces a real radio state you can't reproduce twice.

### LED Quick Reference

| Observation | Meaning |
|---|---|
| 🟢 Green blinking | No recent GPS fix — check antenna placement / go outdoors |
| 🟢 Green steady glow | GPS locked; brightness = signal quality |
| 🟡 Yellow blinking ~1 Hz | Offline — no Wi-Fi/SIM connection |
| 🟡 Yellow slow triangle pulse | Uploading a backlog |
| 🟡 Yellow brief flash every ~10 s | Online and sending live data |
| 🔴 Red bright | Battery low — recharge soon |
| 🔴 Red dim | Battery healthy |

---

## 🛠️ 10. Troubleshooting

| Problem | Likely Cause | Solution |
|---|---|---|
| `#error secrets.h not found` | Template not copied | `cp secrets.example.h secrets.h` in the sketch folder, then edit |
| GPS never gets a fix | Antenna indoors, or TX/RX swapped | Go outdoors with clear sky; verify GPS TX → GPIO 16; allow 2 min for a cold start |
| Modem powers on but never responds | **TX/RX swapped** — the most common cause | Modem TX → **GPIO 27**, modem RX → **GPIO 26**. Run `SIM` for a staged bring-up report |
| Cellular never connects | Wrong APN, no data plan, weak signal, or insufficient power | Check the SIM has data; verify APN in Settings; give the modem its own supply |
| SD card shows `FAIL` | Not FAT32, bad wiring, or unsupported card | Reformat as FAT32; re-check MISO/MOSI/SCK/CS; try a card ≤ 32 GB |
| Battery reading jumps randomly | Divider wrong, or uncalibrated | Run `VCAL` and compare both `<-` lines (Section 7.13). A climbing rejected-sample count means hardware, not tuning |
| Battery % reads low across the whole range | You fitted a 4.20 V cell but the curve targets 4.40 V | Edit `BATT_CURVE` and `BATT_V_FULL` |
| Battery % dips during modem transmit | Voltage sag under a 2 A burst — not a measurement fault | Add bulk capacitance; expect some sag on a used cell |
| Charging status random | `ENABLE_CHARGE_SENSE` on but pull-ups missing | Add 10 kΩ pull-ups on GPIO 35/39, or set the flag to `0` |
| Timestamps show `0000-00-00 00:00:00` | Clock unsynced — no network and no GPS fix yet | Expected at cold boot. Run `TIME`; force with `TIME SYNC` |
| Won't connect to Wi-Fi | 5 GHz network — ESP32 is 2.4 GHz only | Use a 2.4 GHz network; re-enter credentials in the dashboard |
| Records on SD but never uploaded | No connection, or broker/topic misconfigured | Check `NET` and `QUEUE`; verify broker settings; force with `SYNC` |
| Bulk batches fail while live sends work | Buffer failed to grow | Look for `Buffer grow to N bytes FAILED` — that's low heap, not config |
| Can't reach 192.168.4.1 | Joined the wrong network | Connect to `BB-TRACKER-<deviceId>`, not your home Wi-Fi |
| Forgot dashboard PIN | — | Hold BOOT (GPIO 0) at boot for setup mode, or reflash with a known PIN in `secrets.h` after a factory reset |
| Random reboots | Watchdog timeout, usually a brownout during modem transmit | Add capacitance near the modem; check `resets` in the `sysprov` namespace before blaming firmware |
| Dashboard loads but shows no data | Browser JS blocked, or `dashboard_html.h` mismatched | Try another browser; make sure all files came from the same firmware version |

### General Debugging Tips

- Start from the **Serial Monitor**. `STAT`, `NET`, `SD`, `QUEUE`, and `TIME` show the true internal state.
- `NETSIM OFF` tests the whole offline → sync → live path deterministically.
- `TRACE ON` prints every record as raw JSON in real time.
- `TEST` gives a full self-test of GPS, clock, SD, battery, and the publish path in one go.
- `/api/playback_view` shows a stored batch exactly as the broker will receive it — check this before suspecting the transport.

---

## 🚀 11. Future Improvements

- **TLS/SSL for MQTT** — currently plain unencrypted MQTT on a public broker.
- **OTA firmware updates** so field devices can be updated without USB.
- **Map view in the dashboard** — a Leaflet.js embed to visualise the route.
- **Geofencing alerts** when the tracker leaves a defined area.
- **Automatic APN detection** across carriers.
- **Deep-sleep low-power mode** for battery-only deployments needing updates every few minutes.
- **Measured discharge curve** — the current `BATT_CURVE` is an estimate anchored on published nominal/charge voltages, not measured from your pack. Discharge at a representative load, log resting voltage against coulomb count, and replace the table.
- **A 100 nF cap on the divider midpoint** on the next board revision, to fix the ADC source-impedance issue properly.

> Backend ACK mode is **already implemented** — set `ackMode = 1` and have your backend publish to `<topic>/ack`.

---

## 🚀 Meet the Team

<p align="center">
  <b>The people behind Bots Bangla</b>
</p>

<table align="center">
<tr>

<td align="center" width="25%">
  <img src="YOUR_IMAGE_LINK_HERE" width="180px" style="border-radius:50%;" alt="Ankit Mahmud"/>
  <br><br>
  <b>Ankit Mahmud</b>
  <br>
  🎨 3D Designer
  <br>
  SolidWorks Specialist
  <br><br>
  <a href="YOUR_PORTFOLIO_LINK">
    🌐 Portfolio
  </a>
</td>

<td align="center" width="25%">
  <img src="YOUR_IMAGE_LINK_HERE" width="180px" style="border-radius:50%;" alt="Tahsan Masum Fahim"/>
  <br><br>
  <b>Tahsan Masum Fahim</b>
  <br>
  🏗️ System Architect
  <br>
  Robotics & Automation
  <br><br>
  <a href="YOUR_PORTFOLIO_LINK">
    🌐 Portfolio
  </a>
</td>

<td align="center" width="25%">
  <img src="YOUR_IMAGE_LINK_HERE" width="180px" style="border-radius:50%;" alt="Tanvir Ahmmed"/>
  <br><br>
  <b>Tanvir Ahmmed</b>
  <br>
  ⚡ Electronics Hardware Developer
  <br>
  PCB & Embedded Systems
  <br><br>
  <a href="YOUR_PORTFOLIO_LINK">
    🌐 Portfolio
  </a>
</td>

<td align="center" width="25%">
  <img src="YOUR_IMAGE_LINK_HERE" width="180px" style="border-radius:50%;" alt="Rifat Ahmmed"/>
  <br><br>
  <b>Rifat Ahmmed</b>
  <br>
  💻 Firmware Developer
  <br>
  Robotics & IoT Engineer
  <br><br>
  <a href="YOUR_PORTFOLIO_LINK">
    🌐 Portfolio
  </a>
</td>

</tr>
</table>

---

<p align="center">
  <i>Building the future through Robotics, IoT, Automation, and Embedded Systems.</i>
</p>

---
## 📄 License & Credits

Firmware and documentation by [**Bots Bangla**](https://botsbangla.vercel.app/).

Licensed under the MIT License — see [`LICENSE`](LICENSE).
