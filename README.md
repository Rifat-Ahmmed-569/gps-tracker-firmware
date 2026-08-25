# BOTS BANGLA — GPS Tracker Firmware

Production ESP32 asset tracker. GPS acquisition, durable on-device buffering, and dual-transport delivery to MQTT — built so that losing connectivity never means losing data.

**Firmware v6.2** · ESP32 + A7670C/SIM7600 + NEO-8M · Arduino framework

---

<img width="1280" height="576" alt="image" src="https://github.com/user-attachments/assets/8b477f26-1298-4b47-b8c7-2fbe1814c3d4" />



---
## The design invariant

> **Connectivity is optional. Acquisition and preservation are not.**

The device is expected to spend real time in bags, pockets, basement car parks, and under corrugated roofs. Every architectural decision below follows from assuming the link is down and the sky is blocked.

- **Records are never dropped on a failed send.** A live record is held until the broker echoes it back; on timeout it is re-queued to the SD FIFO rather than discarded.
- **Sync progress survives reboots.** The `{file sequence, records ACKed}` pair is persisted to NVS after every verified batch, so a reconnect flap or a reboot mid-file cannot replay work the broker already confirmed.
- **A file is never deleted on a read that returned zero records.** A zero read is an error, not a completion.
- **Batch truncation is structurally impossible.** Record count is computed from the byte budget *before* serialising, not discovered while writing.
- **The clock is ranked, not assumed.** `NETWORK (NTP/CELL) > GPS > HOLDOVER > UNSYNCED`, with an explicit unsynced state that downstream consumers cannot silently plot.

---

## Hardware

| Subsystem | Part | Interface |
|---|---|---|
| MCU | ESP32 (dual-core, FreeRTOS) | — |
| GNSS | u-blox NEO-8M | UART2 — RX `16`, TX `17` |
| Cellular | A7670C / SIM7600 | UART — RX `27`, TX `26`, PWRKEY `4` |
| Storage | microSD (SPI) | MISO `19`, MOSI `23`, SCK `18`, CS `5` |
| Battery sense | 100 kΩ / 100 kΩ divider | ADC1 `34` |
| Charge status | TP4056 CHRG / STDBY | `35` / `39`, active LOW, external 10 kΩ pull-ups |
| LEDs | Red / Yellow / Green | PWM `25` / `33` / `32` |
| Force-setup | Boot button | `0` |

**LED semantics (v6.0 scheme):** RED = battery level (brighter = lower) · YELLOW = network and transmission · GREEN = GPS quality (brighter = better).

Full pinout notes, the ADC source-impedance limitation, and the charge-sense wiring are in [`docs/HARDWARE.md`](docs/HARDWARE.md).

---











## 📌 1. Project Overview

### What does this project do?

BB-TRACKER turns an **ESP32 microcontroller** into a **standalone GPS tracking device**. It:

1. Reads GPS coordinates (latitude, longitude, speed, heading, altitude, satellites) from a GPS module.
2. Stamps every reading with the current Bangladesh time (UTC+6).
3. Tries to send that data **live**, in real time, to an MQTT server over the internet (Wi-Fi first, then a 4G SIM as backup).
4. If there is **no internet connection at all**, it never throws the data away — it safely writes every GPS point to an SD card, and automatically uploads ("syncs") all the saved points the moment the internet comes back, in the exact order they were recorded.
5. Hosts its **own Wi-Fi hotspot** and a **web dashboard** (a website you open in your phone/laptop browser) so you can configure the device, watch live status, and download logs — even with no internet.
6. Uses three LEDs to show battery level, GPS quality, and network/transmission status at a glance, without needing a screen.

### Real-world use case

This is designed for **vehicle or asset tracking** in areas with unreliable internet — for example:
- Tracking a delivery van, rickshaw, or bus across Bangladesh where Wi-Fi/cellular coverage comes and goes.
- Logging the exact route of a vehicle even through "dead zones," then automatically catching up and sending all the missed points once signal returns.
- A battery-powered, solar-chargeable tracker that a technician can configure just by connecting to its Wi-Fi hotspot with a phone — no laptop or SIM-card menu needed.

### Expected output / behavior

- Every few seconds (configurable), the device captures a GPS point and either:
  - **Publishes it live** to an MQTT broker (topic like `tracker/gps/test`), or
  - **Saves it to the SD card** if there's no connection.
