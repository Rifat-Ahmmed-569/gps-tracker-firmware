# Wire protocol

Two schemas, deliberately different. The **SD JSONL** schema is internal and compact — it is written and read only by this firmware. The **MQTT** schema is external and locked to what the backend already ingests.

---

## MQTT

| | |
|---|---|
| Broker (default) | `broker.hivemq.com:1883` |
| Live topic | `cfg.topic` |
| Bulk topic | `cfg.topic` — same topic in this build; see note |
| ACK topic | `<cfg.topic>/ack` — only when `ackMode == 1` |

> **Bulk topic.** `ConfigManager::bulkTopic()` returns `cfg.topic` unconditionally in this build — live and playback share one topic. The v5.2 contract of `<topic>/playback` is preserved in a commented expression on that line, and `pbOnLiveTopic` still exists in config. Restore the split by returning `cfg.pbOnLiveTopic ? cfg.topic : (cfg.topic + "/playback")`.

> **The topic is a credential.** `broker.hivemq.com` is public and unauthenticated. Anyone who knows the topic string can subscribe to the live position feed. This is why it lives in `secrets.h`.

### Live payload

```json
{
  "username": "<deviceId>",
  "modelList": [{
    "username":          "<deviceId>",
    "appCode":           "DEMO",
    "latitude":          23.7808751,
    "longitude":         90.2792371,
    "platform":          "IoMT",
    "visitDate":         "2026-08-23 14:07:31",
    "visitTime":         "2026-08-23 14:07:31",
    "networkType":       "WIFI",
    "broadcastEnabled":  true,
    "locationAccuracy":  0.98,
    "altitude_msl":      12.40,
    "speed_kmph":        0.00,
    "heading_deg":       0.00,
    "satellite_count":   9,
    "internet_available": true,
    "batteryPower":      87,
    "status":            "live",
    "time_valid":        true,
    "time_src":          "NTP"
  }]
}
```

### Field reference

| Field | Type | Notes |
|---|---|---|
| `username` | string | Device ID. Appears twice — envelope and record. |
| `appCode` | string | Constant `"DEMO"`. |
| `latitude` / `longitude` | number | Decimal degrees, 7 dp. |
| `platform` | string | Constant `"IoMT"`. |
| `visitDate` / `visitTime` | string | **Both carry the full `"YYYY-MM-DD HH:MM:SS"` timestamp.** Not split. Local time (UTC+6). |
| `networkType` | string | `WIFI` \| `SIM` \| `NONE`. `NONE` is honest, not an error — the record was captured with no link. |
| `broadcastEnabled` | bool | Constant `true`. |
| `locationAccuracy` | number | **HDOP**, not metres. `99.9` when unknown. |
| `altitude_msl` | number | Metres above mean sea level. |
| `speed_kmph` | number | km/h. |
| `heading_deg` | number | Degrees true. |
| `satellite_count` | int | Satellites used in the fix. |
| `internet_available` | bool | `true` on live, `false` on bulk. |
| `batteryPower` | int | Percent, 0–100. |
| `status` | string | `"live"` or `"playback"`. |
| `time_valid` | bool | `false` iff `tsrc == TSRC_NONE`. |
| `time_src` | string | `GPS` \| `NTP` \| `CELL` \| `HOLDOVER` \| `UNSYNCED`. |

**Only two fields differ between a live and a stored record:** `internet_available` and `status`. Everything else is byte-identical, which is what makes the E2E echo comparison meaningful.

### Bulk payload

Same envelope. `modelList` carries up to **30** records, each with `"status": "playback"` and `"internet_available": false`.

### Size budget

| Constant | Value | Meaning |
|---|---|---|
| `REC_JSON_MAX` | 496 | Worst-case one backend record plus its comma |
| `PAYLOAD_CAP` | 15360 | Static buffer — never heap, so no fragmentation |
| `batchSize` | ≤ 30 | Hard-clamped at config load |

30 × 496 = 14,880, inside 15,360 with the envelope. **Record count is derived from the byte budget before serialising begins.** This is what makes truncation structurally impossible rather than merely unlikely (root cause R4).

If you add a field: recompute `REC_JSON_MAX` first. v6.1's two new fields pushed a record from 427 to 468 bytes, over the old cap of 448, and would have silently re-armed R4.

---

## ACK contract

Two modes, selected by `cfg.ackMode`.

### Mode 0 — broker echo (default)

