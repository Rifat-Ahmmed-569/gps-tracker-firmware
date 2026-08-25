# Architecture

## The invariant

> Connectivity is optional. Acquisition and preservation are not.

Everything below is downstream of that sentence.

---

## Task model

Four tasks. The scheduling matters less than the ownership: **no shared resource has two writers.**

| Task | Core | Prio | Owns |
|---|---|---|---|
| `taskSensor` | 1 | 3 | GPS UART + TinyGPS++, battery ADC, LEDs, record creation, telemetry table |
| `taskStore` | 1 | 2 | The SD FIFO — sole owner, and the only place that decides what is written or deleted |
| `taskNet` | 0 | 2 | Wi-Fi, modem, MQTT, live publish, sync engine |
| `loopTask` | 1 | 1 | Dashboard, captive DNS, serial console, task watchdog |

Cellular work lives on `taskNet` specifically because it can block for a long time — modem init 10 s, `waitForNetwork` 30 s, `gprsConnect` up to 60 s. On `loop()` that combination tripped the 60 s task watchdog (root cause R8). It is now supervised by a software liveness timer instead.

`setup()` blocks on nothing network-related. The device acquires, logs, and serves its dashboard before any link exists.

### Serial is shared

Four tasks print. Every print goes through `sPrintf()`, which takes a mutex, so a telemetry row can never be spliced into the middle of a console reply.

---

## Types that cross task boundaries

Everything in `tracker_types.h` is **trivially copyable**: fixed-width scalars and fixed-size `char` arrays only. No `String`, no pointers, no virtuals.

This is load-bearing, not stylistic. `xQueueSend()` copies the struct byte-for-byte and `GpsRecord` is assigned wholesale (`g_liveHold = r`). Anything with a non-trivial copy constructor would corrupt silently.

### Enum values are wire format

Three enums have integer values that leave the device and come back:

| Enum | Where it goes | Rule |
|---|---|---|
| `NetKind` | SD JSONL `"net"` field, round-tripped through `netName()` and `SDStore::_parse()` | The **strings** `"WIFI"` and `"SIM"` must not change. Integers are never serialised. |
| `TimeSrc` | SD JSONL `"tsrc"` field, read back by `SDStore::_parse()` | Renumbering re-labels every record already queued on a card. Append at the end; never reorder. |
| `SyncStatus` | `/api/status` emits `(int)_sync->status`; the dashboard switches on the number | Appending is safe. Reordering is not. |

`TSRC_NONE` **must be zero.** Every `GpsRecord` is `memset()` to 0 before it is filled, so a record that never had its source assigned has to read back as "nothing has ever set the clock" rather than as a real source.

---

## Data path

```
GPS UART ──► TinyGPS++ ──► taskSensor ──► GpsRecord
                                            │
                        ┌───────────────────┴───────────────────┐
                        ▼                                       ▼
                  live queue                              SD FIFO queue
                  (taskNet)                                (taskStore)
                        │                                       │
                        ▼                                       ▼
                  MQTT publish                            JSONL append
                        │                                  /LOGS/QUEUE
                        ▼                                       │
                  broker echo?                                  ▼
                   ┌────┴────┐                            SyncEngine
                  yes        no                          (oldest first,
                   │          │                            ACK-gated)
                   ▼          ▼                                 │
                 done   re-queue to SD                          ▼
                                                          /LOGS/SENT
```

**Live records are held until verified.** `publish()` returning true only means the TCP write succeeded. The record is held until the broker echoes it back; on timeout it is re-queued to the SD FIFO rather than dropped (R5).

---

## The SD FIFO

`taskStore` is the sole owner. Directory layout:

```
/LOGS
/LOGS/QUEUE    pending batches
/LOGS/SENT     acknowledged batches, subject to auto-delete
```

One file is one MQTT batch. `batchSize` is hard-clamped to `[1, 30]` at load, because the batch must fit `PAYLOAD_CAP` with zero truncation.

### Four rules that exist because of specific data loss

1. **Every path is canonicalised.** Basename is re-anchored under its directory. Root cause R1 was `File::name()` already returning a full path on core 2.x, producing `/logs//logs/...`, which `SD.open()` rejects.
2. **A zero read is an error, not a completion.** A file is never deleted on a read that returned zero records.
3. **The tail file is sealed before it can be transmitted.** Otherwise sync deletes the file offline logging is still appending to (R6).
4. **Record count comes from the byte budget, computed before serialising.** Truncation is structurally impossible rather than merely unlikely (R4).

### The index

An O(1) in-RAM index — file count, oldest and next sequence, tail record count — reconciled against the filesystem at boot and every 5 minutes. It replaced full directory and file walks that ran on every 10 s idle tick and made drain O(n²) (R3).

### Sequence and progress persistence

- The record sequence counter persists to NVS every `SEQ_PERSIST_EVERY` records.
- Sync progress — `{file sequence, records ACKed}` — persists after every **verified** batch, in the `fifo` namespace.

