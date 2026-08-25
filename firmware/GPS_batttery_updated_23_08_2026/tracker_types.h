// ============================================================================
//  tracker_types.h — shared plain-old-data types for the BOTS BANGLA tracker
//
//  Every type here crosses a FreeRTOS queue, a task boundary, or both, so all
//  of them are trivially copyable: fixed-width scalars and fixed-size char
//  arrays only. No String, no pointers, no virtuals. xQueueSend() copies the
//  struct byte-for-byte and GpsRecord is assigned wholesale (g_liveHold = r),
//  so anything with a non-trivial copy constructor would corrupt silently.
//
//  ---------------------------------------------------------------------
//  v6.1 additions — the time authority
//  ---------------------------------------------------------------------
//  TimeSrc, timeSrcName() and GpsRecord::tsrc were introduced alongside
//  TimeService. If you are upgrading a v6.0 copy of this header, those three
//  are the only things that changed; everything else below is as it was.
// ============================================================================
#pragma once

#include <Arduino.h>
#include <stdint.h>

// ============================================================================
//  NetKind — which transport carried (or would have carried) a record.
//
//  ORDERING IS LOAD-BEARING. netName() round-trips through the SD JSONL
//  "net" field and back via SDStore::_parse(), which matches on the STRINGS
//  "WIFI" and "SIM", so the spellings below must not change. The integer
//  values are never serialised, only the names.
// ============================================================================
enum NetKind : uint8_t {
  NET_NONE = 0,        // captured with no link — an honest label, not an error
  NET_WIFI = 1,
  NET_SIM  = 2
};

static inline const char* netName(uint8_t k) {
  switch (k) {
    case NET_WIFI: return "WIFI";
    case NET_SIM:  return "SIM";
    default:       return "NONE";
  }
}

// ============================================================================
//  TimeSrc — what set the system clock. v6.1.
//
//  TSRC_NONE MUST BE ZERO. Every GpsRecord is memset() to 0 before it is
//  filled, so a record that never had its source assigned has to read back as
//  "nothing has ever set the clock" rather than as a real source.
//
//  THESE INTEGERS GO ON THE SD CARD as the "tsrc" field and are read back by
//  SDStore::_parse(). Renumbering them re-labels every record already queued
//  on a card. Add new sources at the END; never reorder.
//
//  Authority as of v6.2 is NETWORK (NTP/CELL) > GPS > HOLDOVER > UNSYNCED.
//  That ranking lives in TimeService, not here — this is only the vocabulary.
// ============================================================================
enum TimeSrc : uint8_t {
  TSRC_NONE = 0,       // unsynced: no source has ever set the clock
  TSRC_GPS  = 1,       // satellite time, fallback authority
  TSRC_NTP  = 2,       // SNTP over Wi-Fi, primary authority
  TSRC_CELL = 3,       // carrier NITZ via AT+CCLK?, primary authority
  TSRC_HOLD = 4        // last valid reference, free-running on the crystal
};

// Takes uint8_t rather than TimeSrc on purpose: the firmware calls this both
// with a TimeSrc (timeSrcName(timeSvc.source())) and with the raw record byte
// (timeSrcName(r.tsrc)). One overload serves both without a cast at any site.
static inline const char* timeSrcName(uint8_t s) {
  switch (s) {
    case TSRC_GPS:  return "GPS";
    case TSRC_NTP:  return "NTP";
    case TSRC_CELL: return "CELL";
    case TSRC_HOLD: return "HOLDOVER";
    default:        return "UNSYNCED";
  }
}

// ============================================================================
//  GpsRecord — one sample. The unit of everything: queue payload, SD line,
//  MQTT record. Kept deliberately flat and small (queues hold 128 of them).
// ============================================================================
struct GpsRecord {
  uint32_t seq;        // monotonic, persisted in NVS every SEQ_PERSIST_EVERY
  char     ts[24];     // "YYYY-MM-DD HH:MM:SS" or TS_UNSYNCED, + NUL. 20 used.
  double   lat;        // decimal degrees, 7 dp on the wire
  double   lng;
  float    alt;        // metres MSL
  float    spd;        // km/h
  float    hdg;        // degrees true
  uint16_t sats;
  float    hdop;       // 99.9 when unknown
  uint8_t  batPct;     // 0..100
  uint8_t  net;        // NetKind
  uint8_t  fix;        // 1 = GPS position valid at capture
  uint8_t  tsrc;       // TimeSrc that stamped ts  (v6.1)
};

// ============================================================================
//  SyncEngine states.
//
//  THE INTEGER VALUES ARE PUBLISHED. /api/status emits (int)_sync->status and
//  the dashboard switches on the number, so appending is safe and reordering
//  is not.
// ============================================================================
enum SyncStatus : uint8_t {
  SYNC_IDLE        = 0,
  SYNC_PREPARING   = 1,
  SYNC_UPLOADING   = 2,
  SYNC_WAITING_ACK = 3,
  SYNC_RETRYING    = 4,
  SYNC_COMPLETED   = 5,
  SYNC_FAILED      = 6,
  SYNC_PAUSED      = 7
};

// ============================================================================
//  NetLed — what the YELLOW LED is currently saying. v6.0 LED scheme:
//  RED = battery level, YELLOW = network/transmission, GREEN = GPS quality.
// ============================================================================
enum NetLed : uint8_t {
  NETLED_OFFLINE = 0,  // 1 Hz square blink
  NETLED_LIVE    = 1,  // dark, flashing on each transmission + 10 s heartbeat
  NETLED_BULK    = 2   // 1.2 s triangle ramp while the backlog drains
};

// ============================================================================
//  NetSim — bench hook for the NETSIM console command. Simulates an outage
//  without touching the radio so the offline -> sync -> live path can be
//  exercised deterministically.
// ============================================================================
enum NetSim : uint8_t {
  NETSIM_AUTO      = 0,  // normal operation
  NETSIM_FORCE_ON  = 1,  // pretend the link is up
  NETSIM_FORCE_OFF = 2   // pretend the link is down
};