The device subscribes to its own publish topic. A batch is confirmed when the broker echoes back a payload matching what was sent. This proves the whole chain: **device → transport → broker → device**.

No backend cooperation required. It cannot prove the backend ingested anything.

### Mode 1 — backend ACK topic

The backend publishes a confirmation to `<topic>/ack`. `E2EVerifier` matches on that topic only.

Stronger, but the backend must implement it.

### On failure

`SyncEngine` reports `Delivery unverified (no broker echo / backend ACK)` and the batch is **not** advanced. The file stays in `/LOGS/QUEUE`. Sync progress in NVS is only ever written after a *verified* batch, so a failure cannot lose records and a retry cannot duplicate them.

### `SyncStatus`

Integer values are published on `/api/status` and the dashboard switches on the number. **Appending is safe; reordering is not.**

| Value | State |
|---|---|
| 0 | `SYNC_IDLE` |
| 1 | `SYNC_PREPARING` |
| 2 | `SYNC_UPLOADING` |
| 3 | `SYNC_WAITING_ACK` |
| 4 | `SYNC_RETRYING` |
| 5 | `SYNC_COMPLETED` |
| 6 | `SYNC_FAILED` |
| 7 | `SYNC_PAUSED` |

---

## SD JSONL

One record per line. Files live in `/LOGS/QUEUE` until acknowledged, then move to `/LOGS/SENT`.

```json
{"seq":10427,"ts":"2026-08-23 14:07:31","lat":23.7808751,"lng":90.2792371,"alt":12.40,"spd":0.00,"hdg":0.00,"sats":9,"hdop":0.98,"bat":87,"net":"WIFI","fix":1,"tsrc":2}
```

| Field | Type | Notes |
|---|---|---|
| `seq` | uint32 | Monotonic. Persisted to NVS every 10 records. |
| `ts` | string | `"YYYY-MM-DD HH:MM:SS"`, or `"0000-00-00 00:00:00"` when unsynced. |
| `lat` / `lng` | double | Decimal degrees, 7 dp. |
| `alt` | float | Metres MSL. |
| `spd` | float | km/h. |
| `hdg` | float | Degrees true. |
| `sats` | uint16 | |
| `hdop` | float | `99.9` when unknown. |
| `bat` | uint8 | Percent. |
| `net` | string | `"WIFI"` \| `"SIM"` \| `"NONE"`. |
| `fix` | uint8 | `1` = GPS position valid at capture. |
| `tsrc` | uint8 | `TimeSrc` integer, 0–4. |

`REC_LINE_MAX` is **224** bytes, with a truncation guard on `append()`.

### Two things that must not change

- **`net` round-trips as a string.** `netName()` writes it and `SDStore::_parse()` matches on the literals `"WIFI"` and `"SIM"`. The integer values of `NetKind` are never serialised — only the names. Changing the spellings breaks every card already in the field.
- **`tsrc` round-trips as an integer.** Renumbering `TimeSrc` re-labels every record already queued on a card. Append new sources at the end; never reorder. `TSRC_NONE` must stay `0`, because every `GpsRecord` is `memset()` to zero before it is filled.

### The unsynced timestamp

`"0000-00-00 00:00:00"` is not a placeholder that happens to be invalid. It is invalid **on purpose.**

The predecessor emitted `1970-01-01 00:00:00`, which parses cleanly in every JSON consumer and gets plotted at the epoch — a real-looking point at a real-looking time, and no way for a downstream system to tell it from a genuine reading. An impossible date forces the consumer to handle it explicitly. `time_valid: false` says the same thing on the MQTT side.

---

## Consumer guidance

**Deduplicate on `seq`.** The ACK contract guarantees no *loss*, and persisted sync progress makes duplicates rare — but a broker echo that arrives after the retry timer fired can produce one. `seq` is monotonic per device, so `(username, seq)` is a natural idempotency key.

**Never plot a record with `time_valid: false`.** The position is good; the time is not.

**`locationAccuracy` is HDOP, not metres.** A rough conversion is `metres ≈ HDOP × 5`, but that constant depends on the receiver and the sky. Prefer treating it as a unitless quality figure: under 1 is excellent, 1–2 good, 2–5 acceptable, over 5 poor, `99.9` means unknown.

**`visitDate` and `visitTime` are identical.** Both carry the full timestamp. Parse either; do not try to concatenate them.
