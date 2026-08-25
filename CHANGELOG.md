# Changelog

Root-cause identifiers (`R1`–`R8`, `T1`–`T6`) match the analysis comments in the sketch header. They are stable across versions — cite them in issues.

---

## v6.2 — Time authority inverted: network first

v6.1 built the right machine and pointed it the wrong way. It ranked `GPS > NTP/CELL > HOLDOVER > UNSYNCED` on the reasoning that GPS is ground truth. It is — when there is a fix. On this deployment there very often is not, while the SIM and the Wi-Fi stay up throughout. Ranking the sometimes-absent source above the almost-always-present one made the clock hostage to the sky.

Authority is now **`NETWORK (NTP/CELL) > GPS > HOLDOVER > UNSYNCED`**.

Three code paths implemented the old ranking; all three changed:

- **T4** — `ingestGps()` called `_setRef()` unconditionally, four times a second, without ever asking what had set the clock. A single NMEA sentence silently overwrote a good NTP reference, then kept overwriting it. It now refuses authority while the effective source is a fresh network reference, and measures the disagreement instead.
- **T5** — `ingestNetwork()` refused a network value outright whenever the current reference was GPS younger than one hour. A bad GPS instant that cleared the sanity floor therefore owned the clock for a full hour with a correct NTP answer sitting in hand. The refusal is gone; the network is never refused for authority.
- **T6** — `netSyncTick()` skipped the sync entirely while the source was GPS, so on a unit with a fix the network was not merely outranked, it was never asked. That early return is gone.

Fallback needs no new state machine: the freshness predicate `source()` already implemented does the work. A network reference older than `NET_FRESH_S`, or any tick with no transport up, reads as `HOLDOVER`, and GPS is admitted the moment that happens.

**Retired:** `GPS_AUTHORITY_HOLD_S`. Under network-first authority there is no window in which GPS may refuse a network correction.

**Cadence unchanged**, so this adds no traffic: four SNTP exchanges a day over Wi-Fi, and nothing at all over cellular, where `AT+CCLK?` is a local query against the NITZ value the modem already holds.

**Unchanged:** timestamp format, wire schema, `tsrc` on SD, the SD FIFO, the sync engine, GPS acquisition and parsing, MQTT, pins, NVS keys.

### Battery calibration (this build)

- Persistent per-board calibration in its own NVS namespace (`battcal`), CRC-guarded and plausibility-checked. A config wipe cannot take it; only an explicit factory reset erases it.
- Correction model widened from gain-only to `V = V_raw × gain + offset`. Gain covers everything that scales with the reading (resistor tolerance, ADC reference error, source-impedance loss); offset covers fixed drops that do not (protection FET, series diode). Offset defaults to `0`, so an existing gain-only calibration behaves exactly as before.
- Discharge curve retargeted to a **4.40 V-charge** high-voltage cell (Samsung EB-BG991ABY class, 3.86 V nominal). The previous table topped out at 4.20 V = 100%, which clipped roughly the top third of usable charge on this pack.
- One-shot auto-solve: set `BATTERY_CAL_REFERENCE_VOLTAGE` to a multimeter reading, upload once, done. The stored reference is compared against the compiled one, so an unchanged value means "already calibrated" and the device will not re-solve against whatever charge state the pack happens to be in.
- `VCAL` console family for diagnostics, two-point calibration, and reset.

> The curve points are an estimate anchored on the published nominal and charge voltages — **not** measured from your pack. Voltage measurement and percentage estimation are independent systems, so replacing the table with real coulomb-counted data requires no other change.

---

## v6.1 — Time source repair

Scope deliberately narrow: only the clock changed.

