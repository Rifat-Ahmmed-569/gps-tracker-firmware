# Console and HTTP API

## Serial console

Line-based, non-blocking, running on `loopTask`. **115200 baud.** Type `HELP` for the live list.

Every print goes through `sPrintf()`, which takes a mutex — a telemetry row can never be spliced into the middle of a console reply.

### Status

| Command | Does |
|---|---|
| `STAT` | Live status — fix, satellites, HDOP, battery, transport, sequence |
| `NET` | Connectivity manager state |
| `SD` | Card statistics |
| `QUEUE` | FIFO state: files, tail, sync offset |
| `TIME` | Clock source, age of the reference, next sync attempt |

### Stored data

| Command | Does |
|---|---|
| `LS` | List batch files, pending and sent |
| `CAT <n\|path>` | Dump a batch file raw as JSONL |
| `PLAY <n\|path>` | Pretty-print a batch file as a table |
| `DEL <n\|path>` | Delete a batch file |

`<n>` is the index from `LS`. A bare name is resolved under `/LOGS/QUEUE`; a leading `/` is taken as an absolute path.

### Actions

| Command | Does |
|---|---|
| `SYNC` | Force a sync attempt now |
| `TIME SYNC` | Force a network time sync now |
| `TEST` | Re-run the self-test |
| `SIM` | Staged cellular bring-up test — **does not touch Wi-Fi** |

### Bench tools

| Command | Does |
|---|---|
| `NETSIM OFF` | Pretend the link is down |
| `NETSIM ON` | Pretend the link is up |
| `NETSIM AUTO` | Normal operation |
| `TRACE ON\|OFF` | Per-record raw JSON trace |
| `VCAL ...` | Battery diagnostics and calibration — see [`CALIBRATION.md`](CALIBRATION.md) |

`NETSIM` simulates an outage **without touching the radio**, so the offline → sync → live path can be exercised deterministically. This is the right way to test the FIFO: pulling the antenna or walking into a lift introduces a real radio state you cannot reproduce twice.

### The telemetry table

One row per generated record, fixed-width, header re-printed every 20 rows. Paste-into-Excel clean. It stays on when `DEBUG_MODE` is `0` — that switch silences the `DBG` chatter, not the telemetry.

---

## HTTP API

Served by `WebDashboard` on `loopTask`. The device raises its own AP at boot regardless of station state, with captive DNS, so the dashboard is always reachable at **`http://192.168.4.1`** even with no infrastructure at all.

The route and role matrix is preserved verbatim from v5.2.

### Roles

Three levels. Unlock with a 4-digit PIN at `/lock`; the session returns a token that subsequent calls must present.

| Role | Reaches |
|---|---|
| **public** | `/lock`, `/api/unlock` only |
| **user** | Status, stored data, downloads |
| **dev** | Everything, including config, destructive operations, and the diagnostic routes |

PIN defaults live in `secrets.h`. Both are settable from the dashboard (`devPin`, `userPin`, exactly 4 digits each).

### Routes

**Public**

| Route | Purpose |
|---|---|
| `GET /lock` | Unlock page |
| `POST /api/unlock` | PIN → session token |
| `POST /api/logout` | Clear session |

**User**

| Route | Purpose |
|---|---|
| `/api/basic` | Minimal status |
| `/api/status` | Full status including `(int)SyncStatus` |
| `/api/storage` | SD usage |
| `/api/log_files` | List batch files |
| `/api/log_page` | Paged record view |
| `/api/playback_view` | Render a stored batch **exactly as the broker will receive it** |
| `/api/download_csv` | Export CSV |
| `/api/download_json` | Export JSON |

**Dev**

| Route | Purpose |
|---|---|
| `/api/config` | Read configuration |
| `/api/save` | Write configuration |
| `/api/scan` | Wi-Fi scan |
| `/api/netdiag` | Network diagnostics |
| `/api/selftest` | Run the self-test |
| `/api/sync` | Force sync |
| `/api/pause_sync` · `/api/resume_sync` | Sync control |
| `/api/set_batch_size` | Batch size, clamped to `[1, 30]` |
| `/api/restart` | Reboot |
| `/api/delete_log` · `/api/delete_all_logs` | Delete stored data |
| `/api/format_sd` | Format the card |
| `/api/factory_reset` | Wipe config, FIFO, **and battery calibration** |

> `/api/factory_reset` is the **only** path that erases per-board battery calibration. A config wipe deliberately preserves it.

> `/api/playback_view` renders a stored batch byte-for-byte as the broker will receive it. When a backend rejects a batch, compare against this before suspecting the transport — it removes serialisation from the list of suspects in one step.

### Session handling

`_sessionToken` is a single active session. A new unlock replaces the previous one. Every gated route checks `srv.arg("token")` against it and clears the session on mismatch.

Config changes touching `wifiSsid` set `needsReboot`.

---

## Self-test

`TEST` on the console, or `/api/selftest`.

Hardware legs run immediately. The **E2E leg needs a live MQTT session**, so it does not fail on a device that has not connected yet — it re-arms and runs automatically on the first MQTT connect.

The E2E leg proves the full round trip: device → transport → broker → device. A pass means the transport chain is sound. It does **not** prove the backend ingested anything — for that you need `ackMode == 1`. See [`PROTOCOL.md`](PROTOCOL.md).

---

## Reading the LEDs

Faster than opening a console when you just want to know whether the thing is working.

| LED | Meaning |
|---|---|
| **Red** | Battery level — PWM, **brighter = lower** |
| **Green** | GPS quality — PWM, **brighter = better** |
| **Yellow** | Network and transmission |

Yellow's three patterns:

| Pattern | State |
|---|---|
| 1 Hz square blink | Offline |
| Dark, flashing per transmission + 10 s heartbeat | Live |
| 1.2 s triangle ramp | Bulk — the backlog is draining |

A device sitting in triangle ramp for a long time is working correctly and has a large backlog. Check `QUEUE` for the depth.
