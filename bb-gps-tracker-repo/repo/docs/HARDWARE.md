# Hardware

## Pinout

### GPS — u-blox NEO-8M, UART2

| Signal | ESP32 | Note |
|---|---|---|
| RX | `16` | ESP32 receives NMEA here |
| TX | `17` | |

### Cellular — A7670C / SIM7600

| Signal | ESP32 | Note |
|---|---|---|
| RX | `27` | |
| TX | `26` | |
| PWRKEY | `4` | Pulse to power the modem on |

Build switch: `TINY_GSM_MODEM_SIM7600`. Set `ENABLE_MODEM_FUNCTIONALITY` to `0` to compile a Wi-Fi-only image — all cellular code is isolated behind that switch.

### microSD — SPI

| Signal | ESP32 |
|---|---|
| MISO | `19` |
| MOSI | `23` |
| SCK | `18` |
| CS | `5` |

### Battery sense

| Signal | ESP32 | Note |
|---|---|---|
| `BATT_ADC_PIN` | `34` | ADC1, 12-bit, 11 dB attenuation |
| `CHRG_SENSE_PIN` | `35` | TP4056 CHRG — active LOW, **external 10 kΩ pull-up required** |
| `FULL_SENSE_PIN` | `39` | TP4056 STDBY — active LOW, **external 10 kΩ pull-up required** |

GPIO `34`, `35`, and `39` are input-only and have **no internal pull-ups.** The pull-ups on the two TP4056 lines must be fitted externally or both will float. Charge sense is behind `ENABLE_CHARGE_SENSE`.

### LEDs — v6.0 scheme

| LED | ESP32 | Meaning |
|---|---|---|
| Red | `25` | Battery level — PWM, **brighter = lower** |
| Yellow | `33` | Network and transmission |
| Green | `32` | GPS quality — PWM, **brighter = better** |

PWM: 5 kHz, 12-bit (`PWM_MAX` 4095), channels 0/1/2. The `LEDC` API changed between ESP32 Arduino core 2.x and 3.x, so `PWM_ATTACH`/`PWM_WRITE` switch on `ESP_ARDUINO_VERSION_MAJOR`. Both cores build.

Yellow encodes three states, distinguishable at a glance across a room:

| State | Pattern |
|---|---|
| `NETLED_OFFLINE` | 1 Hz square blink |
| `NETLED_LIVE` | Dark, flashing on each transmission, plus a 10 s heartbeat |
| `NETLED_BULK` | 1.2 s triangle ramp while the backlog drains |

`LEDManager` is a pure state machine with zero `delay()`.

### Other

| Signal | ESP32 | Note |
|---|---|---|
| `FORCE_SETUP_PIN` | `0` | Boot button |

---

## Battery divider

```
BAT+ ──┬── R1 100 kΩ ──┬── GPIO34
       │               │
       │              R2 100 kΩ
       │               │
      GND ─────────────┴── GND
```

Ratio 2:1, so a 4.40 V pack presents 2.20 V at the pin — comfortably inside the 11 dB attenuation range.

### The limitation you cannot fix in firmware

100 kΩ ∥ 100 kΩ = **50 kΩ source impedance.** The ESP32 SAR ADC needs **under 10 kΩ** to fully charge its sample-and-hold capacitor within the acquisition window. At 50 kΩ the first reads after a channel switch are contaminated by the previously converted channel.

Firmware mitigates this with two discarded dummy reads before each burst, plus a 200 µs settle. That is mitigation, not a fix.

**If you respin the board:** drop to 10 kΩ/10 kΩ (higher quiescent draw — 220 µA at 4.4 V) or, better, add a 100 nF cap from the divider midpoint to ground. The cap gives the S/H a low-impedance local charge reservoir and costs nothing in current.

---

## Pack

Samsung EB-BG991ABY (Galaxy S21) class — a **4.40 V-charge high-voltage cell**, not a 4.20 V cell.

| Parameter | Value |
|---|---|
| Charge voltage | 4.40 V |
| Nominal | 3.86 V |
| Empty (firmware floor) | 3.20 V |

This matters: a discharge curve that tops out at 4.20 V = 100% clips roughly the top third of usable charge on this pack. See [`CALIBRATION.md`](CALIBRATION.md).

Charging is a TP4056 module. Its CHRG and STDBY outputs are open-drain, hence the external pull-ups above.

---

## Power notes

The modem is the dominant load. A SIM7600 draws bursts of **~2 A** during network attach and transmit. If the pack sags on attach:

- Fit at least 1000 µF of bulk capacitance close to the modem's `VBAT` pins.
- Keep the ground return from the modem to the pack short and wide.
- Check the modem's supply directly with a scope during an attach, not with a multimeter — the events are milliseconds long.

A sag deep enough to brown out the ESP32 will look like a random reboot in the logs. Check `resets` in the `sysprov` NVS namespace before assuming a firmware fault.

---

## Board settings

| Setting | Value |
|---|---|
| Board | ESP32 Dev Module |
| Partition scheme | Default 4 MB with spiffs (1.2 MB APP) |
| Flash size | 4 MB |
| PSRAM | Disabled |
| Upload speed | 921600 |
| Monitor baud | 115200 |

---

## Bench bring-up order

Bring the subsystems up one at a time. Each has a console command that exercises it in isolation — see [`CONSOLE.md`](CONSOLE.md).

1. **Power and serial.** `STAT` should respond. If not, nothing else is worth testing.
2. **Battery.** `VCAL` prints the raw ADC counts and pin millivolts. Compare pin millivolts against a DMM reading taken at GPIO `34` — that comparison, not the reported pack voltage, is what separates an ADC error from a divider error.
3. **SD.** `SD` prints card statistics. `LS` lists batch files.
4. **GPS.** `STAT` shows satellite count and HDOP. Cold fix outdoors takes 30–90 s; indoors it may never come, which is the whole reason the time authority is network-first.
5. **Wi-Fi.** Configure via dashboard, then `NET`.
6. **Cellular.** `SIM` runs a staged bring-up test that does not touch Wi-Fi.
7. **End to end.** `TEST`. The hardware legs run immediately; the E2E leg needs a live MQTT session and re-runs automatically on first connect.
8. **The offline path.** `NETSIM OFF` fakes an outage without touching the radio. Watch the yellow LED go to 1 Hz square, let records accumulate, then `NETSIM AUTO` and watch the triangle ramp while the backlog drains.