- **T1** — `GPSManager` derived the timestamp purely from live TinyGPS++ state and emitted a hardcoded `1970-01-01 00:00:00` whenever that state was not yet populated — i.e. every record from boot until first fix. Replaced by `TimeService`, a single ranked authority that also uses the network.
- **T2** — `TinyGPSDate`/`TinyGPSTime::valid` latch true on first commit and are never cleared, so after a fix was lost the old code re-emitted the last parsed instant indefinitely. Plausible-looking, and undetectable downstream. `TimeService` checks `age()`, refuses stale GPS, and projects forward from the last good reference.
- **T3** — Two new wire fields pushed a backend record from 427 to 468 bytes, over the old `REC_JSON_MAX` of 448, which would have silently re-armed root cause R4. `REC_JSON_MAX` raised to 496, `PAYLOAD_CAP` to 15360, holding the batch at 30 records. `REC_LINE_MAX` 200 → 224 with a truncation guard on `append()`.

**Wire changes, additive only:**

- Live and bulk records gain `"time_valid": true|false` and `"time_src": GPS|NTP|CELL|HOLDOVER|UNSYNCED`
- SD JSONL records gain `"tsrc": 0..4`
- Unsynced records carry `ts` = `"0000-00-00 00:00:00"` — an impossible instant, chosen so no consumer can silently plot it the way `1970-01-01` was plotted

---

## v6.0 — Architectural repair of offline data loss

A targeted repair of v5.2, not a rewrite. Pins, broker, topics, payload field names, dashboard, PIN roles, serial commands, watchdog, E2E verification, and NVS config keys are all preserved.

### Why v5.2 lost offline data

- **R1 — Path corruption.** `SDLogger` built every path as `String(SD_LOG_DIR) + "/" + String(entry.name())`. On ESP32 Arduino core 2.x, `File::name()` already returns the full path, so this produced `/logs//logs/off_x.jsonl`, and `SD.open()` on that path returns null. `oldestUnsyncedFile()` therefore handed the sync engine a path it could never open → `readBatch()` returned empty → `linesRead == 0` → `_prepareBatch()` concluded "file fully pushed" and **deleted the file without ever transmitting it.** The single biggest source of silent loss. v6.0 canonicalises every path, and a file is never deleted on a zero read.
- **R2 — RAM-only sync progress.** `_lineOffset` lived in RAM and reset to 0 on reboot *and* on every `resume()`/`triggerNow()`. A reconnect flap mid-file re-sent every already-ACKed record. Now persisted to NVS after every verified batch.
- **R3 — Unbounded-scan indexing.** `oldestUnsyncedFile()`, `getStats()`, and `countLines()` walked the whole directory and whole files on every 10 s idle tick, making drain O(n²) and stalling the loop. Replaced by an O(1) in-RAM index reconciled at boot and every 5 minutes.
- **R4 — Silent batch truncation.** `buildPlaybackJSON()` stopped emitting records near the 16 KB cap, but the caller advanced the offset by records *read*, not records *serialised*. Every truncated batch permanently skipped records. Record count is now derived from the byte budget before serialising.

### Also fixed

- **R5** — Live records were dropped when `publish()` returned true but the broker echo never arrived. A live record is now held until its echo verifies; on timeout it is re-queued to the SD FIFO.
- **R6** — Sync deleted the file that offline logging was still appending to. The tail file is now sealed before it can be transmitted.
- **R7** — Five subsystems each called `battery.update()`, each triggering 64 blocking ADC reads. Sampling is now owned by one task at a fixed cadence.
- **R8** — Modem fallback could block `loop()` for over 60 s and trip the task watchdog. All modem work moved to the network task, supervised by a software liveness timer.

### Structural changes

- Dashboard extracted from inline paste to `dashboard_html.h` as a PROGMEM raw string literal. This is what fixed the `#map invalid preprocessing directive` and `extended character` compile errors — do not remove the raw-string wrapper.
- Boot provisioning gated on `CONFIG_SCHEMA_VERSION`, so a stale NVS layout can never silently drive new code.

---

## v5.x — Prior line

Dual-core FreeRTOS architecture (`senseTask`/`commsTask`), PIN-gated dashboard roles, positioning stack with Kalman filter inlined into a single `.ino`. Superseded by v6.0.