- A **Serial Monitor** (USB) shows a live table of GPS/battery/network status, plus a text-based command console (type `HELP`).
- A **web dashboard** at `http://192.168.4.1` (the device's own hotspot) shows live map data, battery %, GPS fix quality, sync status, and lets you change settings (Wi-Fi, MQTT server, GPS interval, etc.).
- Three onboard LEDs behave like this:
  | LED | Meaning | Behavior |
  |---|---|---|
  | 🔴 Red | Battery level | Brighter = lower battery |
  | 🟡 Yellow | Network / transmission | Blinking = offline, slow "breathing" glow = uploading a backlog, quick flash = live data just sent |
  | 🟢 Green | GPS signal quality | Brighter = better GPS fix; blinks if GPS signal is stale/lost |

<img width="1280" height="576" alt="light" src="https://github.com/user-attachments/assets/9bd0980f-58e2-4f0a-850f-e2c72d48c672" />






---
## Build

### 1. Toolchain

Arduino IDE 2.x or `arduino-cli`, with the ESP32 core installed. Both core 2.x and 3.x are supported — the LEDC and task-watchdog APIs are switched on `ESP_ARDUINO_VERSION_MAJOR` at compile time.

```bash
arduino-cli core install esp32:esp32
```

### 2. Libraries

| Library | Used for |
|---|---|
| `TinyGPSPlus` | NMEA parsing |
| `TinyGSM` | Modem control and TCP over cellular |
| `PubSubClient` | MQTT client |

```bash
arduino-cli lib install "TinyGPSPlus" "TinyGSM" "PubSubClient"
```

`WiFi`, `WebServer`, `DNSServer`, `Preferences`, `SD`, `SPI`, and `FS` ship with the ESP32 core.

### 3. Credentials

The sketch will not compile until you provide `secrets.h`:

```bash
cd firmware/GPS_batttery_updated_23_08_2026
cp secrets.example.h secrets.h
$EDITOR secrets.h
```

`secrets.h` is gitignored. It supplies **first-boot defaults only** — a device that has already been provisioned reads its configuration from NVS and never looks at these values. They matter on a virgin board, after a factory reset, and after a `CONFIG_SCHEMA_VERSION` bump.

> The default broker is `broker.hivemq.com`, which is public and unauthenticated. On a public broker the topic string is the only thing between your live position feed and anyone who subscribes — pick a non-obvious one, and treat it as a credential.

### 4. Board settings

| Setting | Value |
|---|---|
| Board | ESP32 Dev Module |
| Partition scheme | Default 4 MB with spiffs (1.2 MB APP) |
| Flash size | 4 MB |
| PSRAM | Disabled |
| Upload speed | 921600 |

### 5. Compile and upload

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 firmware/GPS_batttery_updated_23_08_2026
arduino-cli upload  --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 firmware/GPS_batttery_updated_23_08_2026
```

Then open the serial monitor at **115200** and type `HELP`.

---

## First run

1. **Calibrate the battery.** Rest the pack, measure the terminals with a multimeter, put that number in `BATTERY_CAL_REFERENCE_VOLTAGE`, upload once. The firmware solves its own correction on first boot and writes it to a dedicated NVS namespace that a config wipe cannot touch. You never enter it again. See [`docs/CALIBRATION.md`](docs/CALIBRATION.md).
2. **Reach the dashboard.** The device raises its own AP at boot regardless of station state. Connect, open `http://192.168.4.1`, unlock with the user or dev PIN.
3. **Verify end-to-end.** Run `TEST` on the serial console. The hardware legs run immediately; the E2E leg needs a live MQTT session, so it re-runs automatically on first connect.
4. **Exercise the offline path.** `NETSIM OFF` simulates an outage without touching the radio, so the offline → sync → live transition can be driven deterministically on the bench. `NETSIM AUTO` restores normal operation.

---

## Repository layout

```
firmware/GPS_batttery_updated_23_08_2026/
  GPS_batttery_updated_23_08_2026.ino   Firmware. Single translation unit.
  tracker_types.h                       POD types that cross task boundaries.
  dashboard_html.h                      Dashboard, PROGMEM raw string literal.
  secrets.example.h                     Credential template. Copy to secrets.h.

docs/
  ARCHITECTURE.md    Task model, ownership rules, data path, time authority.
  HARDWARE.md        Pinout, wiring, ADC limitations, LED scheme.
  PROTOCOL.md        MQTT wire schema, SD JSONL schema, ACK contract.
  CONSOLE.md         Serial commands and the HTTP route/role matrix.
  CALIBRATION.md     Battery calibration procedure and the discharge curve.
```

The Arduino IDE requires the sketch folder name to match the `.ino` filename, which is why that directory keeps its original name.

---

## Architecture at a glance

Four tasks, each with exactly one owner for each shared resource. No resource has two writers.

| Task | Core | Prio | Owns |
|---|---|---|---|
| `taskSensor` | 1 | 3 | GPS UART and parsing, battery ADC, LEDs, record generation |
| `taskStore` | 1 | 2 | The SD FIFO — sole owner, no exceptions |
| `taskNet` | 0 | 2 | Wi-Fi, modem, MQTT, live publish, sync engine |
| `loopTask` | 1 | 1 | Dashboard, captive DNS, serial console, watchdog |

Nothing in `setup()` blocks on the network. The device is fully functional — acquiring, logging, serving its dashboard — before any link exists.

**Battery sampling is owned by one task at a fixed 250 ms cadence.** Everything else reads a cached value. This is a correctness requirement, not an optimisation: five call sites each triggering 64 blocking ADC reads at unpredictable times was the root cause of v5.2's unstable readings.

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the data path, the time authority, and the queue-crossing type rules.

---

## Version history

`v6.0` repaired four architectural causes of silent offline data loss in v5.2. `v6.1` replaced ad-hoc GPS timestamping with a single ranked time authority. `v6.2` inverted that ranking to network-first after field evidence showed GPS absent far more often than the SIM.

Full detail in [`CHANGELOG.md`](CHANGELOG.md). The root-cause analyses are preserved in the sketch header, where they sit next to the code they explain.

---

## License

MIT — see [`LICENSE`](LICENSE).