Both survive reboots and reconnect flaps, so neither can replay work the broker already confirmed (R2).

---

## Time authority

`TimeService` is the single source of the clock. Ranking, as of v6.2:

```
NETWORK (NTP / CELL)  >  GPS  >  HOLDOVER  >  UNSYNCED
```

| Source | Value | Set by |
|---|---|---|
| `TSRC_NTP` | 2 | SNTP over Wi-Fi — primary |
| `TSRC_CELL` | 3 | Carrier NITZ via `AT+CCLK?` — primary |
| `TSRC_GPS` | 1 | Satellite time — fallback |
| `TSRC_HOLD` | 4 | Last valid reference, free-running on the crystal |
| `TSRC_NONE` | 0 | Unsynced — no source has ever set the clock |

### Why network beats GPS

GPS is ground truth *when there is a fix*. This deployment often has none — bag, pocket, basement car park, corrugated roof — while the SIM and Wi-Fi stay up. Ranking the sometimes-absent source above the almost-always-present one made the clock hostage to the sky.

### Fallback needs no new machinery

`source()` already implements a freshness predicate. A network reference older than `NET_FRESH_S`, or any tick with no transport up (`_netStarved`), reads as `HOLDOVER`. GPS is admitted the moment that happens and steps back down when the network returns. No new state machine, no new timer, no new constant.

### Unsynced is explicit

Records stamped with no valid time carry `ts` = `"0000-00-00 00:00:00"` — a deliberately impossible instant. The predecessor emitted `1970-01-01 00:00:00`, which every consumer happily plotted at the epoch. An impossible date forces the consumer to handle it.

v6.3 adds a record gate: records are discarded outright when `TimeService::source()` returns `TSRC_NONE`.

### Cost

Four SNTP exchanges a day over Wi-Fi. **Nothing** over cellular — `AT+CCLK?` is a local AT query against the NITZ value the modem already holds. Cadence is `TIME_REFRESH_MS` on success, `TIME_RETRY_MS` while failing.

### Timezone

The stored instant is UTC. `UTC+6` is applied **only** in `format()`. Nothing else in the firmware knows about local time.

---

## Battery pipeline

Owned by `taskSensor` alone, at a fixed 250 ms cadence:

```
2 dummy reads (charge S/H)
  → 21 samples
  → median
  → MAD outlier rejection
  → mean of survivors
  → divider + calibration  (V = V_raw × gain + offset)
  → slew limit  (reject > 30 mV per sample)
  → EMA  (α = 0.08, τ ≈ 3 s at 4 Hz)
  → discharge curve
  → percent hysteresis  (0.7% to move)
```

Everything else — dashboard, LEDs, serial, records — reads the cached value. **Nothing else touches the ADC.**

Three things went wrong in v5.2 and only one of them was filtering:

1. Five call sites invoked `update()`, each triggering 64 blocking ADC reads at unpredictable times. The moving average was fed at a random rate and the displayed value depended on who asked last.
2. The 100 kΩ/100 kΩ divider presents ~50 kΩ source impedance to a SAR ADC that needs under 10 kΩ to charge its sample-and-hold. **This is a hardware limit** — the dummy reads mitigate it, firmware cannot fix it.
3. Percentage was recomputed from a jittering voltage with no hysteresis, so 1 LSB of noise flipped whole percentage points.

A sanity gate discards any sample outside `[1.00 V, 5.00 V]` — a broken conversion or a disconnected divider is not a battery state, and one such sample must not drag the pack to 0% or 100%.

---

## Boot provisioning

Two independent gates decide whether stored state may be trusted:

- **`CONFIG_SCHEMA_VERSION`** (currently `8`) — bumped by hand whenever the shape or meaning of anything in NVS changes. A mismatch is always a hard reset, because an old layout silently driving new code is exactly the failure this mechanism exists to prevent.
- **Build identity** — policy-dependent; `0` means only a schema bump resets.

NVS namespaces are deliberately separate:

| Namespace | Holds | Erased by |
|---|---|---|
| `tracker` | 17 config keys | Config wipe, factory reset |
| `fifo` | Sync progress, sequence counter | Factory reset |
| `sysprov` | Schema, build, reset count | Provisioner only |
| `battcal` | Per-board calibration | **Factory reset only** |

Battery calibration is per-board *hardware* calibration, not application configuration. It lives in its own namespace so a config wipe cannot take it.

---

## One thing that will look wrong

`ProvisionResult` and `BatteryCal` are declared at the top of the sketch, next to the includes, rather than beside the functions that use them.

This is deliberate and must not be "cleaned up." The Arduino IDE runs a ctags pass over the `.ino`, generates a prototype for every function it finds, and injects the whole set immediately before the first function definition. A generated prototype naming a type declared later in the file cannot compile — and the error is reported against the *original* function line, which makes it look as though the definition is at fault rather than the injected copy.

The same rule governs `GpsRecord`, `SyncStatus`, and `NetLed`.
