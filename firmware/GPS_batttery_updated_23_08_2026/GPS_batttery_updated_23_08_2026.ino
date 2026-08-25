// ============================================================================
//  BOTS BANGLA — GPS TRACKER FIRMWARE  v6.0  (PRODUCTION)
//  ESP32 + A7670C/SIM7600 + NEO-8M GPS → MQTT
//
//  v6.0 is a targeted ARCHITECTURAL repair of v5.2, not a rewrite. Pins,
//  broker, topics, payload field names, dashboard, PIN roles, serial
//  commands, watchdog, E2E broker-echo verification and NVS config keys
//  are all preserved. What changed is the parts that were losing data.
//
//  WHY v5.2 LOST OFFLINE DATA — the four architectural root causes:
//
//   R1  PATH CORRUPTION. SDLogger built every path as
//         String(SD_LOG_DIR) + "/" + String(entry.name())
//       On ESP32 Arduino core 2.x, File::name() ALREADY returns the full
//       path ("/logs/off_x.jsonl"), so this produced
//       "/logs//logs/off_x.jsonl". SD.open() on that path returns null.
//       Result: oldestUnsyncedFile() handed the sync engine a path it
//       could never open → readBatch() returned "" → linesRead == 0 →
//       _prepareBatch() concluded "file fully pushed" and DELETED THE
//       FILE WITHOUT EVER TRANSMITTING IT. This is the single biggest
//       source of silent loss. v6.0 canonicalises every path (basename
//       re-anchored under its directory), and a file is NEVER deleted on
//       a read that returned zero records — a zero read is now an error.
//
//   R2  RAM-ONLY SYNC PROGRESS. _lineOffset lived in RAM and reset to 0
//       on reboot AND on every resume()/triggerNow(). A reconnect flap
//       mid-file re-sent every already-ACKed record; a reboot mid-sync
//       did the same. v6.0 persists {file sequence, records ACKed} in
//       NVS after every verified batch.
//
//   R3  UNBOUNDED-SCAN INDEXING. oldestUnsyncedFile(), getStats() and
//       countLines() walked the whole directory / whole files on every
//       10 s idle tick and after every batch, making drain O(n²) and
//       stalling the loop. v6.0 keeps an O(1) in-RAM index (file count,
//       oldest/next sequence, tail record count) reconciled against the
//       filesystem at boot and every 5 minutes.
//
//   R4  SILENT BATCH TRUNCATION. buildPlaybackJSON() stopped emitting
//       records when the 16 KB buffer neared its cap, but the caller
//       advanced the offset by linesRead (records READ), not records
//       SERIALISED. Every truncated batch permanently skipped records.
//       v6.0 computes the record count from the byte budget BEFORE
//       serialising, so truncation is structurally impossible.
//
//  ALSO FIXED
//   R5  Live records were dropped if publish() returned true but the
//       broker echo never came. A live record is now held until its echo
//       verifies; on timeout it is re-queued to the SD FIFO.
//   R6  Sync deleted the file that offline logging was still appending
//       to. The tail file is now SEALED before it can be transmitted.
//   R7  Battery instability: five subsystems each called battery.update()
//       (including HTTP handlers), each triggering 64 blocking ADC reads.
//       Sampling is now owned by ONE task at a fixed cadence; everyone
//       else reads the cached value.
//   R8  Modem fallback could block loop() for >60 s (init 10 s +
//       waitForNetwork 30 s + gprsConnect up to 60 s) and trip the 60 s
//       task watchdog. All modem work now lives on the network task,
//       supervised by a software liveness timer instead.
//
//  ARCHITECTURE (see report section H)
//    Core 1  taskSensor (prio 3)  GPS UART + parse, battery ADC, LEDs,
//                                 record generation, telemetry table
//    Core 1  taskStore  (prio 2)  SOLE owner of the SD FIFO
//    Core 0  taskNet    (prio 2)  WiFi/SIM/MQTT, live publish, sync engine
//    Core 1  loopTask   (prio 1)  dashboard, captive DNS, serial console
//
//  INVARIANT: connectivity is optional; acquisition and preservation are not.
//
//  ---------------------------------------------------------------------
//  v6.1 — TIME SOURCE REPAIR (this revision)
//  ---------------------------------------------------------------------
//  Scope is deliberately narrow: only the clock changed. GPS acquisition,
//  positioning, the SD FIFO, the sync engine, WiFi/cellular management, the
//  dashboard, pins, broker, topics and NVS keys are all untouched.
//
//   T1  WRONG TIME BEFORE GPS LOCK. GPSManager derived the timestamp purely
//       from live TinyGPS++ state and emitted a hardcoded
//       "1970-01-01 00:00:00" whenever that state was not yet populated —
//       i.e. every record from boot until first fix. Replaced by TimeService,
//       a single ranked time authority (GPS > NTP/CELL > holdover > unsynced)
//       that also uses the network, which the device usually has first.
//
//   T2  FROZEN CLOCK AFTER FIX LOSS. TinyGPSDate/TinyGPSTime::valid latch
//       true on first commit and are never cleared, so after a fix was lost
//       the old code kept re-emitting the last parsed instant indefinitely —
//       plausible-looking and undetectable. TimeService checks age(), refuses
//       stale GPS, and projects forward from the last good reference instead.
//
//   T3  BUDGET. The two new wire fields push a backend record from 427 to
//       468 bytes, over the old REC_JSON_MAX of 448, which would have
//       silently re-armed root cause R4. REC_JSON_MAX is now 496 and
//       PAYLOAD_CAP 15360, holding the batch at 30 records. REC_LINE_MAX
//       200 -> 224 for the "tsrc" field, with a truncation guard on append().
//
//  WIRE CHANGES (additive only, existing field names untouched):
//    live + bulk records gain  "time_valid": true|false
//                              "time_src":   GPS|NTP|CELL|HOLDOVER|UNSYNCED
//    SD JSONL records gain     "tsrc": 0..4
//    Unsynced records carry ts "0000-00-00 00:00:00" — an impossible instant,
//    so no consumer can silently plot it the way 1970-01-01 was plotted.
//
//  ---------------------------------------------------------------------
//  v6.2 — TIME AUTHORITY INVERTED: NETWORK FIRST (this revision)
//  ---------------------------------------------------------------------
//  v6.1 built the right machine and pointed it the wrong way. It ranked
//
//      GPS  >  NTP/CELL  >  HOLDOVER  >  UNSYNCED
//
//  on the reasoning that GPS is ground truth. It is — when there is a fix.
//  On this deployment there very often is not: the unit lives in a bag, a
//  pocket, a basement car park, or under a corrugated roof, while the SIM
//  and the Wi-Fi stay up throughout. Ranking the sometimes-absent source
//  above the almost-always-present one made the clock hostage to the sky.
//  v6.2 inverts it to
//
//      NETWORK (NTP/CELL)  >  GPS  >  HOLDOVER  >  UNSYNCED
//
//  Three code paths implemented the old ranking and all three are changed:
//
//   T4  ingestGps() called _setRef() UNCONDITIONALLY, four times a second.
//       It never asked what had set the clock, so a single NMEA sentence
//       silently overwrote a good NTP reference — and kept overwriting it.
//       It now refuses authority while the effective source is a fresh
//       network reference, and measures the disagreement instead. GPS
//       parsing, positioning and every other GPS field are untouched.
//
//   T5  ingestNetwork() REFUSED a network value outright whenever the
//       current reference was GPS younger than GPS_AUTHORITY_HOLD_S (1 h).
//       A bad GPS instant that cleared the sanity floor therefore owned the
//       clock for a full hour with a correct NTP answer sitting in hand.
//       The refusal is gone: the network is now never refused for authority.
//
//   T6  netSyncTick() SKIPPED the sync entirely while source() == TSRC_GPS,
//       so on a unit with a fix the network was not merely outranked, it was
//       never asked. That early return is gone. Cadence is unchanged
//       (TIME_REFRESH_MS on success, TIME_RETRY_MS while failing), so this
//       does not increase traffic: four SNTP exchanges a day over Wi-Fi, and
//       nothing at all over cellular, where AT+CCLK? is a local AT query
//       against the NITZ value the modem already holds.
//
//  Fallback is driven by the freshness predicate that source() ALREADY
//  implemented — a network reference older than NET_FRESH_S, or any tick
//  with no transport up (_netStarved), reads as HOLDOVER. GPS is admitted
//  the moment that happens and steps back down when the network returns, so
//  the handoff needs no new state machine, no new timer and no new constant.
//
//  RETIRED: GPS_AUTHORITY_HOLD_S. Under network-first authority there is no
//  window in which GPS may refuse a network correction.
//
//  NOT CHANGED by v6.2: the timestamp format ("YYYY-MM-DD HH:MM:SS", UTC+6
//  applied only in format()), the wire schema, "tsrc" on SD, the SD FIFO,
//  the sync engine, GPS acquisition and parsing, MQTT, pins, NVS keys.
//
//  ---------------------------------------------------------------------
//  BUILD NOTE — dashboard_html.h
//  ---------------------------------------------------------------------
//  v5.2 had the dashboard pasted inline. v6.0 expects it in
//  dashboard_html.h in the same sketch folder, wrapped exactly as before:
//
//      #pragma once
//      const char DASHBOARD_HTML[] PROGMEM = R"HTMLEOF( ... )HTMLEOF";
//
//  Move your existing HTML there unchanged, then apply the four small
//  additions in DASHBOARD_PATCH.md (new Sync-card rows). Do not remove the
//  raw-string wrapper — that is what fixed the "#map invalid preprocessing
//  directive" and "extended character" compile errors.
// ============================================================================

#define FW_VERSION "6.2"

// ---------------------------------------------------------- FIRMWARE IDENTITY
// Two independent gates decide whether stored state may be trusted.
//
//   CONFIG_SCHEMA_VERSION — bump this BY HAND whenever the shape or meaning of
//   anything in NVS changes: a key added or removed, a default whose semantics
//   moved, a value range that narrowed. It is the contract between a stored
//   image and the code that reads it. A mismatch is always a hard reset,
//   regardless of policy below, because an old layout silently driving new
//   code is the failure this whole mechanism exists to prevent.
//
//   FW_BUILD_ID — the compiler's own timestamp for this translation unit. It
//   changes on every recompile of the sketch and therefore identifies the
//   binary itself, not just its declared schema. This is what catches "I
//   uploaded new firmware" when the schema number did not move.
//
// __DATE__/__TIME__ are fixed at compile time and are already used in this
// file for TIME_BUILD_YEAR, so no new dependency is introduced. Note that a
// re-upload of a byte-identical cached build keeps the same ID and is
// correctly treated as the same firmware.
#define CONFIG_SCHEMA_VERSION   8
#define FW_BUILD_ID             (__DATE__ " " __TIME__)

// POLICY. 1 = any new binary provisions a clean application state, which is
// what you want on the bench while the firmware is still moving.
// 0 = only a CONFIG_SCHEMA_VERSION bump resets; a same-schema build keeps the
// deployed configuration. Switch to 0 before shipping OTA updates to units in
// the field, or a routine bugfix will wipe the customer's broker, APN and
// PINs. This is the one decision in the mechanism that is a judgement call
// rather than a correctness question, so it is a single line.
#define FRESH_START_ON_NEW_BUILD 1

#define DEBUG_MODE              1      // 0 = silence DBG_*; telemetry stays
#define TINY_GSM_MODEM_SIM7600

// A7670C/SIM7600 cellular fallback. 0 = Wi-Fi-only build.
#define ENABLE_MODEM_FUNCTIONALITY 1

// TP4056 CHRG/STDBY sensing on GPIO35/39. 0 if not wired (floating pins
// give random charge readings). Charge state is reported on the dashboard
// and serial only — in v6.0 the RED LED shows BATTERY LEVEL, not charging.
#define ENABLE_CHARGE_SENSE     1

// Keep delivered batches in /LOGS/SENT instead of deleting them, so the
// dashboard Playback tab still has history. They are the first thing
// evicted when the card fills. 0 = delete on ACK (v5.2 behaviour).
#define KEEP_SENT_ARCHIVE       1

// Bangladesh Standard Time. Fixed, no DST, never changes. ALL internal
// timekeeping is UTC; this offset is applied at exactly one place, in
// TimeService::format(). Do not pre-shift anywhere else.
#define TZ_OFFSET_SECONDS       (6L * 3600L)

// ---------------------------------------------------------------- TIME SOURCE
// v6.1. NTP servers used when Wi-Fi is the active transport. Anycast hosts are
// preferred over country pools: fewer DNS surprises on a captive-portal ISP.
// Change these if the deployment sits behind a firewall with its own NTP.
#define NTP_SERVER_1            "time.google.com"
#define NTP_SERVER_2            "pool.ntp.org"
#define NTP_SERVER_3            "time.cloudflare.com"

// Clock sanity floor, derived from the build date. Any date before the day
// this binary was compiled is impossible and is refused from every source.
// This is also what catches the NEO-8M GPS week-number rollover, which
// reports a date ~19.6 years in the past with every validity flag set.
#define TIME_BUILD_YEAR   ((__DATE__[7] - '0') * 1000 + (__DATE__[8] - '0') * 100 + \
                           (__DATE__[9] - '0') * 10   + (__DATE__[10] - '0'))

#define GPS_TIME_MAX_AGE_MS     2500UL          // older NMEA time = refused
#define GPS_FRESH_S             30UL            // beyond this, GPS ref is holdover
#define NET_FRESH_S             (7UL * 3600UL)  // beyond this, NTP ref is holdover

// RETIRED in v6.2 — deliberately left as a tombstone so an upgrade diff against
// v6.1 does not read as an accidental deletion. It bounded how long a GPS
// reference was allowed to refuse a network correction. Under network-first
// authority no such window exists: the network is never refused.
//   #define GPS_AUTHORITY_HOLD_S  3600UL
//
// NET_FRESH_S is now doing double duty and is the ONLY freshness knob in the
// authority decision. It defines, via source(), both when a network reference
// stops being reported as NETWORK *and* when GPS is allowed to take the clock
// back. One constant, one definition of "fresh", so the two can never disagree.

#define NTP_POLL_WINDOW_MS      15000UL         // non-blocking wait for SNTP
#define TIME_RETRY_MS           60000UL         // retry cadence while unsynced
#define TIME_REFRESH_MS         (6UL * 3600UL * 1000UL)   // drift re-sync

// v6.2. ingestGps() runs at 4 Hz. When the network holds authority every one
// of those calls is a refusal, so the GPS-vs-network drift line is throttled
// to this. 10 min gives ~144 samples a day — enough to see a constellation
// disagreeing, nowhere near enough to flood a console someone is typing into.
#define GPS_DRIFT_LOG_MS        600000UL

// What a record looks like on the wire when nothing has ever set the clock.
// Month 00 / day 00 is not a representable instant in any calendar, so no
// consumer can silently plot it — unlike 1970-01-01, which is a real date.
#define TS_UNSYNCED             "0000-00-00 00:00:00"

#include <Arduino.h>
#include <TinyGPS++.h>
#include <TinyGsmClient.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <FS.h>
#include <time.h>
#include <math.h>
#include <algorithm>
#include <esp_timer.h>          // monotonic us clock for holdover projection
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "tracker_types.h"   // GpsRecord, NetKind, SyncStatus, NetLed, NetSim

// Credentials and per-deployment identity. Untracked; copy secrets.example.h
// to secrets.h and fill it in. Supplies FIRST-BOOT DEFAULTS only — a
// provisioned device reads NVS and never looks at these.
#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #error "secrets.h not found. Run: cp secrets.example.h secrets.h  then edit it."
#endif

// ---------------------------------------------------------- PROVISIONING TYPE
// This sits above the FIRST function definition in the sketch rather than
// down beside provisionPersistentState(), where it would read more naturally.
//
// The Arduino IDE runs a ctags pass over the .ino, generates a prototype for
// every function it finds, and injects the whole set immediately before the
// first function definition -- sPrintf(), a few dozen lines below. A generated
// prototype naming a type declared later in the file cannot compile, and the
// error is reported against the ORIGINAL function line, which makes it look as
// though the definition is at fault rather than the injected copy. Every other
// custom type used in a signature here (GpsRecord, SyncStatus, NetLed) is
// above this point for exactly the same reason.
//
// Do not move this back down next to the function that uses it.
#define PROV_NS        "sysprov"    // owned exclusively by the provisioner
#define PROV_K_SCHEMA  "schema"
#define PROV_K_BUILD   "build"
#define PROV_K_RESETS  "resets"

// Battery calibration record. Same placement rule as ProvisionResult below:
// bcalLoad(), bcalSave(), _bcalCrc() and _bcalPlausible() all take or return
// it, so the generated prototypes for those functions - injected before
// sPrintf() - must be able to see the type. Keep it here.
struct BatteryCal {
  float ref    = 3.720f;    // the reference voltage that produced this solve
  float gain   = 1.05472f;
  float offset = 0.0f;
  float p1True = 0.0f;    // two-point in progress (0 = none pending)
  float p1Raw  = 0.0f;
  bool  valid  = false;
};

struct ProvisionResult {
  bool        wiped        = false;
  bool        nvsOk        = true;
  uint32_t    storedSchema = 0;
  String      storedBuild  = "";
  const char* reason       = "state matches this firmware — nothing cleared";
  float       keptBattCal  = 1.0f;
  float       keptBattOff  = 0.0f;
  bool        keptCalPoint = false;
  uint32_t    keptRecSeq   = 0;
  uint32_t    resetCount   = 0;
};

// ================================================================ PINS
#define GPS_RX          16
#define GPS_TX          17
#define MODEM_RX        27
#define MODEM_TX        26
#define PWRKEY           4
#define FORCE_SETUP_PIN  0

#define SD_MISO         19
#define SD_MOSI         23
#define SD_SCK          18
#define SD_CS            5

// LEDs — v6.0 scheme (report section G)
#define LED_RED_PIN     25    // battery level      (PWM, brighter = lower)
#define LED_YELLOW_PIN  33    // network/transmission (was "status" in v5.2)
#define LED_GREEN_PIN   32    // GPS quality        (PWM, brighter = better)
#define CHRG_SENSE_PIN  35    // TP4056 CHRG  (active LOW, external 10k pull-up)
#define FULL_SENSE_PIN  39    // TP4056 STDBY (active LOW, external 10k pull-up)

#define BATT_ADC_PIN    34

// ================================================================ COMPAT
// LEDC API changed between ESP32 Arduino core 2.x and 3.x.
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  #define PWM_ATTACH(pin, ch, freq, res) ledcAttach((pin), (freq), (res))
  #define PWM_WRITE(pin, ch, duty)       ledcWrite((pin), (duty))
#else
  #define PWM_ATTACH(pin, ch, freq, res) do { ledcSetup((ch), (freq), (res)); \
                                              ledcAttachPin((pin), (ch)); } while (0)
  #define PWM_WRITE(pin, ch, duty)       ledcWrite((ch), (duty))
#endif

#define PWM_FREQ_HZ     5000
#define PWM_BITS        12
#define PWM_MAX         4095
#define PWM_CH_RED      0
#define PWM_CH_YELLOW   1
#define PWM_CH_GREEN    2

// ================================================================ SERIAL
// Serial is shared by four tasks. Every print goes through sPrintf so a
// telemetry row can never be spliced into the middle of a console reply.
static SemaphoreHandle_t serialMutex = nullptr;

static void sPrintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void sPrintf(const char* fmt, ...) {
  static char line[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (serialMutex) xSemaphoreTake(serialMutex, pdMS_TO_TICKS(200));
  Serial.print(line);
  if (serialMutex) xSemaphoreGive(serialMutex);
}

#if DEBUG_MODE
  #define DBG(tag, fmt, ...)  sPrintf("[" tag "] " fmt "\n", ##__VA_ARGS__)
#else
  #define DBG(tag, fmt, ...)
#endif
#define LOGW(tag, fmt, ...)   sPrintf("[" tag "] " fmt "\n", ##__VA_ARGS__)  // always on

// ================================================================ WATCHDOG
#define WDT_TIMEOUT_S   60

static inline void wdtFeed() { esp_task_wdt_reset(); }

static void wdtInit() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  esp_task_wdt_config_t wcfg = {
    .timeout_ms     = WDT_TIMEOUT_S * 1000,
    .idle_core_mask = 0,
    .trigger_panic  = true
  };
  esp_task_wdt_reconfigure(&wcfg);   // core 3.x: WDT already initialised
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(NULL);            // loop task
  DBG("WDT", "Task watchdog armed: %ds, panic+reboot on stall", WDT_TIMEOUT_S);
}

// ============================================================================
//  BOOT PROVISIONING — clean application state on a new firmware image.
//
//  WHAT ACTUALLY SURVIVES A FIRMWARE UPLOAD ON THIS BOARD. Flashing the app
//  partition rewrites code only. Everything below outlives it:
//
//    NVS "tracker"  17 keys — wssid wpass broker bport topic devid apn intv
//                   appass bsize maxstore autodel devpin userpin bcal ackmode
//                   pblive. Written by ConfigManager::save(), read by load()
//                   with per-key defaults. THIS is the dangerous one: a key
//                   written by an older firmware wins over the new default
//                   silently, because getString(k, default) prefers the store.
//    NVS "fifo"     recseq (monotonic record sequence), syncSeq + syncOff
//                   (how far into the current batch file the broker has
//                   acknowledged).
//    SD /LOGS       QUEUE/*.jsonl and SENT/*.jsonl — undelivered and archived
//                   field data.
//    esp_wifi NVS   nvs.net80211 may hold credentials from firmware that ran
//                   before WiFi.persistent(false). Inert here: this firmware
//                   only ever calls WiFi.begin(ssid, pass) with explicit
//                   arguments and never the no-argument overload, so a stored
//                   entry can never influence which network is joined.
//    RF calibration partition — per-chip radio trim. Never application data.
//
//  NOT USED BY THIS PROJECT, verified by inspection rather than assumption:
//  EEPROM, LittleFS, SPIFFS, RTC_DATA_ATTR / RTC_NOINIT_ATTR retained memory,
//  esp_partition or nvs_flash direct access, and any config file on the SD
//  card. SD holds records only; there is no settings file on it.
//
//  WHAT THIS FUNCTION DOES. Runs before ConfigManager::load(), before
//  SDStore::begin() opens "fifo", before any peripheral is touched — so no
//  subsystem can have read stale state by the time the decision is made.
//  On a reset it wipes "tracker" wholesale and drops the two sync-progress
//  keys, then setup() calls load() (which now sees an empty namespace and
//  returns this firmware's defaults for every field) followed by save() to
//  materialise them.
//
//  WHAT IT DELIBERATELY KEEPS, and why each is not firmware state:
//
//    bcal   — the battery divider correction from VCAL, measured against a
//             multimeter on THIS board's 100k/100k resistors. A property of
//             the hardware, like RF trim. Wiping it silently degrades every
//             battery reading and the only recovery is a physical multimeter,
//             which a field unit does not have.
//    recseq — the monotonic record sequence. Backends deduplicate on it. Reset
//             it and the next batch replays sequence numbers that were already
//             consumed, which corrupts the far end rather than this device.
//             (SDStore::_reconcileSeqFromDisk() would recover it from the card
//             anyway, so clearing it buys nothing and risks a gap.)
//    /LOGS  — undelivered customer data. A firmware upload is not consent to
//             destroy it. Old records are already forward-compatible:
//             SDStore::_parse() explicitly migrates pre-v6.1 lines that have
//             no "tsrc" field and normalises the legacy 1970 sentinel.
//
//  WHAT IT REFUSES TO TOUCH. Bootloader, partition table, otadata, NVS
//  encryption keys and the RF calibration partition. Those are ESP32
//  infrastructure; erasing them turns a config problem into a bricked board.
//  The goal is a clean APPLICATION state, not a scorched flash.
// ============================================================================

// PROV_* and struct ProvisionResult are declared up with the includes, not
// here. See the note there: the Arduino IDE injects generated prototypes
// ahead of the first function definition in the sketch, so any type used in
// a function signature must already be visible at that point.

static ProvisionResult provisionPersistentState() {
  ProvisionResult r;

  Preferences prov;
  if (!prov.begin(PROV_NS, false)) {
    // Do NOT fall through to a wipe. An unreadable provisioning record is not
    // evidence that the config is stale, and destroying good configuration on
    // the strength of a failed open would be the worst possible reading of
    // an ambiguous signal.
    r.nvsOk  = false;
    r.reason = "NVS UNAVAILABLE — version check skipped, stored state kept";
    return r;
  }

  r.storedSchema = prov.getUInt  (PROV_K_SCHEMA, 0);
  r.storedBuild  = prov.getString(PROV_K_BUILD,  "");
  r.resetCount   = prov.getUInt  (PROV_K_RESETS, 0);

  const bool firstBoot     = (r.storedSchema == 0 && r.storedBuild.length() == 0);
  const bool schemaChanged = (r.storedSchema != (uint32_t)CONFIG_SCHEMA_VERSION);
  const bool buildChanged  = (r.storedBuild  != String(FW_BUILD_ID));

  if      (firstBoot)     { r.wiped = true;  r.reason = "FIRST BOOT — no provisioning record on this device"; }
  else if (schemaChanged) { r.wiped = true;  r.reason = "SCHEMA CHANGED — stored layout is incompatible with this firmware"; }
#if FRESH_START_ON_NEW_BUILD
  else if (buildChanged)  { r.wiped = true;  r.reason = "NEW BUILD — different firmware image, clean start by policy"; }
#else
  else if (buildChanged)  { r.wiped = false; r.reason = "NEW BUILD, schema unchanged — configuration preserved by policy"; }
#endif

  if (r.wiped) {
    // ---- survivors read out BEFORE anything is destroyed -------------------
    float p1t = 0.0f, p1r = 0.0f;
    { Preferences t;
      if (t.begin("tracker", true)) {
        r.keptBattCal = t.getFloat("bcal",  1.0f);
        r.keptBattOff = t.getFloat("bcalo", 0.0f);
        p1t = t.getFloat("bcp1t", 0.0f);
        p1r = t.getFloat("bcp1r", 0.0f);
        r.keptCalPoint = (p1t > 2.5f);
        t.end();
      } }
    { Preferences f;
      if (f.begin("fifo", true))    { r.keptRecSeq  = f.getULong("recseq", 0);  f.end(); } }

    // ---- application configuration: wipe whole, restore the one survivor ---
    // clear() removes every key including any left behind by a firmware that
    // used names this build no longer knows about. That is the point: an
    // orphaned key is exactly the kind of state that survives an upload and
    // has nobody left to validate it.
    { Preferences t;
      if (t.begin("tracker", false)) {
        t.clear();
        t.putFloat("bcal",  r.keptBattCal);
        t.putFloat("bcalo", r.keptBattOff);
        t.putFloat("bcp1t", p1t);
        t.putFloat("bcp1r", p1r);
        t.end();
      } else { r.nvsOk = false; }
    }

    // ---- FIFO namespace: sync progress only, recseq untouched -------------
    // syncSeq/syncOff say "the broker has already acknowledged the first N
    // records of batch file S". After a firmware change that claim is no
    // longer trustworthy, and the two errors are not symmetric: resuming too
    // early re-sends records the backend deduplicates, resuming too late
    // skips records forever. Reset to 0 and re-send.
    { Preferences f;
      if (f.begin("fifo", false)) {
        f.remove("syncSeq");
        f.remove("syncOff");
        f.end();
      } else { r.nvsOk = false; }
    }

    r.resetCount++;
  }

  prov.putUInt  (PROV_K_SCHEMA, (uint32_t)CONFIG_SCHEMA_VERSION);
  prov.putString(PROV_K_BUILD,  FW_BUILD_ID);
  prov.putUInt  (PROV_K_RESETS, r.resetCount);
  prov.end();
  return r;
}

static void logProvisioning(const ProvisionResult& r) {
  sPrintf("---------- BOOT PROVISIONING ----------\n");
  sPrintf("Firmware        : v%s\n", FW_VERSION);
  sPrintf("Build ID        : %s\n", FW_BUILD_ID);
  sPrintf("Config schema   : %d (this firmware)\n", CONFIG_SCHEMA_VERSION);
  sPrintf("Stored schema   : %lu%s\n", (unsigned long)r.storedSchema,
          r.storedSchema == 0 ? "  (absent)" : "");
  sPrintf("Stored build ID : %s\n",
          r.storedBuild.length() ? r.storedBuild.c_str() : "(absent)");
  sPrintf("New-build policy: %s\n",
          FRESH_START_ON_NEW_BUILD ? "clean start on ANY new binary"
                                   : "clean start only on a schema bump");
  sPrintf("Decision        : %s\n", r.reason);

  if (r.wiped) {
    sPrintf("Cleared         : NVS 'tracker' — all 17 config keys; defaults now "
            "come from this firmware\n");
    sPrintf("Cleared         : NVS 'fifo' syncSeq + syncOff — batch sync restarts "
            "at record 0 of the oldest pending file\n");
    sPrintf("Preserved       : NVS 'tracker' bcal = %.4f, bcalo = %+.4f V "
            "(per-board battery calibration)\n", r.keptBattCal, r.keptBattOff);
    if (r.keptCalPoint)
      sPrintf("Preserved       : NVS 'tracker' bcp1t/bcp1r (two-point "
              "calibration in progress)\n");
    sPrintf("Preserved       : NVS 'fifo' recseq = %lu (monotonic record "
            "sequence; backend dedupes on it)\n", (unsigned long)r.keptRecSeq);
    sPrintf("Preserved       : SD /LOGS — undelivered field data, parser "
            "self-migrates older records\n");
    sPrintf("Untouched       : bootloader, partition table, otadata, RF "
            "calibration\n");
    sPrintf("Clean starts    : %lu on this device\n", (unsigned long)r.resetCount);
  }

  sPrintf("Status          : %s\n", r.nvsOk
          ? (r.wiped ? "PROVISIONED — application state rebuilt from this firmware"
                     : "OK — persistent state belongs to this firmware")
          : "DEGRADED — an NVS operation failed, see lines above");
  sPrintf("---------------------------------------\n");
}

// Forward declaration only. BatteryCal and its store are defined further down
// with the rest of the battery subsystem, but ConfigManager::factoryReset()
// below needs to erase the calibration namespace, and C++ requires the
// declaration to precede that use.
static void bcalErase();

// ============================================================================
//  ConfigManager — NVS keys unchanged from v5.2 so existing devices keep
//  their configuration across the upgrade. Two keys added.
// ============================================================================
struct DeviceConfig {
  String wifiSsid, wifiPass, broker, topic, deviceId, apn, apPass;
  String devPin, userPin;
  int    port, intervalMs, batchSize, maxStoragePct;
  bool   autoDelete;
  // Battery calibration deliberately does NOT live here. It is per-board
  // hardware calibration, not application configuration, and it is kept in
  // its own NVS namespace so a config wipe cannot take it with it.
  // See BatteryCal / BCAL_NS.
  int    ackMode;        // 0 = broker echo (default), 1 = backend ACK topic
  bool   pbOnLiveTopic;  // false = bulk to <topic>/playback (v5.2 contract)
};

class ConfigManager {
public:
  DeviceConfig cfg;

  void load() {
    Preferences p;
    p.begin("tracker", true);
    cfg.wifiSsid      = p.getString("wssid",   DEFAULT_WIFI_SSID);
    cfg.wifiPass      = p.getString("wpass",   DEFAULT_WIFI_PASS);
    cfg.broker        = p.getString("broker",  "broker.hivemq.com");
    cfg.port          = p.getInt   ("bport",   1883);
    cfg.topic         = p.getString("topic",   DEFAULT_MQTT_TOPIC);
    cfg.deviceId      = p.getString("devid",   DEFAULT_DEVICE_ID);
    cfg.apn           = p.getString("apn",     "gpinternet");
    cfg.intervalMs    = p.getInt   ("intv",    5000);
    cfg.apPass        = p.getString("appass",  DEFAULT_AP_PASS);
    cfg.batchSize     = p.getInt   ("bsize",   30);
    cfg.maxStoragePct = p.getInt   ("maxstore",80);
    cfg.autoDelete    = p.getBool  ("autodel", true);
    cfg.devPin        = p.getString("devpin",  DEFAULT_DEV_PIN);
    cfg.userPin       = p.getString("userpin", DEFAULT_USER_PIN);
    cfg.ackMode       = p.getInt   ("ackmode", 0);
    cfg.pbOnLiveTopic = p.getBool  ("pblive",  false);
    p.end();

    // Hard clamp: one file = one MQTT batch, and the batch must fit the
    // payload budget with zero truncation. See PAYLOAD_CAP.
    if (cfg.batchSize < 1)  cfg.batchSize = 1;
    if (cfg.batchSize > 30) cfg.batchSize = 30;
    if (cfg.intervalMs < 1000) cfg.intervalMs = 1000;

    DBG("BOOT", "Config: dev %s | %s:%d | live '%s' | bulk '%s' | batch %d",
        cfg.deviceId.c_str(), cfg.broker.c_str(), cfg.port,
        cfg.topic.c_str(), bulkTopic().c_str(), cfg.batchSize);
  }

  void save() {
    Preferences p;
    p.begin("tracker", false);
    p.putString("wssid",    cfg.wifiSsid);
    p.putString("wpass",    cfg.wifiPass);
    p.putString("broker",   cfg.broker);
    p.putInt   ("bport",    cfg.port);
    p.putString("topic",    cfg.topic);
    p.putString("devid",    cfg.deviceId);
    p.putString("apn",      cfg.apn);
    p.putInt   ("intv",     cfg.intervalMs);
    p.putString("appass",   cfg.apPass);
    p.putInt   ("bsize",    cfg.batchSize);
    p.putInt   ("maxstore", cfg.maxStoragePct);
    p.putBool  ("autodel",  cfg.autoDelete);
    p.putString("devpin",   cfg.devPin);
    p.putString("userpin",  cfg.userPin);
    p.putInt   ("ackmode",  cfg.ackMode);
    p.putBool  ("pblive",   cfg.pbOnLiveTopic);
    p.end();
    DBG("BOOT", "Configuration saved");
  }

  void factoryReset() {
    Preferences p;
    p.begin("tracker", false); p.clear(); p.end();
    // Sync progress and the sequence counter live in their own namespace.
    p.begin("fifo", false);    p.clear(); p.end();
    // Explicit operator action, so the per-board calibration goes too. This is
    // the ONLY path that erases it.
    bcalErase();
    LOGW("BOOT", "FACTORY RESET — all preferences AND battery calibration cleared");
  }

  // Bulk/offline batches. Default keeps the v5.2 contract (<topic>/playback).
  String bulkTopic() const {
    return cfg.topic;  // Always use the same topic for both live and playback (modified for one singlw topic for sending data) || (separate topic ) return cfg.pbOnLiveTopic ? cfg.topic : (cfg.topic + "/playback");
  }
  String diagTopic() const { return cfg.topic + "/diag"; }
  String ackTopic()  const { return cfg.topic + "/ack";  }

  static String jsonEsc(const String& s) {
    String o; o.reserve(s.length() + 16);
    for (size_t i = 0; i < s.length(); i++) {
      char c = s[i];
      if (c == '"' || c == '\\') { o += '\\'; o += c; }
      else if (c == '\n') o += "\\n";
      else o += c;
    }
    return o;
  }
};

// ============================================================================
//  BatteryMonitor  — report section F
//
//  v5.2 instability had three causes, only one of which was filtering:
//   (a) FIVE call sites invoked update() — loop(), /api/basic, /api/config,
//       /api/status, the self-test — each triggering 64 blocking ADC reads
//       at unpredictable times, so the moving average was fed at a random
//       rate and the displayed value depended on who asked last.
//   (b) The 100k/100k divider presents ~50 kΩ source impedance to the SAR
//       ADC, which needs <10 kΩ to charge its sample-and-hold capacitor.
//       The first reads after a channel switch are therefore contaminated
//       by the previously converted channel. THIS IS A HARDWARE LIMIT —
//       firmware can only mitigate it (see remaining risks).
//   (c) Percentage was recomputed from a jittering voltage with no
//       hysteresis, so 1 LSB of noise flipped whole percentage points.
//
//  v6.0 pipeline, owned by taskSensor alone, at a fixed 250 ms cadence:
//    2 dummy reads (charge S/H)  →  21 samples  →  median  →  MAD outlier
//    rejection  →  mean of survivors  →  divider + calibration  →  slew
//    limit  →  EMA (τ≈3 s)  →  curve  →  percent hysteresis
//  Everything else (dashboard, LEDs, serial, records) reads the cached
//  value. Nothing else touches the ADC.
// ============================================================================

// ============================================================================
//  BATTERY CALIBRATION  -  change ONE value here, upload once, done.
//
//  PROCEDURE
//    1. Connect the battery and let it rest (no charging, modem idle).
//    2. Measure the battery terminals with a trusted multimeter.
//    3. Put that reading in BATTERY_CAL_REFERENCE_VOLTAGE below.
//    4. Upload the firmware ONCE.
//    5. On the first boot the firmware measures its own uncalibrated voltage,
//       solves the correction against your reference, and writes it to NVS.
//       Every later boot loads it. You never enter it again.
//
//  Leave the value in place afterwards. It is compared against the reference
//  stored in NVS, so an unchanged value means "already calibrated, do nothing"
//  - the device will NOT recalibrate itself on later boots against whatever
//  charge state the pack happens to be in.
//
//  To recalibrate at the SAME voltage as before, run VCAL RESET on the serial
//  console and reboot. To disable automatic calibration entirely, set this to
//  0.0f and use the VCAL console commands instead.
// ============================================================================
#define BATTERY_CAL_REFERENCE_VOLTAGE   3.720f     // <-- YOUR MULTIMETER READING

// Set to 1 to print the full measurement chain at boot. Off in production.
#define BATTERY_DEBUG                   0

// ---- physical divider. Measure the fitted resistors and correct these if you
// ---- ever want to remove tolerance from the gain term rather than absorb it.
#define BATT_R1            100000.0f
#define BATT_R2            100000.0f

// ---- pack limits. Samsung EB-BG991ABY (Galaxy S21): 3.86 V nominal, 4.40 V
// ---- charge voltage. NOT a 4.20 V cell - see BATT_CURVE below.
#define BATT_V_FULL        4.40f
#define BATT_V_NOMINAL     3.86f
#define BATT_V_EMPTY       3.20f

// ---- sanity window for a single ADC-derived sample. Anything outside this is
// ---- a broken read, not a battery state, and is discarded rather than
// ---- allowed to drag the filter to 0% or 100%.
#define BATT_V_SANE_MIN    1.00f
#define BATT_V_SANE_MAX    5.00f

// ---- persistent calibration store: own NVS namespace, never touched by the
// ---- boot provisioner, only erased by an explicit factory reset.
#define BCAL_NS            "battcal"
#define BCAL_MAGIC         0xBA77CA10UL
#define BCAL_VERSION       1
#define BATT_BURST         21          // odd → true median
#define BATT_SETTLE_US     200
#define BATT_SAMPLE_MS     250UL
#define BATT_EMA_ALPHA     0.08f       // ≈3 s time constant at 4 Hz
#define BATT_MAX_SLEW_V    0.030f      // reject >30 mV jumps per sample
#define BATT_PCT_HYST      0.7f        // percent must move this far to change

struct BattPoint { float v; int pct; };
// Discharge curve for a 4.40 V-charge Li-ion (Samsung EB-BG991ABY class),
// resting terminal voltage -> state of charge. The previous table topped out
// at 4.20 V = 100%, which for this pack clips the entire top ~35% of usable
// charge: a full S21 cell sits at 4.40 V and would have read 100% from 4.20 V
// upward while still holding a third of its energy above that point.
//
// PROVENANCE: these points are an estimate for a 4.4 V high-voltage LCO cell,
// anchored on the published 3.86 V nominal / 4.40 V charge figures. They are
// NOT measured from your pack. To replace them with real data, discharge the
// pack at a representative load, log resting voltage against coulomb count,
// and edit this table - nothing else in the firmware needs to change, because
// voltage measurement and percentage estimation are independent systems.
static const BattPoint BATT_CURVE[] = {
  {4.40f, 100}, {4.30f, 92}, {4.20f, 84}, {4.10f, 75}, {4.00f, 66},
  {3.90f, 56},  {3.85f, 50}, {3.80f, 44}, {3.75f, 37}, {3.70f, 30},
  {3.65f, 24},  {3.60f, 18}, {3.55f, 13}, {3.50f, 9},  {3.45f, 6},
  {3.40f, 4},   {3.30f, 2},  {3.20f, 0}
};

#define BATT_CAL_MIN_SPAN_V 0.40f
static const int BATT_CURVE_LEN = sizeof(BATT_CURVE) / sizeof(BATT_CURVE[0]);

class BatteryMonitor {
public:
  // Correction model:  V_batt = V_uncalibrated * gain + offset
  // gain   covers resistor tolerance, ADC reference error and source-impedance
  //        loss - every cause that scales with the reading.
  // offset covers a fixed drop in the hardware path (protection FET, series
  //        diode) - which does not scale. Defaults to 0, so a gain-only
  //        calibration behaves exactly as it did before.
  void setCalibration(float g, float o = 0.0f) {
    _cal       = (g > 0.2f  && g < 5.0f) ? g : 1.0f;
    _calOffset = (o > -1.0f && o < 1.0f) ? o : 0.0f;
  }
  float getCalibration()       const { return _cal; }
  float getCalibrationOffset() const { return _calOffset; }

  // Diagnostics. rawPinMillivolts() is the number to compare against a DMM
  // reading taken at GPIO34 - that comparison is what separates an ADC error
  // from a divider error.
  float rawPinMillivolts()  { return _rawPinMv(); }
  uint16_t rawAdcCounts()   { return (uint16_t)analogRead(BATT_ADC_PIN); }

  // Averaged uncalibrated reading, used only when solving a calibration.
  // A calibration is a one-shot decision that then governs every later
  // reading, so it is worth spending 8 bursts (~170 samples) on it rather
  // than trusting the single burst a normal tick uses.
  float measureUncalibrated(uint8_t bursts = 8) {
    double acc = 0; uint8_t n = 0;
    for (uint8_t i = 0; i < bursts; i++) { acc += _rawUncalVolts(); n++; delay(20); }
    return n ? (float)(acc / n) : _rawUncalVolts();
  }
  uint32_t rejectedSamples() const { return _rejected; }
  float uncalibratedVolts() const {
    return (_cal > 0.001f) ? ((_volts - _calOffset) / _cal) : _volts;
  }

  void begin() {
    analogReadResolution(12);
    analogSetPinAttenuation(BATT_ADC_PIN, ADC_11db);
#if ENABLE_CHARGE_SENSE
    pinMode(CHRG_SENSE_PIN, INPUT);
    pinMode(FULL_SENSE_PIN, INPUT);
#endif
    float v = _rawVolts();
    _ema = v; _volts = v;
    _pctF = (float)_voltsToPercent(v);
    _pct  = (int)(_pctF + 0.5f);
    _lastSample = millis();
    DBG("BATTERY", "Init %.3fV (%d%%) gain %.4f offset %+.4f V",
        _volts, _pct, _cal, _calOffset);
  }

  // taskSensor ONLY.
  void sample() {
    if (millis() - _lastSample < BATT_SAMPLE_MS) return;
    _lastSample = millis();

#if ENABLE_CHARGE_SENSE
    _charging = (digitalRead(CHRG_SENSE_PIN) == LOW);
    _full     = !_charging && (digitalRead(FULL_SENSE_PIN) == LOW);
#endif

    float v = _rawVolts();

    // Sanity gate. A broken conversion, a disconnected divider or a corrupted
    // calibration produces a number that is not a battery state. Discard it
    // and keep the previous filtered value rather than letting one sample
    // drag the pack to 0% or 100%.
    if (isnan(v) || v < BATT_V_SANE_MIN || v > BATT_V_SANE_MAX) {
      if ((_rejected++ % 40) == 0)
        LOGW("BATTERY", "Implausible sample %.3f V discarded (%lu total) - "
                        "check the divider on GPIO%d and the calibration",
             v, (unsigned long)_rejected, BATT_ADC_PIN);
      return;
    }

    // Slew limiter: a single sample may not move the accepted value more
    // than BATT_MAX_SLEW_V. Kills modem-TX current spikes and ADC glitches
    // without hiding a genuine trend (a real change just takes a few
    // samples to walk in).
    float delta = v - _ema;
    if (delta >  BATT_MAX_SLEW_V) v = _ema + BATT_MAX_SLEW_V;
    if (delta < -BATT_MAX_SLEW_V) v = _ema - BATT_MAX_SLEW_V;

    _ema   = BATT_EMA_ALPHA * v + (1.0f - BATT_EMA_ALPHA) * _ema;
    _volts = _ema;

    float target = (float)_voltsToPercent(_volts);
    _pctF = 0.90f * _pctF + 0.10f * target;
    if (fabsf(_pctF - (float)_pct) >= BATT_PCT_HYST) _pct = (int)(_pctF + 0.5f);
    if (_pct < 0) _pct = 0;
    if (_pct > 100) _pct = 100;
  }

  // VCAL <volts>: caller supplies the true terminal voltage from a
  // multimeter. Filters are re-seeded at the corrected value.
  // Single point, gain only. The offset is forced to zero so the model stays
  // one-parameter: solving a gain against a nonzero offset folds part of that
  // offset into the gain and mis-tracks at both ends of the range.
  float calibrateTo(float measuredV) {
    float uncal = uncalibratedVolts();          // exact inverse of the model
    _calOffset = 0.0f;
    if (uncal > 0.5f && measuredV > 0.5f) _cal = measuredV / uncal;
    _reseed();
    LOGW("VCAL", "Single-point: gain %.4f, offset 0 → %.3fV (%d%%)",
         _cal, _volts, _pct);
    return _cal;
  }

  // Two points, gain and offset together. Needs a real lever arm between the
  // readings: at 0.60 V separation with 5 mV of noise per point the residual
  // gain error is about 1.2%, which is 50 mV at 4.2 V. Shorter spans are
  // refused rather than silently producing a worse calibration than before.
  bool calibrateTwoPoint(float v1t, float v1r, float v2t, float v2r) {
    float span = fabsf(v2r - v1r);
    if (span < BATT_CAL_MIN_SPAN_V) return false;
    float g = (v2t - v1t) / (v2r - v1r);
    float o = v1t - g * v1r;
    if (g < 0.5f || g > 2.0f)  return false;    // implausible for this divider
    if (o < -1.0f || o > 1.0f) return false;
    _cal = g; _calOffset = o;
    _reseed();
    LOGW("VCAL", "Two-point: gain %.4f, offset %+.4f V (span %.3f V) → "
                 "%.3fV (%d%%)", _cal, _calOffset, span, _volts, _pct);
    return true;
  }

  float volts()      const { return _volts; }
  int   percent()    const { return _pct; }
  bool  charging()   const { return _charging; }
  bool  chargeFull() const { return _full; }

private:
  float _cal = 1.0f, _calOffset = 0.0f, _volts = 0.0f, _ema = 0.0f, _pctF = 0.0f;
  int   _pct = 0;
  uint32_t _rejected = 0;
  bool  _charging = false, _full = false;
  unsigned long _lastSample = 0;

  // Re-seed every filter at the current corrected reading. Without this a
  // calibration change would crawl in at the slew limit (0.08 * 30 mV = 2.4 mV
  // per sample, ~20 s for a 200 mV correction) and look like it had not taken.
  void _reseed() {
    float v = _rawVolts();
    _ema = v; _volts = v;
    _pctF = (float)_voltsToPercent(v);
    _pct  = (int)(_pctF + 0.5f);
  }

  // Uncorrected battery volts: pin voltage scaled by the nominal divider only.
  float _rawUncalVolts() {
    return (_rawPinMv() / 1000.0f) * ((BATT_R1 + BATT_R2) / BATT_R2);
  }

  // Corrected battery volts - the forward model.
  float _rawVolts() { return _rawUncalVolts() * _cal + _calOffset; }

  // Median + MAD-filtered ADC pin voltage, millivolts. Sampling unchanged.
  float _rawPinMv() {
    // Two throwaway conversions let the sample-and-hold capacitor charge
    // through the 50 kΩ divider before the samples we keep.
    (void)analogReadMilliVolts(BATT_ADC_PIN);
    (void)analogReadMilliVolts(BATT_ADC_PIN);

    uint32_t s[BATT_BURST];
    for (int i = 0; i < BATT_BURST; i++) {
      s[i] = analogReadMilliVolts(BATT_ADC_PIN);
      delayMicroseconds(BATT_SETTLE_US);
    }
    std::sort(s, s + BATT_BURST);
    uint32_t med = s[BATT_BURST / 2];

    // Median absolute deviation → robust scale estimate → keep samples
    // within 3·MAD of the median, average those. Rejects asymmetric
    // spikes that a plain trimmed mean would still partly absorb.
    uint32_t dev[BATT_BURST];
    for (int i = 0; i < BATT_BURST; i++)
      dev[i] = (s[i] > med) ? (s[i] - med) : (med - s[i]);
    std::sort(dev, dev + BATT_BURST);
    uint32_t mad = dev[BATT_BURST / 2];
    uint32_t band = (mad < 4) ? 12 : (mad * 3);

    uint64_t sum = 0; int n = 0;
    for (int i = 0; i < BATT_BURST; i++) {
      uint32_t d = (s[i] > med) ? (s[i] - med) : (med - s[i]);
      if (d <= band) { sum += s[i]; n++; }
    }
    return n ? (float)(sum / (double)n) : (float)med;
  }

  int _voltsToPercent(float v) {
    if (v >= BATT_CURVE[0].v) return 100;
    if (v <= BATT_CURVE[BATT_CURVE_LEN - 1].v) return 0;
    for (int i = 0; i < BATT_CURVE_LEN - 1; i++) {
      float vh = BATT_CURVE[i].v, vl = BATT_CURVE[i + 1].v;
      if (v <= vh && v >= vl) {
        float span = vh - vl;
        float frac = (span > 0.0001f) ? (v - vl) / span : 0.0f;
        return BATT_CURVE[i + 1].pct +
               (int)((BATT_CURVE[i].pct - BATT_CURVE[i + 1].pct) * frac + 0.5f);
      }
    }
    return 0;
  }
};

// ============================================================================
//  BatteryCalStore - persistent per-board calibration.
//
//  WHY ITS OWN NVS NAMESPACE. Calibration is not application configuration.
//  It is a physical property of THIS board's resistors and THIS chip's ADC,
//  in the same category as RF trim. It therefore lives in "battcal", which the
//  boot provisioner never opens and never clears, so a firmware upload cannot
//  destroy it no matter how the schema moves. The only thing that erases it is
//  an explicit factory reset.
//
//  INTEGRITY. magic + version + checksum, validated on load. A store that
//  fails any check is not used - the firmware falls back to unity gain and
//  says so, rather than silently applying a corrupted correction. That matters
//  outdoors: a brownout during a write must degrade to "uncalibrated", never
//  to "confidently wrong".
//
//  FLASH WEAR. Written only when a calibration is actually solved, which is a
//  deliberate operator action. Never written on a battery reading.
// ============================================================================
// struct BatteryCal is declared up with the includes, alongside
// ProvisionResult, for the Arduino prototype-injection reason documented
// there. bcalLoad/bcalSave/_bcalCrc below all name it in their signatures.

static uint32_t _bcalCrc(const BatteryCal& c) {
  // FNV-1a over the value bytes. Detects a torn write, which is the realistic
  // corruption mode here - not an adversary.
  const uint8_t* b[5] = { (const uint8_t*)&c.ref,    (const uint8_t*)&c.gain,
                          (const uint8_t*)&c.offset, (const uint8_t*)&c.p1True,
                          (const uint8_t*)&c.p1Raw };
  uint32_t h = 2166136261u;
  for (int f = 0; f < 5; f++)
    for (size_t i = 0; i < sizeof(float); i++) { h ^= b[f][i]; h *= 16777619u; }
  h ^= (uint32_t)BCAL_VERSION; h *= 16777619u;
  return h;
}

static bool _bcalPlausible(const BatteryCal& c) {
  if (isnan(c.gain) || isnan(c.offset)) return false;
  if (c.gain   < 0.5f || c.gain   > 2.0f) return false;
  if (c.offset < -1.0f || c.offset > 1.0f) return false;
  return true;
}

static BatteryCal bcalLoad() {
  BatteryCal c;
  Preferences p;
  if (!p.begin(BCAL_NS, true)) return c;              // absent = first run
  uint32_t magic = p.getULong ("magic", 0);
  uint16_t ver   = (uint16_t)p.getUShort("ver", 0);
  c.ref    = p.getFloat("ref",    0.0f);
  c.gain   = p.getFloat("gain",   1.0f);
  c.offset = p.getFloat("offset", 0.0f);
  c.p1True = p.getFloat("p1t",    0.0f);
  c.p1Raw  = p.getFloat("p1r",    0.0f);
  uint32_t crc = p.getULong("crc", 0);
  p.end();

  if (magic != BCAL_MAGIC) return BatteryCal();
  if (ver   != BCAL_VERSION) {
    // Migration point. Version 1 is the first, so there is nothing to migrate
    // from yet; a future layout change adds its conversion here. Until then an
    // unknown version falls back to factory defaults rather than guessing.
    LOGW("BCAL", "Stored calibration is version %u, firmware expects %u - "
                 "falling back to factory defaults", ver, BCAL_VERSION);
    return BatteryCal();
  }
  if (crc != _bcalCrc(c)) {
    LOGW("BCAL", "Stored calibration FAILED checksum - discarded, running "
                 "uncalibrated until recalibrated");
    return BatteryCal();
  }
  if (!_bcalPlausible(c)) {
    LOGW("BCAL", "Stored calibration out of range (gain %.4f offset %+.4f) - "
                 "discarded", c.gain, c.offset);
    return BatteryCal();
  }
  c.valid = true;
  return c;
}

static bool bcalSave(BatteryCal& c) {
  if (!_bcalPlausible(c)) return false;
  Preferences p;
  if (!p.begin(BCAL_NS, false)) return false;
  p.putULong ("magic",  BCAL_MAGIC);
  p.putUShort("ver",    BCAL_VERSION);
  p.putFloat ("ref",    c.ref);
  p.putFloat ("gain",   c.gain);
  p.putFloat ("offset", c.offset);
  p.putFloat ("p1t",    c.p1True);
  p.putFloat ("p1r",    c.p1Raw);
  p.putULong ("crc",    _bcalCrc(c));
  p.end();
  c.valid = true;
  return true;
}

static void bcalErase() {
  Preferences p;
  if (p.begin(BCAL_NS, false)) { p.clear(); p.end(); }
}

// Legacy bridge: v6.2 kept a bare gain in NVS "tracker"/"bcal". Adopt it once
// so an already-calibrated unit does not lose its correction on this upgrade,
// then never look again.
static bool bcalMigrateLegacy(BatteryCal& c) {
  Preferences t;
  if (!t.begin("tracker", true)) return false;
  float g = t.getFloat("bcal",  1.0f);
  float o = t.getFloat("bcalo", 0.0f);
  t.end();
  if (fabsf(g - 1.0f) < 0.0005f && fabsf(o) < 0.0005f) return false;  // never set
  c.ref = 0.0f; c.gain = g; c.offset = o; c.p1True = 0.0f; c.p1Raw = 0.0f;
  if (!bcalSave(c)) return false;
  LOGW("BCAL", "Migrated legacy calibration from NVS 'tracker' (gain %.4f, "
               "offset %+.4f V) into '%s'", g, o, BCAL_NS);
  return true;
}

BatteryCal batteryCal;    // the live copy

// ============================================================================
//  LEDManager — report section G. Pure state machine, zero delay(), driven
//  from taskSensor at ~50 Hz. All three LEDs are PWM.
//
//   RED    (25) battery level. Full → barely lit, empty → maximum. Smooth
//               PWM over the whole range with gamma correction so the
//               apparent brightness tracks the percentage linearly.
//   YELLOW (33) network / transmission:
//                 no internet        → 500 ms blink
//                 online + live      → 80 ms flash per transmission, and a
//                                      heartbeat flash at least every 10 s
//                 bulk sync running  → continuous LOW→HIGH→LOW ramp, which
//                                      is visually unmistakable against
//                                      both blink patterns
//   GREEN  (32) GPS quality (fix + satellites + HDOP) as brightness;
//               blinks at 400 ms if no usable GPS data for 60 s.
// ============================================================================

#define GPS_STALE_MS      60000UL
#define LED_RED_MIN_DUTY  30           // "very dim", still visibly alive
#define LED_RED_MAX_DUTY  PWM_MAX
#define LED_GRN_MIN_DUTY  25
#define LED_GRN_MAX_DUTY  PWM_MAX

class LEDManager {
public:
  void begin() {
    PWM_ATTACH(LED_RED_PIN,    PWM_CH_RED,    PWM_FREQ_HZ, PWM_BITS);
    PWM_ATTACH(LED_YELLOW_PIN, PWM_CH_YELLOW, PWM_FREQ_HZ, PWM_BITS);
    PWM_ATTACH(LED_GREEN_PIN,  PWM_CH_GREEN,  PWM_FREQ_HZ, PWM_BITS);
    // Boot sweep proves all three channels before anything else runs.
    for (int i = 0; i < 3; i++) {
      _raw(i, PWM_MAX); delay(140); _raw(i, 0);
    }
    DBG("LED", "RED=battery | YELLOW=network/TX | GREEN=GPS quality (all PWM)");
  }

  void notifyTx()               { _txFlashUntil = millis() + 80; }
  void setNetMode(NetLed m)     { _netMode = m; }
  void setBatteryPct(int pct)   { _battPct = pct; }
  void setGps(bool fix, int sats, float hdop) {
    _fix = fix; _sats = sats; _hdop = hdop;
    if (fix) _lastGoodGps = millis();
  }
  bool gpsStale() const { return (millis() - _lastGoodGps) > GPS_STALE_MS; }

  void update() {
    unsigned long now = millis();

    // ---- RED: battery. Perceptual (gamma 2.2) so a 50 % battery looks
    // half as bright as an empty one, not 20 % as bright.
    float want =  (constrain(_battPct, 0, 100) / 100.0f);
    float g    = powf(want, 2.2f);
    _raw(0, (uint32_t)(LED_RED_MIN_DUTY + g * (LED_RED_MAX_DUTY - LED_RED_MIN_DUTY)));

    // ---- YELLOW: network / transmission
    uint32_t y = 0;
    switch (_netMode) {
      case NETLED_BULK: {
        // Triangle ramp, 1.2 s period. Deliberately unlike any blink.
        uint32_t ph = now % 1200UL;
        y = (ph < 600UL) ? (ph * PWM_MAX / 600UL)
                         : ((1200UL - ph) * PWM_MAX / 600UL);
        break;
      }
      case NETLED_LIVE:
        if (now - _liveHeartbeat >= 10000UL) { _liveHeartbeat = now; notifyTx(); }
        y = (now < _txFlashUntil) ? PWM_MAX : 0;
        break;
      case NETLED_OFFLINE:
      default:
        y = ((now / 500UL) & 1UL) ? PWM_MAX : 0;
        break;
    }
    _raw(1, y);

    // ---- GREEN: GPS quality, or blink when stale
    uint32_t gr;
    if (gpsStale()) {
      gr = ((now / 400UL) & 1UL) ? PWM_MAX : 0;
    } else {
      gr = (uint32_t)(LED_GRN_MIN_DUTY +
                      powf(quality(), 2.2f) * (LED_GRN_MAX_DUTY - LED_GRN_MIN_DUTY));
    }
    _raw(2, gr);
  }

  // 0.0 .. 1.0 — fix validity 40 %, satellite count 30 %, HDOP 30 %.
  float quality() const {
    if (!_fix) return 0.0f;
    float q = 0.40f;
    q += 0.30f * constrain((_sats - 3) / 9.0f, 0.0f, 1.0f);   // 3 sats→0, 12→full
    float h = _hdop;
    if (h < 0.8f) h = 0.8f;
    q += 0.30f * constrain((3.0f - h) / 2.2f, 0.0f, 1.0f);    // 3.0→0, 0.8→full
    return constrain(q, 0.0f, 1.0f);
  }

private:
  NetLed _netMode = NETLED_OFFLINE;
  int    _battPct = 0, _sats = 0;
  float  _hdop    = 99.9f;
  bool   _fix     = false;
  unsigned long _txFlashUntil = 0, _liveHeartbeat = 0, _lastGoodGps = 0;
  uint32_t _last[3] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};

  void _raw(int idx, uint32_t duty) {
    if (duty > PWM_MAX) duty = PWM_MAX;
    if (_last[idx] == duty) return;         // no redundant register writes
    _last[idx] = duty;
    switch (idx) {
      case 0: PWM_WRITE(LED_RED_PIN,    PWM_CH_RED,    duty); break;
      case 1: PWM_WRITE(LED_YELLOW_PIN, PWM_CH_YELLOW, duty); break;
      case 2: PWM_WRITE(LED_GREEN_PIN,  PWM_CH_GREEN,  duty); break;
    }
  }
};

// ============================================================================
//  TimeService — THE single time authority. v6.1.
//
//  REPLACES GPSManager, which was the whole bug: it computed the timestamp as
//  a pure function of live TinyGPS++ parser state, with a hardcoded
//  "1970-01-01 00:00:00" for the case where that state was not yet populated.
//  Two distinct defects followed from that one design choice:
//
//    D1  COLD BOOT. From power-on until first satellite fix — 30 s to 15 min,
//        and forever indoors — every record carried the 1970 literal. It was
//        written to SD, into the batch filename, and into visitDate/visitTime
//        on the wire. The backend cannot distinguish it from a device that
//        genuinely believes it is 1970.
//
//    D2  FROZEN CLOCK (not in the original bug report, found while mapping).
//        TinyGPSDate::valid and TinyGPSTime::valid LATCH TRUE on first commit
//        and are never reset — verified against TinyGPS++ source: only
//        TinyGPSCustom::begin() ever clears a valid flag. So after the first
//        fix, isValid() stays true forever while date/time hold the LAST
//        PARSED values. Drive into a tunnel or a basement car park and every
//        record for the next forty minutes carries the same frozen instant —
//        not 1970, not advancing, and entirely plausible-looking. Silent.
//
//  Both are cured by the same structure: one reference point (UTC epoch +
//  monotonic timer), fed by ranked sources, projected forward when no source
//  is currently offering anything, and explicitly flagged when no source ever
//  has. age() rather than isValid() is what makes D2 detectable.
//
//  AUTHORITY (v6.2, inverted):   NTP/CELL  >  GPS fix  >  Holdover  >  Unsynced
//
//  The network is the primary authority and is never refused. GPS is the
//  fallback: it is admitted only when the effective source is NOT a fresh
//  network reference — that is, when the last network sync has aged past
//  NET_FRESH_S, or when _netStarved says no transport has been able to
//  confirm the clock on the current tick. Both conditions are already
//  computed by source(), which is therefore the single arbiter; ingestGps()
//  asks it rather than re-deriving the rule, so the two cannot drift apart.
//
//  A GPS instant arriving while the network holds authority is not thrown
//  away silently — it is differenced against the running clock and logged at
//  a low rate, so a disagreeing constellation is visible rather than assumed.
//
//  Why this ranking and not v6.1's. GPS is more accurate when present, but
//  accuracy is worthless if the source is absent, and on this unit it very
//  often is: indoors, in a bag, under a roof. The network is present nearly
//  always and is accurate to well inside one second, which is far below the
//  resolution this tracker records at. Ranking availability above precision
//  is the correct trade for a timestamp; it would be the wrong trade for a
//  position, which is exactly why GPS remains untouched for positioning.
//
//  TIMEZONE:    everything internal is UTC. TZ_OFFSET_SECONDS is applied in
//  format() and nowhere else, so the GPS path and the network path cannot
//  drift apart or double-shift — the classic failure of this exact fix.
//
//  CONCURRENCY: the reference is an (int64 epoch, int64 microseconds) pair
//  written by taskSensor (GPS) and taskNet (network) and read by everything.
//  A torn read between the two halves would produce a wildly wrong instant,
//  so the pair is guarded by a portMUX spinlock. Critical sections are a
//  handful of instructions; nothing blocks.
//
//  Calendar arithmetic is Howard Hinnant's civil-days algorithm, carried over
//  unchanged from GPSManager — correct across month, year and leap rollover
//  without touching libc TZ state.
// ============================================================================
class TimeService {
public:
  // ---- ingestion: GPS (called from taskSensor) --------------------------
  // Takes the parser by reference rather than reading globals, so this stays
  // the only code in the firmware that touches gps.date / gps.time.
  bool ingestGps(TinyGPSPlus& g) {
    if (!g.date.isValid() || !g.time.isValid()) return false;

    // THE D2 FIX. isValid() latches true forever; age() is the only field
    // that tells the truth about whether this is a live fix or a fossil.
    uint32_t ageMs = g.date.age() > g.time.age() ? g.date.age() : g.time.age();
    if (ageMs > GPS_TIME_MAX_AGE_MS) return false;

    int  y  = (int)g.date.year();
    int  mo = (int)g.date.month(), d = (int)g.date.day();
    int  hh = (int)g.time.hour(),  mi = (int)g.time.minute(), ss = (int)g.time.second();
    if (!_sane(y, mo, d, hh, mi, ss)) {
      if (millis() - _lastGpsRejectLog > 60000UL) {
        _lastGpsRejectLog = millis();
        LOGW("TIME", "GPS date %04d-%02d-%02d %02d:%02d:%02d REFUSED "
                     "(outside %d..%d — week rollover or corrupt NMEA)",
             y, mo, d, hh, mi, ss, TIME_BUILD_YEAR, TIME_BUILD_YEAR + 30);
      }
      return false;
    }

    int64_t epoch = _toEpoch(y, mo, d, hh, mi, ss);

    // ---- T4: THE v6.2 AUTHORITY CHECK ------------------------------------
    // v6.1 fell straight through to _setRef() here, unconditionally, four
    // times a second. That single line was the whole inversion: one NMEA
    // sentence displaced a good NTP reference, and the next one displaced it
    // again. Ask source() first. It already folds together "the reference is
    // a network one", "it has not aged past NET_FRESH_S" and "a transport is
    // actually up to confirm it" (_netStarved), which is precisely the
    // condition under which GPS must stand down. Reusing it means there is
    // one definition of network freshness in the firmware, not two.
    int64_t  nowUs = esp_timer_get_time();
    bool     had   = false;
    int64_t  curUtc = 0;
    TimeSrc  cur   = TSRC_NONE;
    uint32_t age   = 0;
    _snapshot(had, curUtc, cur, age, nowUs);

    if (_isNetSrc(source())) {
      // Network holds the clock. Do not touch it — but do not discard the
      // reading either: the difference between an independent satellite
      // instant and the running clock is the only cross-check this device
      // has. Rate-limited hard, because this path runs at 4 Hz.
      if (millis() - _lastGpsDriftLog > GPS_DRIFT_LOG_MS) {
        _lastGpsDriftLog = millis();
        LOGW("TIME", "GPS instant %+lld s from %s clock — GPS is fallback, "
                     "authority unchanged (drift measurement only)",
             (long long)(epoch - curUtc), timeSrcName(cur));
      }
      return false;
    }

    // Network is stale, starved or has never synced: GPS is the best source
    // available and takes the clock. Announce the handoff once, not per fix.
    if (had && _isNetSrc(cur))
      LOGW("TIME", "Network time unavailable (%s ref %lu s old%s) — "
                   "falling back to GPS time",
           timeSrcName(cur), (unsigned long)age,
           _netStarved ? ", no transport" : "");

    // Anchor at the instant the sentence was committed, not now, so the
    // parse-to-ingest latency does not become a permanent offset.
    _setRef(epoch, nowUs - (int64_t)ageMs * 1000LL, TSRC_GPS);
    return true;
  }

  // ---- ingestion: network (called from taskNet) -------------------------
  // Returns true if the value took authority. A refusal still measures drift.
  bool ingestNetwork(int64_t epochUtc, TimeSrc src) {
    if (epochUtc < _floorEpoch()) return false;

    int64_t  nowUs  = esp_timer_get_time();
    bool     had    = false;
    int64_t  curUtc = 0;
    TimeSrc  cur    = TSRC_NONE;
    uint32_t age    = 0;
    _snapshot(had, curUtc, cur, age, nowUs);

    if (had) {
      int64_t skew = epochUtc - curUtc;
      // ---- T5: THE v6.2 AUTHORITY CHANGE ---------------------------------
      // v6.1 returned false here whenever the current reference was GPS
      // younger than GPS_AUTHORITY_HOLD_S, which meant a wrong-but-plausible
      // satellite instant owned the clock for a full hour while a correct
      // NTP answer sat unused in this very function. The network is now
      // never refused for authority. Reclaiming the clock from GPS is a
      // state change worth a line in the log; a routine drift correction on
      // an existing network reference is only worth one past 2 s.
      if (cur == TSRC_GPS)
        LOGW("TIME", "%s reclaiming authority from GPS (%+lld s correction, "
                     "GPS ref %lu s old)",
             timeSrcName(src), (long long)skew, (unsigned long)age);
      else if (skew > 2 || skew < -2)
        LOGW("TIME", "Clock corrected %+lld s by %s (previous ref %s, %lu s old)",
             (long long)skew, timeSrcName(src), timeSrcName(cur), (unsigned long)age);
    } else {
      LOGW("TIME", "First sync from %s — clock is live", timeSrcName(src));
    }

    _setRef(epochUtc, nowUs, src);
    return true;
  }

  // ---- query ------------------------------------------------------------
  bool synced() {
    bool had; int64_t utc; TimeSrc s; uint32_t age;
    _snapshot(had, utc, s, age, esp_timer_get_time());
    return had;
  }

  int64_t nowUtc() {
    bool had; int64_t utc; TimeSrc s; uint32_t age;
    _snapshot(had, utc, s, age, esp_timer_get_time());
    return had ? utc : 0;
  }

  // Effective source: what actually set the clock, downgraded to HOLDOVER
  // once that reference can no longer be called fresh.
  //
  // Elapsed time alone is not sufficient for a network reference. The sync
  // cadence is 6 h, so a 3-hour-old NTP value is "not due yet" — but a device
  // that has been failing to reach any source every 60 s for those 3 hours is
  // coasting on a free-running crystal, and calling that NTP overstates it.
  // Starvation is therefore holdover no matter how recent the last success.
  TimeSrc source() {
    bool had; int64_t utc; TimeSrc s; uint32_t age;
    _snapshot(had, utc, s, age, esp_timer_get_time());
    if (!had) return TSRC_NONE;
    if (s == TSRC_GPS) return (age <= GPS_FRESH_S) ? s : TSRC_HOLD;
    return (age > NET_FRESH_S || _netStarved) ? TSRC_HOLD : s;
  }

  TimeSrc refSource() {
    bool had; int64_t utc; TimeSrc s; uint32_t age;
    _snapshot(had, utc, s, age, esp_timer_get_time());
    return had ? s : TSRC_NONE;
  }

  uint32_t sinceSyncSec() {
    bool had; int64_t utc; TimeSrc s; uint32_t age;
    _snapshot(had, utc, s, age, esp_timer_get_time());
    return had ? age : 0;
  }

  // ---- THE formatter — the only place TZ_OFFSET_SECONDS is applied ------
  void format(char* out, size_t cap) {
    bool had; int64_t utc; TimeSrc s; uint32_t age;
    _snapshot(had, utc, s, age, esp_timer_get_time());
    if (!had) { snprintf(out, cap, "%s", TS_UNSYNCED); return; }

    int64_t local = utc + (int64_t)TZ_OFFSET_SECONDS;
    int64_t z     = local / 86400LL;
    int     sod   = (int)(local - z * 86400LL);
    if (sod < 0) { sod += 86400; z -= 1; }        // pre-1970 guard, defensive
    int y; unsigned mo, d;
    _civilFromDays(z, y, mo, d);
    snprintf(out, cap, "%04d-%02u-%02u %02d:%02d:%02d",
             y, mo, d, sod / 3600, (sod % 3600) / 60, sod % 60);
  }

  String formatStr() { char b[24]; format(b, sizeof(b)); return String(b); }

  // ---- network sync driver ----------------------------------------------
  // Declared here, defined after the globals it needs (same pattern the file
  // already uses for SDStore::_stampNow).
  void netSyncTick();
  void forceResync() { _nextAttempt = 0; _phase = NTP_IDLE; }
  const char* phaseName() const;

private:
  // Reference point. Guarded as a unit: a half-updated pair is a wrong clock.
  int64_t  _refEpochUtc = 0;
  int64_t  _refUs       = 0;
  TimeSrc  _refSrc      = TSRC_NONE;
  bool     _hasRef      = false;
  portMUX_TYPE _mux     = portMUX_INITIALIZER_UNLOCKED;

  unsigned long _lastGpsRejectLog = 0;

  // v6.2. Throttle for the GPS-vs-network drift line emitted while the
  // network holds authority. taskSensor only.
  unsigned long _lastGpsDriftLog = 0;

  // v6.2. Last effective source announced to the log, so _maybeAnnounce()
  // can emit one line per TRANSITION instead of one per tick. taskNet only.
  TimeSrc _lastAnnounced = TSRC_NONE;
  bool    _announcedOnce = false;

  // The one place that answers "is this a network-derived source". Both the
  // authority check in ingestGps() and the reporting in netSyncTick() go
  // through it, so adding a future source means editing exactly one line.
  static bool _isNetSrc(TimeSrc s) { return s == TSRC_NTP || s == TSRC_CELL; }

  // Emits "[TIME] Time source: X" when the EFFECTIVE authority changes.
  // Called from netSyncTick(), i.e. taskNet, i.e. one writer for _lastAnnounced.
  void _maybeAnnounce() {
    TimeSrc eff = source();
    if (_announcedOnce && eff == _lastAnnounced) return;
    _announcedOnce = true;
    _lastAnnounced = eff;
    if (eff == TSRC_NONE) {
      LOGW("TIME", "Time source: UNSYNCED — no source has ever set the clock");
    } else if (eff == TSRC_HOLD) {
      LOGW("TIME", "No external time source — using last valid system clock | "
                   "Time source: HOLDOVER (ref %s, %lu s old)",
           timeSrcName(refSource()), (unsigned long)sinceSyncSec());
    } else {
      char b[24]; format(b, sizeof(b));
      LOGW("TIME", "Time source: %s | Current time: %s (BST, UTC+6)",
           timeSrcName(eff), b);
    }
  }

  // Set when a network sync attempt does not succeed, cleared when one does.
  // Single writer (taskNet), many readers — the same pattern the rest of the
  // firmware uses for cross-task scalars. Advisory only: it changes the
  // freshness LABEL, never the time value itself.
  volatile bool _netStarved = false;

  // NTP state machine (taskNet only — no locking needed on these).
  enum NtpPhase : uint8_t { NTP_IDLE, NTP_WAITING };
  NtpPhase      _phase       = NTP_IDLE;
  unsigned long _nextAttempt = 0;
  unsigned long _armedAt     = 0;
  int64_t       _clockAtArm  = 0;
  int64_t       _usAtArm     = 0;

  int64_t _queryModemClock();          // defined with netSyncTick, needs modemSerial

  void _setRef(int64_t epochUtc, int64_t refUs, TimeSrc src) {
    portENTER_CRITICAL(&_mux);
    _refEpochUtc = epochUtc;
    _refUs       = refUs;
    _refSrc      = src;
    _hasRef      = true;
    portEXIT_CRITICAL(&_mux);
  }

  // One consistent read of the whole reference, plus the projection.
  void _snapshot(bool& had, int64_t& utcOut, TimeSrc& srcOut,
                 uint32_t& ageOut, int64_t nowUs) {
    int64_t e, u;
    portENTER_CRITICAL(&_mux);
    had = _hasRef; e = _refEpochUtc; u = _refUs; srcOut = _refSrc;
    portEXIT_CRITICAL(&_mux);
    if (!had) { utcOut = 0; ageOut = 0; srcOut = TSRC_NONE; return; }
    int64_t elapsed = (nowUs - u) / 1000000LL;    // esp_timer never wraps
    if (elapsed < 0) elapsed = 0;
    utcOut = e + elapsed;
    ageOut = (uint32_t)elapsed;
  }

  static int64_t _floorEpoch() {
    return _daysFromCivil(TIME_BUILD_YEAR, 1, 1) * 86400LL;
  }

  static bool _sane(int y, int mo, int d, int hh, int mi, int ss) {
    return y >= TIME_BUILD_YEAR && y <= TIME_BUILD_YEAR + 30 &&
           mo >= 1 && mo <= 12 && d >= 1 && d <= 31 &&
           hh >= 0 && hh <= 23 && mi >= 0 && mi <= 59 && ss >= 0 && ss <= 59;
  }

  static int64_t _toEpoch(int y, int mo, int d, int hh, int mi, int ss) {
    return _daysFromCivil(y, mo, d) * 86400LL
         + (int64_t)hh * 3600LL + (int64_t)mi * 60LL + (int64_t)ss;
  }

  // AT+CCLK? → UTC epoch, or 0 on refusal. Static and pure so it is unit
  // testable off-device; verified on host against libc timegm().
  static int64_t _parseCCLK(const char* resp) {
    const char* p = strstr(resp, "+CCLK:");
    if (!p) return 0;
    p = strchr(p, '"');
    if (!p) return 0;
    p++;
    int yy, MM, dd, hh, mi, ss, zz;
    char sign;
    // The ±zz field is MANDATORY. A modem reporting local time without a zone
    // is indistinguishable from one reporting UTC, and guessing wrong is a
    // silent six-hour error — refuse instead.
    if (sscanf(p, "%2d/%2d/%2d,%2d:%2d:%2d%c%2d",
               &yy, &MM, &dd, &hh, &mi, &ss, &sign, &zz) != 8) return 0;
    if (sign != '+' && sign != '-') return 0;
    int y = 2000 + yy;
    if (!_sane(y, MM, dd, hh, mi, ss)) return 0;
    int64_t localEpoch = _toEpoch(y, MM, dd, hh, mi, ss);
    int64_t offs = (int64_t)zz * 900LL * (sign == '-' ? -1 : 1);
    return localEpoch - offs;                     // → UTC
  }

  static long long _daysFromCivil(int y, unsigned m, unsigned d) {
    y -= (m <= 2);
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? (unsigned)-3 : 9u)) + 2u) / 5u + d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097LL + (long long)doe - 719468LL;
  }

  static void _civilFromDays(long long z, int& y, unsigned& m, unsigned& d) {
    z += 719468LL;
    long long era = (z >= 0 ? z : z - 146096LL) / 146097LL;
    unsigned doe = (unsigned)(z - era * 146097LL);
    unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    long long yy = (long long)yoe + era * 400LL;
    unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    unsigned mp  = (5u * doy + 2u) / 153u;
    d = doy - (153u * mp + 2u) / 5u + 1u;
    m = (mp < 10u) ? (mp + 3u) : (mp - 9u);
    y = (int)(yy + (m <= 2u));
  }
};

// ============================================================================
//  E2EVerifier — the delivery-acknowledgement mechanism.
//
//  MODE 0 (default, unchanged from v5.2): the device subscribes to its own
//  topics; every outgoing payload is FNV-1a hashed and matched against the
//  broker's echo. A match proves device → transport → broker → device.
//  It does NOT prove the backend consumed the message — see remaining risks.
//
//  MODE 1 (cfg.ackMode = 1): the backend replies on <topic>/ack with the
//  batchId it processed. This is a true application-level ACK and is the
//  correct long-term answer; it requires a backend change, so it is off by
//  default and adds a "batchId" field to bulk payloads only when enabled.
// ============================================================================
class E2EVerifier {
public:
  uint32_t liveSent = 0, liveVerified = 0, liveLost = 0;
  uint32_t pbSent   = 0, pbVerified   = 0;
  unsigned long lastLiveEchoMs = 0, lastPbEchoMs = 0;

  volatile bool livePending = false;
  volatile bool pbPending   = false;
  volatile bool pbAckOk     = false;

  static uint32_t fnv1a(const char* d, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= (uint8_t)d[i]; h *= 16777619u; }
    return h;
  }

  void expectLive(const char* payload, size_t len) {
    _liveHash = fnv1a(payload, len);
    livePending = true;
    liveSent++;
  }

  void expectBulk(const char* payload, size_t len, uint32_t batchId) {
    _pbHash  = fnv1a(payload, len);
    _batchId = batchId;
    pbPending = true;
    pbAckOk   = false;
    pbSent++;
  }

  void setAckMode(int m, const String& ackTopic) { _mode = m; _ackTopic = ackTopic; }

  // PubSubClient callback context. Keep this cheap — it runs inside
  // mqtt->loop() on the network task.
  void onMessage(const char* topic, const uint8_t* payload, unsigned int len) {
    if (_mode == 1 && _ackTopic.length() && _ackTopic == topic) {
      // Backend ACK: {"batchId":<n>, ...}. Substring match is sufficient
      // and avoids pulling a JSON parser into the callback path.
      char needle[32];
      snprintf(needle, sizeof(needle), "\"batchId\":%lu", (unsigned long)_batchId);
      String body; body.reserve(len + 1);
      for (unsigned int i = 0; i < len && i < 512; i++) body += (char)payload[i];
      if (pbPending && body.indexOf(needle) >= 0) {
        pbPending = false; pbAckOk = true; pbVerified++;
        lastPbEchoMs = millis();
        DBG("E2E", "BULK batch %lu ACKed by backend", (unsigned long)_batchId);
      }
      return;
    }

    uint32_t h = fnv1a((const char*)payload, len);
    if (livePending && h == _liveHash) {
      livePending = false; liveVerified++; lastLiveEchoMs = millis();
      DBG("E2E", "LIVE verified via broker echo (#%u, %u bytes)", liveVerified, len);
    }
    if (pbPending && h == _pbHash) {
      pbPending = false; pbAckOk = true; pbVerified++; lastPbEchoMs = millis();
      DBG("E2E", "BULK batch verified via broker echo (#%u, %u bytes)", pbVerified, len);
    }
  }

  String toJSON() {
    char b[224];
    snprintf(b, sizeof(b),
      "{\"liveSent\":%u,\"liveVerified\":%u,\"liveLost\":%u,\"pbSent\":%u,"
      "\"pbVerified\":%u,\"lastLiveEchoMs\":%lu,\"lastPbEchoMs\":%lu,\"ackMode\":%d}",
      liveSent, liveVerified, liveLost, pbSent, pbVerified,
      lastLiveEchoMs, lastPbEchoMs, _mode);
    return String(b);
  }

private:
  uint32_t _liveHash = 0, _pbHash = 0, _batchId = 0;
  int      _mode = 0;
  String   _ackTopic = "";
};

// ============================================================================
//  SDStore — the durable FIFO. Sole owner of the SD card.
//
//  STORAGE MODEL (adapted from the proven reference logger)
//    /LOGS/QUEUE/Q00000042_20260810-061500.jsonl   pending, one file = one batch
//    /LOGS/SENT /Q00000041_20260810-061422.jsonl   delivered + verified
//
//  The zero-padded sequence number comes FIRST so lexicographic order,
//  numeric order and chronological order are the same thing — that is what
//  makes FIFO ordering a property of the filesystem rather than of RAM
//  state. The human-readable timestamp follows it so a file can be
//  identified by eye on a card reader. v5.2's "off_<ts>_<millis%100000>"
//  names could not be ordered at all once GPS time was invalid (every file
//  became off_19700101_000000_<random>), which is why its "oldest file"
//  lookup returned files in effectively arbitrary order.
//
//  ONE FILE = ONE BATCH is the second key idea from the reference: it makes
//  delete-after-ACK atomic and makes reboot recovery trivial. Within a file
//  we additionally persist how many records have been ACKed, so a reboot
//  mid-file cannot replay work that the broker already confirmed.
//
//  PATH CANONICALISATION (root cause R1): File::name() returns the full
//  path on core 2.x and the bare basename on core 1.x/some builds. Every
//  path derived from a directory walk is reduced to its basename and
//  re-anchored under the owning directory. Never concatenate name()
//  directly onto a directory prefix.
// ============================================================================

#define LOG_ROOT     "/LOGS"
#define DIR_QUEUE    "/LOGS/QUEUE"
#define DIR_SENT     "/LOGS/SENT"
#define REC_LINE_MAX 224          // v6.1: 195 -> 206 worst case with "tsrc"
#define SEQ_PERSIST_EVERY 10

class SDStore {
public:
  // ---- lifecycle ---------------------------------------------------------
  bool begin(uint8_t csPin) {
    _cs = csPin;
    _mtx = xSemaphoreCreateRecursiveMutex();
    _nvs.begin("fifo", false);
    _recSeq = _nvs.getULong("recseq", 0);

    if (!SD.begin(csPin)) {
      _ok = false;
      LOGW("SD", "MOUNT FAILED — health check will keep retrying");
      return false;
    }
    _ok = true;
    _ensureDirs();
    _recoverTail();
    _resyncIndexUnlocked();
    _reconcileSeqFromDisk();
    LOGW("SD", "Ready | %u sealed batch file(s) | tail %u/%u | next record seq %lu",
         _queueFiles, _tailRecords, _recPerFile, (unsigned long)_recSeq + 1);
    return true;
  }

  void setRecordsPerFile(uint16_t n) { _recPerFile = (n < 1) ? 1 : n; }
  void setMaxStoragePct(int p)       { _maxPct = p; }
  void setAutoDelete(bool on)        { _autoDelete = on; }

  // Functional check + auto-remount. Returns true if usable right now.
  bool healthCheck() {
    Guard g(_mtx);
    if (_ok) {
      File d = SD.open(DIR_QUEUE);
      if (d) { d.close(); return true; }
      LOGW("SD", "HEALTH: /LOGS/QUEUE unreadable — card lost, remounting");
      _ok = false;
      SD.end();
    }
    if (SD.begin(_cs)) {
      _ok = true;
      _ensureDirs();
      _recoverTail();
      _resyncIndexUnlocked();
      LOGW("SD", "HEALTH: remount OK — %u batch file(s) recovered", _queueFiles);
      return true;
    }
    return false;
  }

  bool     isOk()        const { return _ok; }
  uint16_t queueFiles()  const { return _queueFiles; }
  uint16_t tailRecords() const { return _tailRecords; }
  uint32_t evicted()     const { return _evicted; }
  uint32_t writeErrors() const { return _writeErrors; }
  uint32_t totalRecords()const { return _totalRecords; }
  uint32_t nextSeq()           { return ++_recSeq; }   // taskSensor only

  // O(1). No SD access. This is called on every record and would otherwise
  // be a full directory walk (root cause R3).
  bool hasBacklog() const { return (_queueFiles > 0) || (_tailRecords > 0); }

  void persistSeqIfDue(bool force = false) {
    if (!force && ++_sinceSeqPersist < SEQ_PERSIST_EVERY) return;
    _sinceSeqPersist = 0;
    _nvs.putULong("recseq", _recSeq);
  }

  // ---- NVS passthrough for the sync engine's persisted progress ----------
  uint32_t nvsGet(const char* k, uint32_t d) { return _nvs.getULong(k, d); }
  void     nvsPut(const char* k, uint32_t v) { _nvs.putULong(k, v); }

  // ---- write path --------------------------------------------------------
  // Appends one record to the FIFO tail, sealing the file when it reaches
  // recordsPerFile. Returns false ONLY if the record is genuinely not on
  // the card — the caller must treat that as data loss and say so loudly.
  bool append(const GpsRecord& r) {
    Guard g(_mtx);
    if (!_ok && !_healthCheckUnlocked()) { _writeErrors++; return false; }

    if (!_ensureTailUnlocked()) { _writeErrors++; return false; }

    char line[REC_LINE_MAX];
    int n = _serialise(r, line, sizeof(line));
    // snprintf returns what it WOULD have written. Without the upper bound a
    // record longer than the buffer would be appended truncated, and _parse
    // would then reject it on read — a silent loss. Related to the v6.1
    // REC_LINE_MAX change, so guarded here rather than left implicit.
    if (n <= 0 || n >= (int)sizeof(line)) {
      _writeErrors++;
      LOGW("SD", "Record seq %lu serialised to %d bytes, buffer is %u — NOT written",
           (unsigned long)r.seq, n, (unsigned)sizeof(line));
      return false;
    }

    File f = SD.open(_tailPath, FILE_APPEND);
    if (!f) {
      // One remount + retry, then give up honestly.
      LOGW("SD", "append: cannot open %s — remounting", _tailPath.c_str());
      _ok = false;
      if (!_healthCheckUnlocked() || !_ensureTailUnlocked()) { _writeErrors++; return false; }
      f = SD.open(_tailPath, FILE_APPEND);
      if (!f) { _writeErrors++; LOGW("SD", "append: retry failed"); return false; }
    }

    size_t w = f.print(line);
    f.print("\n");
    f.flush();
    f.close();
    if (w == 0) { _writeErrors++; return false; }

    _tailRecords++;
    _totalRecords++;

    if (_tailRecords >= _recPerFile) _sealTailUnlocked();
    return true;
  }

  // Seal a partial tail so it becomes transmittable. Called when the link
  // returns and the tail is the only thing holding records — this is what
  // stops the sync engine from reading a file that logging is still
  // appending to (root cause R6).
  void sealTail() {
    Guard g(_mtx);
    if (_tailRecords > 0) _sealTailUnlocked();
  }

  // ---- read path (sync engine) -------------------------------------------
  struct BatchInfo {
    char     path[72];
    uint32_t seq;
    uint16_t records;
    bool     valid;
  };

  // Oldest sealed batch file. O(1) in the common case: the cached oldest
  // sequence is trusted, and only re-derived from disk if it is stale.
  BatchInfo oldestBatch() {
    Guard g(_mtx);
    BatchInfo b; b.valid = false; b.records = 0; b.seq = 0; b.path[0] = 0;
    if (!_ok || _queueFiles == 0) return b;

    String p = _findOldestQueuePathUnlocked(b.seq);
    if (p.length() == 0) {
      // Index said there was a file and the card disagrees. Filesystem is
      // ground truth — resync rather than fabricate.
      _resyncIndexUnlocked();
      return b;
    }
    snprintf(b.path, sizeof(b.path), "%s", p.c_str());
    b.records = _countRecordsUnlocked(p);
    b.valid   = true;
    return b;
  }

  // Reads records [from, from+maxN) out of a batch file into typed records.
  // Returning 0 for a file that exists is an ERROR, never a completion
  // signal — v5.2 treated it as "file fully pushed" and deleted the file
  // (root cause R1).
  int readRecords(const char* path, uint16_t from, uint16_t maxN, GpsRecord* out) {
    Guard g(_mtx);
    if (!_ok || !path || !path[0]) return -1;
    File f = SD.open(path, FILE_READ);
    if (!f) { LOGW("SD", "readRecords: cannot open %s", path); return -1; }

    uint16_t idx = 0; int got = 0;
    while (f.available() && got < (int)maxN) {
      String ln = f.readStringUntil('\n');
      ln.trim();
      if (ln.length() < 8) continue;
      if (idx++ < from) continue;
      if (_parse(ln, out[got])) got++;
    }
    f.close();
    return got;
  }

  // A delivered, verified batch. Archived (or deleted) — never both.
  bool retireBatch(const char* path) {
    Guard g(_mtx);
    if (!_ok || !path || !path[0]) return false;
    bool done;
#if KEEP_SENT_ARCHIVE
    String dst = String(DIR_SENT) + "/" + _basename(String(path));
    done = SD.rename(path, dst.c_str());
    if (!done) done = SD.remove(path);   // rename unsupported → delete
#else
    done = SD.remove(path);
#endif
    if (done && _queueFiles > 0) _queueFiles--;
    return done;
  }

  void setTransmitting(const char* path) {
    Guard g(_mtx);
    _transmitting = path ? String(path) : String("");
  }

  // ---- periodic maintenance ---------------------------------------------
  void resyncIndex() { Guard g(_mtx); _resyncIndexUnlocked(); }

  void evictIfNeeded() { Guard g(_mtx); _evictIfNeededUnlocked(0); }

  int usedPercent() {
    Guard g(_mtx);
    if (!_ok) return 0;
    uint64_t t = SD.totalBytes();
    return t ? (int)((SD.usedBytes() * 100ULL) / t) : 0;
  }
  uint64_t totalBytes() { Guard g(_mtx); return _ok ? SD.totalBytes() : 0; }
  uint64_t usedBytes()  { Guard g(_mtx); return _ok ? SD.usedBytes()  : 0; }

  // ---- dashboard / console helpers --------------------------------------
  struct LogStats {
    uint32_t fileCount, totalLines;
    String   oldestFile, newestFile;
    uint64_t totalLogBytes;
  };

  LogStats getStats() {
    Guard g(_mtx);
    LogStats s = {0, 0, "", "", 0};
    if (!_ok) return s;
    const char* dirs[2] = { DIR_QUEUE, DIR_SENT };
    for (int d = 0; d < 2; d++) {
      File dir = SD.open(dirs[d]);
      if (!dir) continue;
      File e = dir.openNextFile();
      while (e) {
        String base = _basename(String(e.name()));
        if (base.endsWith(".jsonl")) {
          String full = String(dirs[d]) + "/" + base;
          s.fileCount++;
          s.totalLogBytes += e.size();
          s.totalLines += _countRecordsUnlocked(full);
          if (!s.oldestFile.length() || base < _basename(s.oldestFile)) s.oldestFile = full;
          if (!s.newestFile.length() || base > _basename(s.newestFile)) s.newestFile = full;
        }
        e.close();
        e = dir.openNextFile();
        wdtFeed();
      }
      dir.close();
    }
    return s;
  }

  String listFilesJSON() {
    Guard g(_mtx);
    static char out[4096];
    out[0] = 0;
    strcat(out, "[");
    if (!_ok) { strcat(out, "]"); return String(out); }
    bool first = true;
    const char* dirs[2] = { DIR_QUEUE, DIR_SENT };
    const char* tags[2] = { "PENDING", "SENT" };
    for (int d = 0; d < 2; d++) {
      File dir = SD.open(dirs[d]);
      if (!dir) continue;
      File e = dir.openNextFile();
      while (e) {
        String base = _basename(String(e.name()));
        if (base.endsWith(".jsonl")) {
          String full = String(dirs[d]) + "/" + base;
          uint32_t lines = _countRecordsUnlocked(full);
          char item[320];
          snprintf(item, sizeof(item),
            "%s{\"file\":\"%s\",\"name\":\"%s %s\",\"state\":\"%s\",\"size\":%lu,\"records\":%lu}",
            first ? "" : ",", full.c_str(), tags[d], base.c_str(), tags[d],
            (unsigned long)e.size(), (unsigned long)lines);
          if (strlen(out) + strlen(item) + 2 < sizeof(out)) { strcat(out, item); first = false; }
        }
        e.close();
        e = dir.openNextFile();
        wdtFeed();
      }
      dir.close();
    }
    strcat(out, "]");
    return String(out);
  }

  uint32_t countLines(const String& path) { Guard g(_mtx); return _countRecordsUnlocked(path); }

  int listAllLogPaths(String* out, int maxN) {
    Guard g(_mtx);
    int n = 0;
    if (!_ok) return 0;
    const char* dirs[2] = { DIR_QUEUE, DIR_SENT };
    for (int d = 0; d < 2 && n < maxN; d++) {
      File dir = SD.open(dirs[d]);
      if (!dir) continue;
      File e = dir.openNextFile();
      while (e && n < maxN) {
        String base = _basename(String(e.name()));
        if (base.endsWith(".jsonl")) out[n++] = String(dirs[d]) + "/" + base;
        e.close();
        e = dir.openNextFile();
      }
      dir.close();
    }
    std::sort(out, out + n);
    return n;
  }

  // Two-pass window read for the dashboard. Pass 1 counts matches, pass 2
  // streams only the requested window, so memory is bounded by pageSize
  // regardless of file size.
  String readPage(const String& path, uint32_t offset, int pageSize,
                  bool newestFirst, const String& search = "") {
    Guard g(_mtx);
    if (!_ok || !_isSafePath(path)) return "[]";
    if (pageSize < 1)   pageSize = 1;
    if (pageSize > 100) pageSize = 100;

    uint32_t matches = 0;
    {
      File f = SD.open(path, FILE_READ);
      if (!f) return "[]";
      while (f.available()) {
        wdtFeed();
        String ln = f.readStringUntil('\n'); ln.trim();
        if (ln.length() < 8) continue;
        if (search.length() && ln.indexOf(search) == -1) continue;
        matches++;
      }
      f.close();
    }
    if (!matches) return "[]";

    uint32_t startIdx, endIdx;
    if (newestFirst) {
      endIdx   = (matches > offset) ? (matches - offset) : 0;
      startIdx = (endIdx > (uint32_t)pageSize) ? (endIdx - (uint32_t)pageSize) : 0;
    } else {
      startIdx = offset;
      endIdx   = offset + (uint32_t)pageSize;
      if (endIdx > matches) endIdx = matches;
    }
    if (endIdx <= startIdx) return "[]";

    String sel[100]; int selN = 0;
    {
      File f = SD.open(path, FILE_READ);
      if (!f) return "[]";
      uint32_t idx = 0;
      while (f.available() && idx < endIdx) {
        wdtFeed();
        String ln = f.readStringUntil('\n'); ln.trim();
        if (ln.length() < 8) continue;
        if (search.length() && ln.indexOf(search) == -1) continue;
        if (idx >= startIdx && selN < 100) sel[selN++] = ln;
        idx++;
      }
      f.close();
    }

    String out = "[";
    if (newestFirst) {
      for (int i = selN - 1; i >= 0; i--) { if (i != selN - 1) out += ","; out += sel[i]; }
    } else {
      for (int i = 0; i < selN; i++) { if (i) out += ","; out += sel[i]; }
    }
    out += "]";
    return out;
  }

  bool deleteFile(const String& path) {
    Guard g(_mtx);
    if (!_ok || !_isSafePath(path)) return false;
    if (path == _transmitting) { LOGW("SD", "refusing to delete in-flight batch %s", path.c_str()); return false; }
    bool wasQueue = path.startsWith(DIR_QUEUE);
    bool wasTail  = (path == _tailPath);
    bool r = SD.remove(path);
    if (r) {
      if (wasTail) { _tailPath = ""; _tailRecords = 0; }
      else if (wasQueue && _queueFiles > 0) _queueFiles--;
    }
    return r;
  }

  void deleteAll() {
    Guard g(_mtx);
    if (!_ok) return;
    const char* dirs[2] = { DIR_QUEUE, DIR_SENT };
    for (int d = 0; d < 2; d++) {
      String names[96]; int n = 0;
      File dir = SD.open(dirs[d]);
      if (!dir) continue;
      File e = dir.openNextFile();
      while (e && n < 96) { names[n++] = _basename(String(e.name())); e.close(); e = dir.openNextFile(); }
      dir.close();
      for (int i = 0; i < n; i++) SD.remove(String(dirs[d]) + "/" + names[i]);
    }
    _tailPath = ""; _tailRecords = 0; _queueFiles = 0; _totalRecords = 0;
    LOGW("SD", "All log files deleted");
  }

  String tailPath() { Guard g(_mtx); return _tailPath; }

  // ---- record (de)serialisation, shared with the sync engine ------------
  static int _serialise(const GpsRecord& r, char* out, size_t cap) {
    return snprintf(out, cap,
      "{\"seq\":%lu,\"ts\":\"%s\",\"lat\":%.7f,\"lng\":%.7f,\"alt\":%.2f,"
      "\"spd\":%.2f,\"hdg\":%.2f,\"sats\":%u,\"hdop\":%.2f,\"bat\":%u,"
      "\"net\":\"%s\",\"fix\":%u,\"tsrc\":%u}",
      (unsigned long)r.seq, r.ts, r.lat, r.lng, r.alt, r.spd, r.hdg,
      (unsigned)r.sats, r.hdop, (unsigned)r.batPct, netName(r.net),
      (unsigned)r.fix, (unsigned)r.tsrc);
  }

  static bool _parse(const String& ln, GpsRecord& r) {
    memset(&r, 0, sizeof(r));
    r.seq  = (uint32_t)_jNum(ln, "\"seq\":");
    r.lat  = _jNum(ln, "\"lat\":");
    r.lng  = _jNum(ln, "\"lng\":");
    r.alt  = (float)_jNum(ln, "\"alt\":");
    r.spd  = (float)_jNum(ln, "\"spd\":");
    r.hdg  = (float)_jNum(ln, "\"hdg\":");
    r.sats = (uint16_t)_jNum(ln, "\"sats\":");
    r.hdop = (float)_jNum(ln, "\"hdop\":");
    r.batPct = (uint8_t)_jNum(ln, "\"bat\":");
    r.fix  = (uint8_t)_jNum(ln, "\"fix\":");
    String ts = _jStr(ln, "\"ts\":\"");
    String nt = _jStr(ln, "\"net\":\"");
    snprintf(r.ts, sizeof(r.ts), "%s", ts.length() ? ts.c_str() : TS_UNSYNCED);
    r.net = nt.equals("WIFI") ? NET_WIFI : nt.equals("SIM") ? NET_SIM : NET_NONE;

    // MIGRATION. Cards written by v6.0 and earlier have no "tsrc" field, and
    // their unsynced records carry the old 1970 literal. Infer rather than
    // discard: a real-looking stamp in a legacy file came from GPS, because
    // GPS was the only source that existed. Records already in the FIFO when
    // the device is reflashed therefore keep their meaning.
    if (ln.indexOf("\"tsrc\":") >= 0) {
      r.tsrc = (uint8_t)_jNum(ln, "\"tsrc\":");
    } else {
      r.tsrc = _tsIsReal(r.ts) ? TSRC_GPS : TSRC_NONE;
    }
    if (!_tsIsReal(r.ts)) {
      // Normalise the legacy 1970 sentinel to the marker that cannot be
      // mistaken for an instant, so replayed backlog is honest on the wire.
      snprintf(r.ts, sizeof(r.ts), "%s", TS_UNSYNCED);
      r.tsrc = TSRC_NONE;
    }
    return ts.length() > 0;
  }

  // "Real" means a stamp that denotes an actual instant: not the v6.1 marker
  // and not the v6.0 1970 sentinel.
  static bool _tsIsReal(const char* ts) {
    if (!ts || strlen(ts) < 19) return false;
    if (!strncmp(ts, "0000", 4)) return false;
    if (!strncmp(ts, "1970-01-01 00:00:00", 19)) return false;
    return isdigit((unsigned char)ts[0]) != 0;
  }

  static double _jNum(const String& j, const char* key) {
    int i = j.indexOf(key);
    if (i < 0) return 0.0;
    int s = i + strlen(key), e = s;
    while (e < (int)j.length() && (isdigit(j[e]) || j[e] == '.' || j[e] == '-' || j[e] == '+')) e++;
    return j.substring(s, e).toDouble();
  }

  static String _jStr(const String& j, const char* key) {
    int i = j.indexOf(key);
    if (i < 0) return "";
    int s = i + strlen(key), e = j.indexOf('"', s);
    return (e < 0) ? String("") : j.substring(s, e);
  }

private:
  // RAII lock. Recursive so a public method may call another public method.
  struct Guard {
    SemaphoreHandle_t h;
    explicit Guard(SemaphoreHandle_t m) : h(m) { if (h) xSemaphoreTakeRecursive(h, portMAX_DELAY); }
    ~Guard() { if (h) xSemaphoreGiveRecursive(h); }
  };

  SemaphoreHandle_t _mtx = nullptr;
  Preferences _nvs;
  uint8_t  _cs = 5;
  bool     _ok = false, _autoDelete = true;
  int      _maxPct = 80;
  uint16_t _recPerFile = 30;
  String   _tailPath = "", _transmitting = "";
  uint16_t _tailRecords = 0, _queueFiles = 0;
  uint32_t _nextFileSeq = 1, _totalRecords = 0, _evicted = 0, _writeErrors = 0;
  uint32_t _recSeq = 0;
  uint16_t _sinceSeqPersist = 0;

  static String _basename(const String& p) {
    int s = p.lastIndexOf('/');
    return (s >= 0) ? p.substring(s + 1) : p;
  }

  static bool _isSafePath(const String& p) {
    return p.startsWith(DIR_QUEUE "/") || p.startsWith(DIR_SENT "/");
  }

  void _ensureDirs() {
    if (!SD.exists(LOG_ROOT))  SD.mkdir(LOG_ROOT);
    if (!SD.exists(DIR_QUEUE)) SD.mkdir(DIR_QUEUE);
#if KEEP_SENT_ARCHIVE
    if (!SD.exists(DIR_SENT))  SD.mkdir(DIR_SENT);
#endif
  }

  bool _healthCheckUnlocked() {
    if (_ok) return true;
    if (SD.begin(_cs)) { _ok = true; _ensureDirs(); _recoverTail(); _resyncIndexUnlocked(); return true; }
    return false;
  }

  static bool _parseSeq(const String& base, uint32_t& seq) {
    if (base.length() < 3 || base[0] != 'Q') return false;
    int u = base.indexOf('_');
    if (u < 2) return false;
    String d = base.substring(1, u);
    for (size_t i = 0; i < d.length(); i++) if (!isdigit(d[i])) return false;
    seq = (uint32_t)strtoul(d.c_str(), nullptr, 10);
    return true;
  }

  String _makeName(uint32_t seq, const char* stamp) {
    char b[80];
    snprintf(b, sizeof(b), "%s/Q%08lu_%s.jsonl", DIR_QUEUE, (unsigned long)seq, stamp);
    return String(b);
  }

  // Walks QUEUE once. Used at boot, on remount and every 5 minutes — never
  // in the per-record path.
  void _resyncIndexUnlocked() {
    _queueFiles = 0; _nextFileSeq = 1;
    if (!_ok) return;
    File dir = SD.open(DIR_QUEUE);
    if (!dir) return;
    uint32_t highest = 0, count = 0;
    File e = dir.openNextFile();
    while (e) {
      String base = _basename(String(e.name()));
      uint32_t s;
      if (base.endsWith(".jsonl") && _parseSeq(base, s)) {
        count++;
        if (s > highest) highest = s;
      }
      e.close();
      e = dir.openNextFile();
      wdtFeed();
    }
    dir.close();
    _nextFileSeq = highest + 1;
    // The tail (partial) file is on disk too but is not a sealed batch.
    if (_tailPath.length() && SD.exists(_tailPath)) _queueFiles = (count > 0) ? count - 1 : 0;
    else                                            _queueFiles = count;
  }

  // Boot recovery: if the newest queue file is partial, keep appending to
  // it rather than orphaning those records in a file that will never fill.
  void _recoverTail() {
    _tailPath = ""; _tailRecords = 0;
    if (!_ok) return;
    File dir = SD.open(DIR_QUEUE);
    if (!dir) return;
    uint32_t newest = 0; String newestBase = "";
    File e = dir.openNextFile();
    while (e) {
      String base = _basename(String(e.name()));
      uint32_t s;
      if (base.endsWith(".jsonl") && _parseSeq(base, s) && (newestBase.length() == 0 || s > newest)) {
        newest = s; newestBase = base;
      }
      e.close();
      e = dir.openNextFile();
    }
    dir.close();
    if (!newestBase.length()) return;
    String full = String(DIR_QUEUE) + "/" + newestBase;
    uint32_t n = _countRecordsUnlocked(full);
    if (n < _recPerFile) {
      _tailPath = full;
      _tailRecords = (uint16_t)n;
      LOGW("SD", "Recovered partial batch %s (%u/%u records)",
           full.c_str(), _tailRecords, _recPerFile);
    }
  }

  // Filesystem is ground truth for the record sequence; NVS is the fallback
  // for when the queue is empty because everything was delivered.
  void _reconcileSeqFromDisk() {
    if (!_ok) return;
    uint32_t fromDisk = 0;
    String paths[96];
    int n = 0;
    const char* dirs[2] = { DIR_QUEUE, DIR_SENT };
    for (int d = 0; d < 2 && n < 96; d++) {
      File dir = SD.open(dirs[d]);
      if (!dir) continue;
      File e = dir.openNextFile();
      while (e && n < 96) {
        String base = _basename(String(e.name()));
        if (base.endsWith(".jsonl")) paths[n++] = String(dirs[d]) + "/" + base;
        e.close(); e = dir.openNextFile();
      }
      dir.close();
    }
    std::sort(paths, paths + n);
    if (n > 0) {
      File f = SD.open(paths[n - 1], FILE_READ);
      if (f) {
        String last = "";
        while (f.available()) { String l = f.readStringUntil('\n'); l.trim(); if (l.length() > 8) last = l; }
        f.close();
        if (last.length()) fromDisk = (uint32_t)_jNum(last, "\"seq\":");
      }
    }
    if (fromDisk > _recSeq) _recSeq = fromDisk;
    _nvs.putULong("recseq", _recSeq);
    LOGW("SD", "Record sequence recovered: %lu (disk %lu)",
         (unsigned long)_recSeq, (unsigned long)fromDisk);
  }

  uint32_t _countRecordsUnlocked(const String& path) {
    if (!_ok || !path.length()) return 0;
    File f = SD.open(path, FILE_READ);
    if (!f) return 0;
    uint32_t n = 0;
    while (f.available()) {
      String l = f.readStringUntil('\n');
      l.trim();
      if (l.length() > 8) n++;
      if ((n & 0x7F) == 0) wdtFeed();
    }
    f.close();
    return n;
  }

  String _findOldestQueuePathUnlocked(uint32_t& seqOut) {
    File dir = SD.open(DIR_QUEUE);
    if (!dir) return "";
    uint32_t oldest = 0; String oldestBase = "";
    File e = dir.openNextFile();
    while (e) {
      String base = _basename(String(e.name()));
      uint32_t s;
      if (base.endsWith(".jsonl") && _parseSeq(base, s)) {
        String full = String(DIR_QUEUE) + "/" + base;
        // The tail is still being appended to — never hand it to the sync
        // engine until sealTail() has closed it.
        if (full != _tailPath && (oldestBase.length() == 0 || s < oldest)) {
          oldest = s; oldestBase = base;
        }
      }
      e.close();
      e = dir.openNextFile();
    }
    dir.close();
    if (!oldestBase.length()) return "";
    seqOut = oldest;
    return String(DIR_QUEUE) + "/" + oldestBase;
  }

  bool _ensureTailUnlocked() {
    if (_tailPath.length() && SD.exists(_tailPath)) return true;
    // Capacity is checked here — once per batch file — instead of on every
    // record: SD.usedBytes() walks the FAT and costs hundreds of ms.
    _evictIfNeededUnlocked((uint32_t)REC_LINE_MAX * _recPerFile);
    if (_nextFileSeq == 0) _nextFileSeq = 1;
    char stamp[20];
    _stampNow(stamp, sizeof(stamp));
    _tailPath = _makeName(_nextFileSeq++, stamp);
    _tailRecords = 0;
    File f = SD.open(_tailPath, FILE_WRITE);
    if (!f) {
      LOGW("SD", "Failed to create batch file %s", _tailPath.c_str());
      _tailPath = "";
      return false;
    }
    f.close();
    LOGW("SD", "New batch file: %s", _tailPath.c_str());
    return true;
  }

  void _sealTailUnlocked() {
    if (!_tailPath.length()) return;
    LOGW("SD", "Batch SEALED: %s (%u records)", _tailPath.c_str(), _tailRecords);
    _tailPath = "";
    _tailRecords = 0;
    _queueFiles++;
  }

  // Capacity policy (report section: storage). Evict SENT first — it is
  // verified-delivered and therefore genuinely safe. Only if no SENT
  // remains do we drop the OLDEST pending batch, and that is counted and
  // announced, never silent. The tail and the in-flight batch are never
  // touched, and logging is never stopped.
  void _evictIfNeededUnlocked(uint32_t needBytes) {
    if (!_ok || !_autoDelete) return;
    uint64_t total = SD.totalBytes();
    if (!total) return;
    int guardTrips = 0;
    while (((SD.usedBytes() + needBytes) * 100ULL) / total >= (uint64_t)_maxPct) {
      if (++guardTrips > 8) break;      // bounded: never spin on a full card
#if KEEP_SENT_ARCHIVE
      String victim = _oldestInDirUnlocked(DIR_SENT);
      if (victim.length()) {
        SD.remove(victim);
        LOGW("SD", "Storage %d%% — evicted delivered archive %s", _maxPct, victim.c_str());
        continue;
      }
#endif
      uint32_t s = 0;
      String pend = _findOldestQueuePathUnlocked(s);
      if (!pend.length() || pend == _transmitting) {
        LOGW("SD", "Storage at cap and nothing safe to evict — logging continues, "
                   "card needs servicing");
        return;
      }
      uint32_t lost = _countRecordsUnlocked(pend);
      SD.remove(pend);
      if (_queueFiles > 0) _queueFiles--;
      _evicted += lost;
      LOGW("SD", "STORAGE FULL — DROPPED %lu UNDELIVERED RECORDS (%s). "
                 "Total dropped this boot: %lu",
           (unsigned long)lost, pend.c_str(), (unsigned long)_evicted);
    }
  }

  String _oldestInDirUnlocked(const char* d) {
    File dir = SD.open(d);
    if (!dir) return "";
    String oldestBase = "";
    File e = dir.openNextFile();
    while (e) {
      String base = _basename(String(e.name()));
      if (base.endsWith(".jsonl") && (oldestBase.length() == 0 || base < oldestBase))
        oldestBase = base;
      e.close();
      e = dir.openNextFile();
    }
    dir.close();
    return oldestBase.length() ? (String(d) + "/" + oldestBase) : String("");
  }

  void _stampNow(char* out, size_t cap);   // defined after timeSvc exists
};

// ============================================================================
//  SyncEngine — bulk delivery of the SD FIFO, oldest first, ACK-gated.
//
//    IDLE → PREPARING → UPLOADING → WAITING_ACK ─ACK→ ADVANCE → PREPARING…
//                            ▲                  └timeout→ RETRYING ┘
//
//  Progress is persisted after every verified batch:
//    fifo/syncSeq = sequence number of the batch file in progress
//    fifo/syncOff = records within that file already acknowledged
//  so a reboot mid-drain resumes at the right record instead of replaying
//  the file from zero (root cause R2). The file is retired only when the
//  offset reaches its record count.
//
//  NO TRUNCATION: the record count for a batch is derived from the byte
//  budget BEFORE serialising, so a batch can never contain fewer records
//  than the offset is advanced by (root cause R4).
//
//  v6.1 BUDGET NOTE. The two time-validity fields added to the wire record
//  push its worst case from 427 to 468 bytes, which overruns the old
//  REC_JSON_MAX of 448 and would have silently re-armed R4. REC_JSON_MAX is
//  therefore 496, and PAYLOAD_CAP rises 14336 -> 15360 so that
//  (PAYLOAD_CAP - PAYLOAD_HDR) / REC_JSON_MAX still yields 30 records per
//  batch. Batch size is unchanged; the extra kilobyte is static, not heap.
// ============================================================================

#define PAYLOAD_CAP   15360        // static, never heap — no fragmentation
#define REC_JSON_MAX  496          // worst-case one backend record + comma
#define PAYLOAD_HDR   96

static char g_payload[PAYLOAD_CAP];

class LEDManager;
extern LEDManager leds;

class SyncEngine {
public:
  volatile SyncStatus status = SYNC_IDLE;
  uint32_t pendingRecords = 0, syncedRecords = 0;
  uint32_t currentBatch = 0, totalInBatch = 0;
  float    uploadSpeed = 0.0f;
  uint32_t estimatedSec = 0;
  unsigned long lastSyncOk = 0, lastSyncFail = 0;
  String   lastError = "";
  bool     paused = false;

  void begin(SDStore* sd, ConfigManager* cfg, E2EVerifier* e2e) {
    _sd = sd; _cfg = cfg; _e2e = e2e;
    _persistedSeq = _sd->nvsGet("syncSeq", 0);
    _persistedOff = _sd->nvsGet("syncOff", 0);
    if (_persistedSeq) 
      LOGW("SYNC", "Resuming persisted progress: batch %lu, %lu record(s) already ACKed",
           (unsigned long)_persistedSeq, (unsigned long)_persistedOff);
  }

  void attachMqtt(PubSubClient* m) { _mqtt = m; }

  void resume(PubSubClient* m) {
    _mqtt = m; paused = false;
    // Do NOT reset the in-file offset here — that is exactly what made
    // v5.2 re-send acknowledged records on every reconnect flap.
    if (status == SYNC_PAUSED || status == SYNC_FAILED) status = SYNC_IDLE;
    _idleCheck = 0;
    DBG("SYNC", "Resumed");
  }

  void pause() { paused = true; status = SYNC_PAUSED; DBG("SYNC", "Paused"); }

  void triggerNow() { if (!paused) { _idleCheck = 0; if (status == SYNC_IDLE || status == SYNC_COMPLETED || status == SYNC_FAILED) status = SYNC_PREPARING; } }

  bool busy() const {
    return status == SYNC_PREPARING || status == SYNC_UPLOADING ||
           status == SYNC_WAITING_ACK || status == SYNC_RETRYING;
  }

  // Network task only.
  void tick() {
    if (paused || !_sd || !_sd->isOk()) return;
    if (!_mqtt || !_mqtt->connected()) {
      if (busy()) { status = SYNC_PAUSED; lastError = "Link down mid-batch"; }
      return;
    }
    unsigned long now = millis();

    switch (status) {
      case SYNC_IDLE:
      case SYNC_COMPLETED:
        if (now - _idleCheck > 5000UL) {
          _idleCheck = now;
          if (_sd->hasBacklog()) {
            // Seal the tail so the remainder of a short outage can be sent
            // instead of sitting in a file that will never fill.
            if (_sd->queueFiles() == 0) _sd->sealTail();
            status = SYNC_PREPARING;
          } else {
            status = SYNC_IDLE;
          }
        }
        break;

      case SYNC_PREPARING:  _prepare();  break;
      case SYNC_UPLOADING:  _upload();   break;

      case SYNC_WAITING_ACK:
        if (_e2e->pbAckOk)        _onAck();
        else if (now > _ackDue)   _onTimeout();
        break;

      case SYNC_RETRYING:
        if (now - _retryAt >= _retryDelay) {
          LOGW("SYNC", "Retry %d/%d for %s[%u..]", _retries, MAX_RETRIES,
               _batch.path, _offset);
          // Resume at the stage that actually failed. Resuming a prepare
          // failure at UPLOADING would re-publish the PREVIOUS batch's
          // payload, which is still sitting in g_payload.
          status = _retryToPrepare ? SYNC_PREPARING : SYNC_UPLOADING;
        }
        break;

      case SYNC_FAILED:
        // Never a dead end: back off long, then try again. Data is still
        // on the card and must eventually leave.
        if (now - _retryAt >= FAILED_COOLDOWN_MS) { _retries = 0; _retryDelay = 5000; status = SYNC_PREPARING; }
        break;

      case SYNC_PAUSED:
        break;
    }
  }

  // Public so /api/playback_view and the console PLAY command render the
  // byte-identical payload the broker will receive.
  size_t buildBatchPayload(const GpsRecord* recs, int n, uint32_t batchId,
                           char* out, size_t cap) {
    const char* uid = _cfg->cfg.deviceId.c_str();
    int pos = 0;
    if (_cfg->cfg.ackMode == 1)
      pos += snprintf(out + pos, cap - pos,
                      "{\"username\":\"%s\",\"batchId\":%lu,\"modelList\":[",
                      uid, (unsigned long)batchId);
    else
      pos += snprintf(out + pos, cap - pos,
                      "{\"username\":\"%s\",\"modelList\":[", uid);

    for (int i = 0; i < n; i++) {
      const GpsRecord& r = recs[i];
      pos += snprintf(out + pos, cap - pos,
        "%s{\"username\":\"%s\",\"appCode\":\"DEMO\",\"latitude\":%.7f,"
        "\"longitude\":%.7f,\"platform\":\"IoMT\",\"visitDate\":\"%s\","
        "\"visitTime\":\"%s\",\"networkType\":\"%s\",\"broadcastEnabled\":true,"
        "\"locationAccuracy\":%.2f,\"altitude_msl\":%.2f,\"speed_kmph\":%.2f,"
        "\"heading_deg\":%.2f,\"satellite_count\":%d,\"internet_available\":false,"
        "\"batteryPower\":%d,\"status\":\"offline\","
        "\"time_valid\":%s,\"time_src\":\"%s\"}",
        i ? "," : "", uid, r.lat, r.lng, r.ts, r.ts, netName(r.net),
        r.hdop, r.alt, r.spd, r.hdg, (int)r.sats, (int)r.batPct,
        r.tsrc == TSRC_NONE ? "false" : "true", timeSrcName(r.tsrc));
    }
    pos += snprintf(out + pos, cap - pos, "]}");
    return (size_t)pos;
  }

  uint16_t offsetInFile() const { return _offset; }
  const char* currentFile() const { return _batch.path; }

private:
  SDStore*       _sd   = nullptr;
  ConfigManager* _cfg  = nullptr;
  E2EVerifier*   _e2e  = nullptr;
  PubSubClient*  _mqtt = nullptr;

  SDStore::BatchInfo _batch = {};
  GpsRecord _recs[30];
  int       _nInBatch = 0;
  size_t    _payloadLen = 0;
  uint16_t  _offset = 0;
  uint32_t  _persistedSeq = 0, _persistedOff = 0;

  int           _retries = 0;
  bool          _retryToPrepare = false;
  unsigned long _retryAt = 0, _retryDelay = 5000;
  unsigned long _idleCheck = 0, _batchStart = 0, _ackDue = 0;

  static const unsigned long MAX_RETRY_DELAY   = 300000UL;
  static const unsigned long ACK_TIMEOUT_MS    = 15000UL;
  static const unsigned long FAILED_COOLDOWN_MS= 60000UL;
  static const int           MAX_RETRIES       = 10;

  void _prepare() {
    if (!_batch.valid) {
      _batch = _sd->oldestBatch();
      if (!_batch.valid) {
        status = _sd->hasBacklog() ? SYNC_IDLE : SYNC_COMPLETED;
        if (status == SYNC_COMPLETED) {
          pendingRecords = 0;
          lastSyncOk = millis();
          DBG("SYNC", "Backlog empty — live mode");
        }
        return;
      }
      _sd->setTransmitting(_batch.path);
      // Resume inside this file if we were part-way through it before a
      // reboot or a link flap.
      _offset = (_persistedSeq == _batch.seq) ? (uint16_t)_persistedOff : 0;
      if (_offset) LOGW("SYNC", "Resuming %s at record %u/%u",
                        _batch.path, _offset, _batch.records);
      else         LOGW("SYNC", "Draining %s (%u records)", _batch.path, _batch.records);
    }

    if (_batch.records == 0) {
      // An empty file is the only case where discarding without sending is
      // correct — and it is announced, not silent.
      LOGW("SYNC", "Batch %s contains 0 records — retiring empty file", _batch.path);
      _sd->retireBatch(_batch.path);
      _clearBatch();
      return;
    }

    if (_offset >= _batch.records) { _finishFile(); return; }

    // Byte budget first, record count second. This is what makes silent
    // truncation structurally impossible.
    int budgetMax = (int)((PAYLOAD_CAP - PAYLOAD_HDR) / REC_JSON_MAX);
    if (budgetMax < 1) budgetMax = 1;
    int want = _batch.records - _offset;
    if (want > _cfg->cfg.batchSize) want = _cfg->cfg.batchSize;
    if (want > budgetMax)           want = budgetMax;
    if (want > (int)(sizeof(_recs) / sizeof(_recs[0])))
      want = (int)(sizeof(_recs) / sizeof(_recs[0]));

    int got = _sd->readRecords(_batch.path, _offset, (uint16_t)want, _recs);
    if (got <= 0) {
      // File exists per the index but cannot be read (removed from the
      // dashboard, card yanked, corrupt entry). NEVER delete it — v5.2 did
      // exactly that and lost the whole outage. Release it and let the next
      // PREPARING re-derive the oldest file from the filesystem.
      _retryToPrepare = true;
      _batch.valid = false;
      _batch.path[0] = 0;
      _sd->setTransmitting("");
      _fail(got == 0 ? "Batch readable but empty at offset" : "Batch file unreadable");
      _sd->resyncIndex();
      return;
    }
    _retryToPrepare = false;

    _nInBatch    = got;
    _payloadLen  = buildBatchPayload(_recs, got, _batch.seq, g_payload, PAYLOAD_CAP);
    totalInBatch = got;
    _batchStart  = millis();
    status       = SYNC_UPLOADING;
  }

  void _upload();   // defined after globals (needs leds + mqttEnsureBuffer)

  void _onAck() {
    _offset       += _nInBatch;
    syncedRecords += _nInBatch;
    currentBatch   = _nInBatch;

    // Persist BEFORE anything else: if power dies right here we must not
    // re-send what the broker already confirmed.
    _persistedSeq = _batch.seq;
    _persistedOff = _offset;
    _sd->nvsPut("syncSeq", _persistedSeq);
    _sd->nvsPut("syncOff", _persistedOff);

    unsigned long el = millis() - _batchStart;
    if (el > 0) uploadSpeed = _nInBatch * 1000.0f / el;

    pendingRecords = (uint32_t)(_batch.records - _offset) +
                     _sd->queueFiles() * (uint32_t)_cfg->cfg.batchSize +
                     _sd->tailRecords();
    estimatedSec = (uploadSpeed > 0.01f) ? (uint32_t)(pendingRecords / uploadSpeed) : 0;
    _retries = 0; _retryDelay = 5000; lastSyncOk = millis();
    LOGW("SYNC", "Batch VERIFIED — %d record(s), %s at %u/%u, %.1f rec/s",
         _nInBatch, _batch.path, _offset, _batch.records, uploadSpeed);

    if (_offset >= _batch.records) _finishFile();
    else                           status = SYNC_PREPARING;
  }

  void _finishFile() {
    if (!_sd->retireBatch(_batch.path))
      LOGW("SYNC", "WARNING: %s delivered but could not be retired — it may be "
                   "re-sent; backend dedup on (username, visitDate) applies", _batch.path);
    _sd->nvsPut("syncSeq", 0);
    _sd->nvsPut("syncOff", 0);
    _persistedSeq = 0; _persistedOff = 0;
    LOGW("SYNC", "Batch file fully delivered and retired: %s", _batch.path);
    _clearBatch();
  }

  void _clearBatch() {
    _sd->setTransmitting("");
    _batch.valid = false;
    _batch.path[0] = 0;
    _offset = 0;
    _nInBatch = 0;
    status = SYNC_PREPARING;
    pendingRecords = _sd->queueFiles() * _cfg->cfg.batchSize + _sd->tailRecords();
  }

  void _onTimeout() {
    _e2e->pbPending = false;
    _fail("Delivery unverified (no broker echo / backend ACK)");
  }

  void _fail(const char* why) {
    _retries++;
    lastSyncFail = millis();
    lastError = why;
    if (_retries >= MAX_RETRIES) {
      status = SYNC_FAILED;
      _retryAt = millis();
      LOGW("SYNC", "FAILED after %d attempts (%s) — data retained, cooldown %lus",
           _retries, why, FAILED_COOLDOWN_MS / 1000UL);
      return;
    }
    _retryDelay = (_retryDelay * 2 > MAX_RETRY_DELAY) ? MAX_RETRY_DELAY : _retryDelay * 2;
    _retryAt    = millis();
    status      = SYNC_RETRYING;
    LOGW("SYNC", "%s — retry in %lums (%d/%d). Nothing deleted.",
         why, _retryDelay, _retries, MAX_RETRIES);
  }
};

// ============================================================================
//  NetworkDiag — v5.2 structure retained. v5.2 left sim.networkType empty,
//  which is why the SIM diagnostics panel rendered blank; it is now filled
//  from AT+CPSI? (access technology as reported by the modem itself).
// ============================================================================
struct WiFiInfo { int rssi = 0, signalPct = 0; String ip, gateway, dns, ssid; };
struct SIMInfo  {
  int rssi = 0, signalPct = 0;
  String imei, iccid, operator_, networkType, apn, ip;
  bool roaming = false;
  String regStatus = "Unknown";
  String balance = "Not Supported by Carrier";
  String dataRemain = "Not Supported by Carrier";
};

class NetworkDiag {
public:
  WiFiInfo wifi;
  SIMInfo  sim;
  unsigned long connectionTime = 0;
  int reconnectCount = 0;
  String mqttStatus = "Disconnected";

  void begin(TinyGsm* m) { _modem = m; }

  void updateWiFiInfo() {
    wifi.rssi      = WiFi.RSSI();
    wifi.signalPct = _pct(wifi.rssi);
    wifi.ip        = WiFi.localIP().toString();
    wifi.gateway   = WiFi.gatewayIP().toString();
    wifi.dns       = WiFi.dnsIP().toString();
    wifi.ssid      = WiFi.SSID();
    connectionTime = millis();
    reconnectCount++;
    DBG("WIFI", "%s @ %d dBm (%d%%) | IP %s", wifi.ssid.c_str(), wifi.rssi,
        wifi.signalPct, wifi.ip.c_str());
  }

  // Network task only — these are blocking AT transactions.
  void updateSIMInfo(const String& accessTech) {
    if (!_modem) return;
    sim.imei      = _modem->getIMEI();
    sim.iccid     = _modem->getSimCCID();
    sim.operator_ = _modem->getOperator();
    sim.ip        = _modem->getLocalIP();
    if (accessTech.length()) sim.networkType = accessTech;

    int csq = _modem->getSignalQuality();
    sim.rssi      = (csq <= 31) ? (-113 + 2 * csq) : 0;
    sim.signalPct = (csq <= 31) ? map(csq, 0, 31, 0, 100) : 0;

    SIM7600RegStatus reg = _modem->getRegistrationStatus();
    switch (reg) {
      case REG_OK_HOME:    sim.regStatus = "Registered (Home)";    sim.roaming = false; break;
      case REG_OK_ROAMING: sim.regStatus = "Registered (Roaming)"; sim.roaming = true;  break;
      case REG_SEARCHING:  sim.regStatus = "Searching";  break;
      case REG_DENIED:     sim.regStatus = "Denied";     break;
      default:             sim.regStatus = "Unknown";
    }
    connectionTime = millis();
    DBG("SIM", "%s | %s | %d dBm (%d%%) | %s", sim.regStatus.c_str(),
        sim.operator_.c_str(), sim.rssi, sim.signalPct, sim.networkType.c_str());
  }

  String toJSON() {
    static char j[1600];
    snprintf(j, sizeof(j),
      "{\"wifi\":{\"rssi\":%d,\"signal\":%d,\"ip\":\"%s\",\"gateway\":\"%s\","
      "\"dns\":\"%s\",\"ssid\":\"%s\"},\"sim\":{\"rssi\":%d,\"signal\":%d,"
      "\"imei\":\"%s\",\"iccid\":\"%s\",\"operator_\":\"%s\",\"networkType\":\"%s\","
      "\"ip\":\"%s\",\"roaming\":%s,\"regStatus\":\"%s\",\"balance\":\"%s\","
      "\"dataRemain\":\"%s\"},\"connectedMs\":%lu,\"reconnects\":%d,\"mqttStatus\":\"%s\"}",
      wifi.rssi, wifi.signalPct, wifi.ip.c_str(), wifi.gateway.c_str(),
      wifi.dns.c_str(), wifi.ssid.c_str(), sim.rssi, sim.signalPct,
      sim.imei.c_str(), sim.iccid.c_str(), sim.operator_.c_str(),
      sim.networkType.c_str(), sim.ip.c_str(), sim.roaming ? "true" : "false",
      sim.regStatus.c_str(), sim.balance.c_str(), sim.dataRemain.c_str(),
      millis() - connectionTime, reconnectCount, mqttStatus.c_str());
    return String(j);
  }

private:
  TinyGsm* _modem = nullptr;
  int _pct(int r) { if (r >= -50) return 100; if (r <= -100) return 0; return (int)((r + 100) * 2.0f); }
};

// ============================================================================
//  WebDashboard — v5.2 route/role matrix preserved verbatim.
//   PUBLIC : /lock, /api/unlock
//   USER+DEV: /api/logout, /api/basic, /api/config, /api/save, /api/scan,
//             /api/log_files, /api/log_page, /api/playback_view
//   DEV ONLY: /api/status, /api/storage, /api/netdiag, /api/selftest,
//             /api/sync, /api/pause_sync, /api/resume_sync,
//             /api/set_batch_size, /api/restart, /api/factory_reset,
//             /api/format_sd, /api/delete_all_logs, /api/delete_log,
//             /api/download_csv, /api/download_json
//  Changes: no handler samples the ADC any more (root cause R7); the WiFi
//  scan is asynchronous so it cannot stall the HTTP task; /api/status
//  reports the new FIFO counters.
// ============================================================================

#include "dashboard_html.h"     // const char DASHBOARD_HTML[] PROGMEM = R"HTMLEOF(...)HTMLEOF";

extern TinyGPSPlus    gps;
extern TimeService    timeSvc;
extern BatteryMonitor battery;
extern E2EVerifier    e2e;
extern LEDManager     leds;
extern bool           usingWiFi;
extern volatile bool  linkOnline;
extern String         selfTestJson;
extern volatile uint32_t droppedRecords;

enum WifiPhase : uint8_t { WIFI_PH_IDLE, WIFI_PH_TRYING, WIFI_PH_UP };

class WebDashboard {
public:
  ConfigManager* _cfg  = nullptr;
  SDStore*       _sd   = nullptr;
  SyncEngine*    _sync = nullptr;
  NetworkDiag*   _net  = nullptr;
  unsigned long* _rebootAt = nullptr;

  String _sessionToken = "", _sessionMode = "";
  unsigned long _sessionCreatedAt = 0;
  static const unsigned long SESSION_TIMEOUT_MS = 3600000UL;

  int _pinFails = 0;
  unsigned long _pinLockUntil = 0;
  static const int           PIN_MAX_FAILS  = 5;
  static const unsigned long PIN_LOCKOUT_MS = 30000UL;

  void attach(ConfigManager& cfg, SDStore& sd, SyncEngine& sync,
              NetworkDiag& net, unsigned long& rebootAt) {
    _cfg = &cfg; _sd = &sd; _sync = &sync; _net = &net; _rebootAt = &rebootAt;
  }

  // Started once at boot, never stopped on connectivity loss.
  void startRuntime(WebServer& srv, ConfigManager& cfg) {
    _registerRoutes(srv, cfg);
    srv.begin();
    DBG("WEB", "Dashboard routes registered, server persistent");
  }

private:
  bool _routesRegistered = false;

  void _registerRoutes(WebServer& srv, ConfigManager& cfg) {
    if (_routesRegistered) return;
    _routesRegistered = true;

    srv.on("/", HTTP_GET, [&]() {
      if (_sessionToken.length() == 0) {
        srv.sendHeader("Location", "/lock", true);
        srv.send(302, "text/plain", "");
        return;
      }
      srv.send_P(200, "text/html", DASHBOARD_HTML);
    });

    srv.on("/lock", HTTP_GET, [&]() { srv.send_P(200, "text/html", DASHBOARD_HTML); });

    srv.on("/api/unlock", HTTP_POST, [&]() {
      if (millis() < _pinLockUntil) {
        unsigned long w = (_pinLockUntil - millis()) / 1000 + 1;
        srv.send(200, "application/json",
                 "{\"ok\":false,\"locked\":true,\"waitSec\":" + String(w) + "}");
        return;
      }
      String pinIn = srv.arg("pin");
      if (pinIn == cfg.cfg.devPin || pinIn == cfg.cfg.userPin) {
        _sessionMode      = (pinIn == cfg.cfg.devPin) ? "dev" : "user";
        _sessionToken     = _makeToken();
        _sessionCreatedAt = millis();
        _pinFails = 0;
        DBG("WEB", "Auth SUCCESS | role %s", _sessionMode.c_str());
        srv.send(200, "application/json",
          "{\"ok\":true,\"mode\":\"" + _sessionMode + "\",\"token\":\"" + _sessionToken + "\"}");
      } else {
        _pinFails++;
        if (_pinFails >= PIN_MAX_FAILS) {
          _pinLockUntil = millis() + PIN_LOCKOUT_MS;
          _pinFails = 0;
          LOGW("WEB", "PIN LOCKOUT — unlock disabled for %lus", PIN_LOCKOUT_MS / 1000);
        }
        srv.send(200, "application/json", "{\"ok\":false}");
      }
    });

    srv.on("/api/logout", HTTP_POST, [&]() {
      _sessionToken = ""; _sessionMode = "";
      srv.send(200, "application/json", "{\"ok\":true}");
    });

    // ---------------- USER + DEV ----------------
    srv.on("/api/basic", HTTP_GET, [&]() {
      if (!_authed(srv)) return;
      static char b[320];
      snprintf(b, sizeof(b),
        "{\"fix\":%s,\"sats\":%d,\"batV\":%.2f,\"batPct\":%d,\"net\":\"%s\","
        "\"sdOk\":%s,\"charging\":%s,\"chargeFull\":%s}",
        gps.location.isValid() ? "true" : "false",
        gps.satellites.isValid() ? gps.satellites.value() : 0,
        battery.volts(), battery.percent(),
        linkOnline ? (usingWiFi ? "WIFI" : "SIM") : "OFFLINE",
        _sd->isOk() ? "true" : "false",
        battery.charging()   ? "true" : "false",
        battery.chargeFull() ? "true" : "false");
      srv.send(200, "application/json", b);
    });

    // Asynchronous scan: the handler never blocks the HTTP task for the
    // 2–4 s a synchronous WiFi.scanNetworks() takes.
    srv.on("/api/scan", HTTP_GET, [&]() {
      if (!_authed(srv)) return;
      // A scan started while the STA is mid-association aborts that
      // association. Report "scanning" and let the client poll instead.
      extern WifiPhase wifiPhase;
      if (wifiPhase == WIFI_PH_TRYING) {
        srv.send(200, "application/json", "{\"networks\":[],\"scanning\":true}");
        return;
      }
      int16_t n = WiFi.scanComplete();
      if (n == WIFI_SCAN_RUNNING) { srv.send(200, "application/json",
        "{\"networks\":[],\"scanning\":true}"); return; }
      if (n == WIFI_SCAN_FAILED) {
        WiFi.scanNetworks(true, true);
        srv.send(200, "application/json", "{\"networks\":[],\"scanning\":true}");
        return;
      }
      String out = "{\"networks\":[";
      int added = 0;
      for (int i = 0; i < n && added < 20; i++) {
        String ssid = WiFi.SSID(i);
        if (!ssid.length()) continue;
        if (added++) out += ",";
        out += "{\"ssid\":\"" + ConfigManager::jsonEsc(ssid) + "\",\"rssi\":" +
               String(WiFi.RSSI(i)) + ",\"open\":" +
               String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + "}";
      }
      out += "],\"scanning\":false}";
      WiFi.scanDelete();
      WiFi.scanNetworks(true, true);     // keep results fresh for next poll
      srv.send(200, "application/json", out);
    });

    srv.on("/api/config", HTTP_GET, [&]() {
      if (!_authed(srv)) return;
      String out = "{";
      out += "\"deviceId\":\"" + ConfigManager::jsonEsc(cfg.cfg.deviceId) + "\",";
      out += "\"wifiSsid\":\"" + ConfigManager::jsonEsc(cfg.cfg.wifiSsid) + "\"";
      if (_sessionMode == "dev") {
        out += ",\"wifiPass\":\""  + ConfigManager::jsonEsc(cfg.cfg.wifiPass) + "\"";
        out += ",\"broker\":\""    + ConfigManager::jsonEsc(cfg.cfg.broker)   + "\"";
        out += ",\"port\":"        + String(cfg.cfg.port);
        out += ",\"topic\":\""     + ConfigManager::jsonEsc(cfg.cfg.topic)    + "\"";
        out += ",\"apn\":\""       + ConfigManager::jsonEsc(cfg.cfg.apn)      + "\"";
        out += ",\"intervalMs\":"  + String(cfg.cfg.intervalMs);
        out += ",\"apPass\":\""    + ConfigManager::jsonEsc(cfg.cfg.apPass)   + "\"";
        out += ",\"batchSize\":"   + String(cfg.cfg.batchSize);
        out += ",\"maxStorage\":"  + String(cfg.cfg.maxStoragePct);
        out += ",\"autoDelete\":"  + String(cfg.cfg.autoDelete ? "true" : "false");
        out += ",\"devPin\":\""    + ConfigManager::jsonEsc(cfg.cfg.devPin)   + "\"";
        out += ",\"userPin\":\""   + ConfigManager::jsonEsc(cfg.cfg.userPin)  + "\"";
      }
      out += "}";
      srv.send(200, "application/json", out);
    });

    srv.on("/api/save", HTTP_POST, [&]() {
      if (!_authed(srv)) return;
      if (srv.hasArg("wifiSsid")) cfg.cfg.wifiSsid = srv.arg("wifiSsid");
      if (srv.hasArg("wifiPass")) cfg.cfg.wifiPass = srv.arg("wifiPass");
      bool needsReboot = srv.hasArg("wifiSsid");

      if (_sessionMode == "dev") {
        if (srv.hasArg("broker"))     cfg.cfg.broker     = srv.arg("broker");
        if (srv.hasArg("topic"))      cfg.cfg.topic      = srv.arg("topic");
        if (srv.hasArg("deviceId"))   cfg.cfg.deviceId   = srv.arg("deviceId");
        if (srv.hasArg("apn"))        cfg.cfg.apn        = srv.arg("apn");
        if (srv.hasArg("port"))       cfg.cfg.port       = constrain(srv.arg("port").toInt(), 1, 65535);
        if (srv.hasArg("intervalMs")) cfg.cfg.intervalMs = max(1000, (int)srv.arg("intervalMs").toInt());
        if (srv.hasArg("apPass") && srv.arg("apPass").length() >= 8) cfg.cfg.apPass = srv.arg("apPass");
        if (srv.hasArg("batchSize")) {
          cfg.cfg.batchSize = constrain(srv.arg("batchSize").toInt(), 1, 30);
          _sd->setRecordsPerFile(cfg.cfg.batchSize);
        }
        if (srv.hasArg("maxStorage")) {
          cfg.cfg.maxStoragePct = constrain(srv.arg("maxStorage").toInt(), 10, 99);
          _sd->setMaxStoragePct(cfg.cfg.maxStoragePct);
        }
        if (srv.hasArg("autoDelete")) {
          cfg.cfg.autoDelete = srv.arg("autoDelete") == "1";
          _sd->setAutoDelete(cfg.cfg.autoDelete);
        }
        if (srv.hasArg("devPin")  && srv.arg("devPin").length()  == 4) cfg.cfg.devPin  = srv.arg("devPin");
        if (srv.hasArg("userPin") && srv.arg("userPin").length() == 4) cfg.cfg.userPin = srv.arg("userPin");
      }
      cfg.save();
      srv.send(200, "application/json", "{\"ok\":true}");
      if (needsReboot) *_rebootAt = millis() + 2000;
    });

    srv.on("/api/log_files", HTTP_GET, [&]() {
      if (!_authed(srv)) return;
      srv.send(200, "application/json", "{\"files\":" + _sd->listFilesJSON() + "}");
    });

    srv.on("/api/log_page", HTTP_GET, [&]() {
      if (!_authed(srv)) return;
      String file   = srv.hasArg("file") ? srv.arg("file") : String("");
      int    offset = srv.hasArg("offset") ? srv.arg("offset").toInt() : 0;
      int    size   = constrain(srv.hasArg("size") ? srv.arg("size").toInt() : 20, 1, 100);
      bool   newest = srv.hasArg("newest") && srv.arg("newest") == "1";
      String search = srv.hasArg("search") ? srv.arg("search") : "";
      srv.send(200, "application/json", _sd->readPage(file, offset, size, newest, search));
    });

    // Renders a stored batch exactly as the broker will receive it.
    srv.on("/api/playback_view", HTTP_GET, [&]() {
      if (!_authed(srv)) return;
      String file = srv.arg("file");
      if (!file.length()) {
        SDStore::BatchInfo b = _sd->oldestBatch();
        if (b.valid) file = String(b.path);
      }
      if (!file.length()) {
        srv.send(200, "application/json", "{\"ok\":false,\"msg\":\"No pending batches\"}");
        return;
      }
      uint32_t offset = srv.hasArg("offset") ? (uint32_t)srv.arg("offset").toInt() : 0;
      int size = constrain(srv.hasArg("size") ? srv.arg("size").toInt() : 10, 1, 10);

      static GpsRecord vr[10];
      static char vbuf[5120];
      int got = _sd->readRecords(file.c_str(), (uint16_t)offset, (uint16_t)size, vr);
      if (got <= 0) { srv.send(200, "application/json",
        "{\"ok\":false,\"msg\":\"Batch unreadable at that offset\"}"); return; }
      _sync->buildBatchPayload(vr, got, 0, vbuf, sizeof(vbuf));

      String out = "{\"ok\":true,\"file\":\"" + file + "\",\"offset\":" + String(offset) +
                   ",\"count\":" + String(got) + ",\"total\":" + String(_sd->countLines(file)) +
                   ",\"topic\":\"" + _cfg->bulkTopic() + "\",\"payload\":" + String(vbuf) + "}";
      srv.send(200, "application/json", out);
    });

    // ---------------- DEV ONLY ----------------
    srv.on("/api/status", HTTP_GET, [&]() {
      if (!_devAuthed(srv)) return;
      static char buf[2048];
      snprintf(buf, sizeof(buf),
        "{\"lat\":%.7f,\"lng\":%.7f,\"alt\":%.2f,\"spd\":%.2f,\"hdg\":%.2f,"
        "\"sats\":%d,\"hdop\":%.2f,\"fix\":%s,\"batV\":%.2f,\"batPct\":%d,"
        "\"net\":\"%s\",\"sdOk\":%s,\"gpsQ\":%.2f,\"sync\":{\"status\":%d,"
        "\"pending\":%u,\"synced\":%u,\"batch\":%u,\"speed\":%.1f,\"estSec\":%u,"
        "\"lastOk\":%lu,\"lastFail\":%lu,\"lastError\":\"%s\",\"paused\":%s,"
        "\"files\":%u,\"tail\":%u,\"onsd\":%u,\"evicted\":%lu,\"dropped\":%lu},"
        "\"time\":{\"ts\":\"%s\",\"valid\":%s,\"src\":\"%s\",\"ref\":\"%s\","
        "\"ageSec\":%lu},"
        "\"e2e\":%s}",
        gps.location.isValid() ? gps.location.lat() : 0.0,
        gps.location.isValid() ? gps.location.lng() : 0.0,
        gps.altitude.isValid() ? gps.altitude.meters() : 0.0,
        gps.speed.isValid() ? gps.speed.kmph() : 0.0,
        gps.course.isValid() ? gps.course.deg() : 0.0,
        gps.satellites.isValid() ? gps.satellites.value() : 0,
        gps.hdop.isValid() ? gps.hdop.hdop() : 99.9,
        gps.location.isValid() ? "true" : "false",
        battery.volts(), battery.percent(),
        linkOnline ? (usingWiFi ? "WIFI" : "SIM") : "OFFLINE",
        _sd->isOk() ? "true" : "false", leds.quality(),
        (int)_sync->status, _sync->pendingRecords, _sync->syncedRecords,
        _sync->totalInBatch, _sync->uploadSpeed, _sync->estimatedSec,
        _sync->lastSyncOk, _sync->lastSyncFail, _sync->lastError.c_str(),
        _sync->paused ? "true" : "false",
        _sd->queueFiles(), _sd->tailRecords(), _sd->totalRecords(),
        (unsigned long)_sd->evicted(), (unsigned long)droppedRecords,
        timeSvc.formatStr().c_str(), timeSvc.synced() ? "true" : "false",
        timeSrcName(timeSvc.source()), timeSrcName(timeSvc.refSource()),
        (unsigned long)timeSvc.sinceSyncSec(),
        e2e.toJSON().c_str());
      srv.send(200, "application/json", buf);
    });

    srv.on("/api/storage", HTTP_GET, [&]() {
      if (!_devAuthed(srv)) return;
      SDStore::LogStats s = _sd->getStats();
      uint64_t total = _sd->totalBytes(), used = _sd->usedBytes();
      static char buf[1024];
      snprintf(buf, sizeof(buf),
        "{\"ok\":%s,\"totalMB\":%.1f,\"usedMB\":%.1f,\"freeMB\":%.1f,\"usedPct\":%d,"
        "\"logFiles\":%u,\"logLines\":%u,\"logMB\":%.2f,\"oldest\":\"%s\",\"newest\":\"%s\"}",
        _sd->isOk() ? "true" : "false", total / 1048576.0f, used / 1048576.0f,
        (total - used) / 1048576.0f, _sd->usedPercent(),
        s.fileCount, s.totalLines, s.totalLogBytes / 1048576.0f,
        s.oldestFile.c_str(), s.newestFile.c_str());
      srv.send(200, "application/json", buf);
    });

    srv.on("/api/netdiag",  HTTP_GET, [&]() { if (!_devAuthed(srv)) return; srv.send(200, "application/json", _net->toJSON()); });
    srv.on("/api/selftest", HTTP_GET, [&]() { if (!_devAuthed(srv)) return; srv.send(200, "application/json", selfTestJson); });

    srv.on("/api/sync",         HTTP_POST, [&]() { if (!_devAuthed(srv)) return; _sync->triggerNow(); srv.send(200, "application/json", "{\"ok\":true}"); });
    srv.on("/api/pause_sync",   HTTP_POST, [&]() { if (!_devAuthed(srv)) return; _sync->pause();      srv.send(200, "application/json", "{\"ok\":true}"); });
    srv.on("/api/resume_sync",  HTTP_POST, [&]() { if (!_devAuthed(srv)) return; _sync->paused = false; _sync->triggerNow(); srv.send(200, "application/json", "{\"ok\":true}"); });

    srv.on("/api/set_batch_size", HTTP_POST, [&]() {
      if (!_devAuthed(srv)) return;
      int sz = constrain(srv.arg("size").toInt(), 1, 30);
      cfg.cfg.batchSize = sz;
      _sd->setRecordsPerFile(sz);
      cfg.save();
      srv.send(200, "application/json", "{\"ok\":true,\"batchSize\":" + String(sz) + "}");
    });

    srv.on("/api/restart", HTTP_POST, [&]() {
      if (!_devAuthed(srv)) return;
      srv.send(200, "application/json", "{\"ok\":true}");
      *_rebootAt = millis() + 1500;
    });

    srv.on("/api/factory_reset", HTTP_POST, [&]() {
      if (!_devAuthed(srv)) return;
      _cfg->factoryReset();
      srv.send(200, "application/json", "{\"ok\":true}");
      *_rebootAt = millis() + 1500;
    });

    srv.on("/api/format_sd", HTTP_POST, [&]() {
      if (!_devAuthed(srv)) return;
      _sd->deleteAll();
      srv.send(200, "application/json", "{\"ok\":true}");
    });

    srv.on("/api/delete_all_logs", HTTP_DELETE, [&]() {
      if (!_devAuthed(srv)) return;
      _sd->deleteAll();
      srv.send(200, "application/json", "{\"ok\":true}");
    });

    srv.on("/api/delete_log", HTTP_DELETE, [&]() {
      if (!_devAuthed(srv)) return;
      String f = srv.arg("file");
      bool ok = f.length() ? _sd->deleteFile(f) : false;
      srv.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
    });

    srv.on("/api/download_csv",  HTTP_GET, [&]() { if (!_devAuthed(srv)) return; _streamCSV(srv);  });
    srv.on("/api/download_json", HTTP_GET, [&]() { if (!_devAuthed(srv)) return; _streamJSON(srv); });

    srv.onNotFound([&]() {
      srv.sendHeader("Location", "/lock", true);
      srv.send(302, "text/plain", "");
    });
  }

  bool _authed(WebServer& srv) {
    if (!_sessionToken.length() || srv.arg("token") != _sessionToken) {
      srv.send(401, "application/json", "{\"ok\":false,\"msg\":\"Unauthorized\"}");
      return false;
    }
    if ((millis() - _sessionCreatedAt) > SESSION_TIMEOUT_MS) {
      _sessionToken = "";
      srv.send(401, "application/json", "{\"ok\":false,\"msg\":\"Session expired\"}");
      return false;
    }
    return true;
  }

  bool _devAuthed(WebServer& srv) {
    if (!_authed(srv)) return false;
    if (_sessionMode != "dev") {
      srv.send(403, "application/json", "{\"ok\":false,\"msg\":\"Developer PIN required\"}");
      return false;
    }
    return true;
  }

  String _makeToken() {
    char b[17];
    sprintf(b, "%08x%08x", (uint32_t)esp_random(), (uint32_t)esp_random());
    return String(b);
  }

  String _datedFilename(const char* ext) {
    char b[32];
    sprintf(b, "gps_%lu.%s", millis() / 1000, ext);
    return String(b);
  }

  void _streamCSV(WebServer& srv) {
    srv.setContentLength(CONTENT_LENGTH_UNKNOWN);
    srv.sendHeader("Content-Disposition", "attachment; filename=" + _datedFilename("csv"));
    srv.send(200, "text/csv", "");
    srv.sendContent("seq,timestamp,latitude,longitude,altitude,speed,heading,satellites,hdop,battery,network\n");
    String files[64];
    int n = _sd->listAllLogPaths(files, 64);
    for (int i = 0; i < n; i++) {
      for (uint32_t off = 0; ; off += 100) {
        wdtFeed();
        String page = _sd->readPage(files[i], off, 100, false, "");
        if (page.length() <= 2) break;
        int start = 1;
        while (start < (int)page.length()) {
          int end = page.indexOf("},{", start);
          String rec = (end < 0) ? page.substring(start, page.length() - 1)
                                 : page.substring(start, end + 1);
          if (rec.length() < 8) break;
          GpsRecord r;
          if (SDStore::_parse(rec, r)) {
            char line[192];
            snprintf(line, sizeof(line), "%lu,%s,%.7f,%.7f,%.2f,%.2f,%.2f,%u,%.2f,%u,%s\n",
                     (unsigned long)r.seq, r.ts, r.lat, r.lng, r.alt, r.spd, r.hdg,
                     (unsigned)r.sats, r.hdop, (unsigned)r.batPct, netName(r.net));
            srv.sendContent(line);
          }
          if (end < 0) break;
          start = end + 2;
        }
        if (page.length() < 200) break;
      }
    }
  }

  void _streamJSON(WebServer& srv) {
    srv.setContentLength(CONTENT_LENGTH_UNKNOWN);
    srv.sendHeader("Content-Disposition", "attachment; filename=" + _datedFilename("json"));
    srv.send(200, "application/json", "");
    srv.sendContent("[");
    String files[64];
    int n = _sd->listAllLogPaths(files, 64);
    bool first = true;
    for (int i = 0; i < n; i++) {
      for (uint32_t off = 0; ; off += 100) {
        wdtFeed();
        String page = _sd->readPage(files[i], off, 100, false, "");
        if (page.length() <= 2) break;
        String body = page.substring(1, page.length() - 1);
        if (body.length()) { if (!first) srv.sendContent(","); srv.sendContent(body); first = false; }
        if (page.length() < 200) break;
      }
    }
    srv.sendContent("]");
  }
};

// ============================================================================
//  GLOBALS
// ============================================================================
HardwareSerial gpsSerial(2);
HardwareSerial modemSerial(1);

TinyGPSPlus    gps;
TinyGsm        modem(modemSerial);
TinyGsmClient  gsmClient(modem);
WiFiClient     wifiClient;

ConfigManager  configMgr;
BatteryMonitor battery;
LEDManager     leds;
TimeService    timeSvc;
SDStore        sdStore;
SyncEngine     syncEngine;
NetworkDiag    netDiag;
WebDashboard   webDash;
E2EVerifier    e2e;

PubSubClient mqttGSM(gsmClient);
PubSubClient mqttWiFi(wifiClient);
PubSubClient* mqtt = nullptr;

WebServer  server(80);
DNSServer  dnsServer;

bool          usingWiFi    = false;
volatile bool linkOnline   = false;
bool          modemPowered = false;

// Cross-task state. All are 32-bit aligned scalars: reads and writes are
// atomic on Xtensa, and every one of them has exactly one writer task.
volatile bool     liveBusy       = false;   // net task mid-publish
volatile bool     simDiagRequest = false;   // console SIM -> taskNet bring-up test
volatile bool     liveHoldActive = false;   // net task awaiting live echo
volatile uint32_t droppedRecords = 0;       // records that never reached SD
volatile unsigned long hbSensor = 0, hbStore = 0, hbNet = 0;

QueueHandle_t qRecords = nullptr;   // sensor → store   (the FIFO inlet)
QueueHandle_t qLive    = nullptr;   // store  → net     (live candidate)
QueueHandle_t qRecycle = nullptr;   // net    → store   (live that failed)

#define Q_RECORDS_DEPTH 128         // ≈10 min of buffer at a 5 s interval
#define Q_LIVE_DEPTH      4
#define Q_RECYCLE_DEPTH   8
#define LIVE_ACK_MS   12000UL       // live echo window before we store it
#define NET_TASK_STALL_MS 180000UL  // software supervisor for blocking modem AT

static char g_liveBuf[640];
static GpsRecord g_liveHold;
static unsigned long g_liveHoldDue = 0;

// WiFi/MQTT connectivity FSM
WifiPhase     wifiPhase   = WIFI_PH_IDLE;
unsigned long wifiAttemptAt = 0, wifiNextTry = 0, wifiBackoff = 5000UL;
unsigned long mqttNextTry = 0, simNextTry = 0;
bool          forceSetupMode = false, fullSelfTestDone = false;

#define WIFI_ATTEMPT_TIMEOUT_MS 20000UL
#define WIFI_BACKOFF_MAX_MS     60000UL
#define MQTT_RETRY_MS            5000UL
#define SIM_RETRY_MS            60000UL

// Bench test hook borrowed from the reference logger: simulate an outage
// without touching the radio, so the offline→sync→live path can be tested
// deterministically. NETSIM OFF / ON / AUTO.
NetSim netSim = NETSIM_AUTO;

unsigned long rebootAt = 0;
String selfTestJson = "{\"ran\":false}";
String mqttClientId = "";
bool   traceRecords = false;        // TRACE ON → per-record raw JSON trace

// Forward declarations
static bool  mqttEnsureBuffer(size_t need);
void  mqttCallback(char* topic, byte* payload, unsigned int length);
void  mqttSubscribeSelf();
void  useWiFiTransport();
void  useSimTransport();
bool  connectSIM();
bool  modemInternetAvailable();
bool  connectModemFallback();
void  initModemFunctionality();
String queryAccessTech();
void  simDiagnostic();
void  batteryDiagnostics();
void  batteryCalibrationBoot();
void  startAlwaysOnWeb();
void  runSelfTest();
void  handleSerialConsole();
void  processSerialCommand(String cmd);
String resolveLogPath(String arg);
size_t buildLivePayload(const GpsRecord& r, char* out, size_t cap);
void  taskSensor(void*);
void  taskStore(void*);
void  taskNet(void*);

// ============================================================================
//  BATTERY CALIBRATION AT BOOT
//
//  Decides, once per boot, which correction governs every later reading:
//    1. Load the stored calibration and validate magic/version/checksum/range.
//    2. If nothing valid is stored, adopt a legacy v6.2 "bcal" if one exists.
//    3. If BATTERY_CAL_REFERENCE_VOLTAGE names a voltage that differs from the
//       reference already stored, treat that as an operator request: measure,
//       solve, persist.
//    4. Otherwise use what is stored. Never recalibrate on its own.
//
//  Step 3 is the whole one-value workflow. Comparing against the STORED
//  reference - not a flag, not a boot counter - is what makes it idempotent:
//  the constant can stay in the source forever and the device recalibrates
//  exactly once, on the boot after it changed.
// ============================================================================
void batteryCalibrationBoot() {
  batteryCal = bcalLoad();

  if (!batteryCal.valid) bcalMigrateLegacy(batteryCal);

  const float wanted = BATTERY_CAL_REFERENCE_VOLTAGE;
  bool wantCal = (wanted >= 2.50f && wanted <= 4.60f) &&
                 (!batteryCal.valid || fabsf(wanted - batteryCal.ref) > 0.0005f);

  if (wantCal) {
    battery.setCalibration(1.0f, 0.0f);            // solve against raw metal
    float uncal = battery.measureUncalibrated(8);
    if (uncal > 0.5f) {
      BatteryCal c;
      c.ref    = wanted;
      c.gain   = wanted / uncal;
      c.offset = 0.0f;
      if (_bcalPlausible(c) && bcalSave(c)) {
        batteryCal = c;
        LOGW("BCAL", "CALIBRATED against %.3f V reference: measured %.3f V "
                     "uncalibrated, gain %.5f, stored in NVS '%s'",
             wanted, uncal, c.gain, BCAL_NS);
      } else {
        LOGW("BCAL", "Calibration REFUSED: %.3f V reference against %.3f V "
                     "measured implies gain %.4f, outside 0.5-2.0. Check the "
                     "divider wiring before trusting either number.",
             wanted, uncal, wanted / uncal);
      }
    } else {
      LOGW("BCAL", "Calibration skipped: ADC read %.3f V, no battery on the "
                   "divider?", uncal);
    }
  }

  battery.setCalibration(batteryCal.gain, batteryCal.offset);

  if (batteryCal.valid)
    LOGW("BCAL", "Calibration ACTIVE: gain %.5f, offset %+.4f V, reference "
                 "%.3f V %s", batteryCal.gain, batteryCal.offset, batteryCal.ref,
         wantCal ? "(just solved)" : "(loaded from NVS)");
  else
    LOGW("BCAL", "UNCALIBRATED: gain 1.0, offset 0. Set "
                 "BATTERY_CAL_REFERENCE_VOLTAGE to a multimeter reading and "
                 "upload once.");

#if BATTERY_DEBUG
  batteryDiagnostics();
#endif
}

// Full measurement chain, for traceability. Not printed in production unless
// BATTERY_DEBUG is 1; otherwise available on demand via the VCAL console.
void batteryDiagnostics() {
  float pinMv = battery.rawPinMillivolts();
  float mult  = (BATT_R1 + BATT_R2) / BATT_R2;
  sPrintf("\n========== BATTERY DIAGNOSTICS ==========\n");
  sPrintf("Raw ADC             : %u counts (12-bit)\n", battery.rawAdcCounts());
  sPrintf("ADC voltage         : %.3f V  (GPIO%d, 11dB, analogReadMilliVolts)\n",
          pinMv / 1000.0f, BATT_ADC_PIN);
  sPrintf("R1                  : %.0f ohm\n", BATT_R1);
  sPrintf("R2                  : %.0f ohm\n", BATT_R2);
  sPrintf("Divider multiplier  : %.4f\n", mult);
  sPrintf("Uncalibrated voltage: %.3f V\n", (pinMv / 1000.0f) * mult);
  sPrintf("Calibration factor  : %.6f\n", battery.getCalibration());
  sPrintf("Calibration offset  : %+.3f V\n", battery.getCalibrationOffset());
  sPrintf("Calibration ref     : %.3f V\n", batteryCal.ref);
  sPrintf("Filtered voltage    : %.3f V  (median+MAD, slew, EMA)\n", battery.volts());
  sPrintf("Final voltage       : %.3f V\n", battery.volts());
  sPrintf("Battery percentage  : %d%%\n", battery.percent());
  sPrintf("Rejected samples    : %lu\n", (unsigned long)battery.rejectedSamples());
  sPrintf("Calibration status  : %s\n", batteryCal.valid ? "VALID" : "NOT CALIBRATED");
  sPrintf("=========================================\n\n");
}

// Batch file stamp — needs timeSvc, so it is defined here rather than inline.
// Before first sync this yields "00000000-000000". That is deliberate and
// harmless: uniqueness comes from the Q%08lu sequence prefix in _makeName(),
// never from the stamp, so repeated stamps cannot collide. A filename of
// 00000000-000000 is itself a useful signal that the batch predates sync.
void SDStore::_stampNow(char* out, size_t cap) {
  char ts[24];
  timeSvc.format(ts, sizeof(ts));             // "YYYY-MM-DD HH:MM:SS" or TS_UNSYNCED
  snprintf(out, cap, "%c%c%c%c%c%c%c%c-%c%c%c%c%c%c",
           ts[0], ts[1], ts[2], ts[3], ts[5], ts[6], ts[8], ts[9],
           ts[11], ts[12], ts[14], ts[15], ts[17], ts[18]);
}

// ============================================================================
//  TimeService::netSyncTick — the network half of the time authority, and
//  as of v6.2 the PRIMARY half. Runs on taskNet, which is the task already
//  permitted to block on modem AT transactions. Defined here because it needs
//  wifiPhase, modemPowered and modemSerial, declared with the globals above.
//
//  v6.2: this function no longer stands down when GPS has a fix. It attempts
//  a sync on its own schedule whenever a transport is up, and whatever it
//  gets takes the clock. GPS is admitted only through the fallback branch in
//  ingestGps(), and only when this side has nothing fresh to offer.
//
//  TRANSPORT SPLIT — this is not a preference, it is a hard constraint:
//
//    Wi-Fi up   → SNTP via configTime(). lwIP owns the Wi-Fi netif, so the
//                 SNTP client has a route.
//    SIM only   → AT+CCLK?. SNTP CANNOT work over the cellular path in this
//                 firmware. TinyGSM implements sockets inside the modem via
//                 AT commands; the PDP context is never exposed to lwIP as a
//                 netif, so lwIP's SNTP client has nowhere to send a packet
//                 and would time out forever. The modem, however, already
//                 receives network time from the carrier (NITZ), which
//                 AT+CCLK? reads back directly — no round trip needed.
//
//  NON-BLOCKING: configTime() only arms the background SNTP client; the reply
//  is polled with time(nullptr) on later ticks and the function returns
//  immediately either way. Nothing here spins, and GPS polling, MQTT and SD
//  are all on other tasks regardless.
// ============================================================================
const char* TimeService::phaseName() const {
  return (_phase == NTP_WAITING) ? "awaiting SNTP reply" : "idle";
}

// Carrier time straight off the modem.
//
// SIM FIX: this used to write to modemSerial and drain the RX buffer by hand.
// TinyGSM owns that stream. A raw read loop swallows the +CIPRXGET / +IPCLOSE
// socket URCs TinyGSM is waiting for, and any reply arriving after the read
// window is left in the buffer where TinyGSM reads it as the answer to its
// NEXT command. One desync is permanent: init(), waitForNetwork() and
// isGprsConnected() all return garbage from then on. Going through sendAT /
// waitResponse keeps the stream state machine coherent and re-dispatches URCs
// to the right socket. Returns UTC epoch, or 0 on refusal.
//
// The manual AT+CTZU=1 that used to sit here is gone twice over: it blind-
// discarded every byte for 600 ms with no response matching at all, and
// TinyGSM's SIM7600 init() already sends +CTZU=1 itself.
int64_t TimeService::_queryModemClock() {
#if ENABLE_MODEM_FUNCTIONALITY
  if (!modemPowered) return 0;

  String resp;
  modem.sendAT("+CCLK?");
  if (modem.waitResponse(1200L, resp) != 1) return 0;

  int64_t e = _parseCCLK(resp.c_str());
  if (!e) {
    static unsigned long lastLog = 0;
    if (millis() - lastLog > 300000UL) {
      lastLog = millis();
      LOGW("TIME", "AT+CCLK? gave no usable time (carrier NITZ not received, "
                   "or reply carried no ±zz zone field) — holdover continues");
    }
  }
  return e;
#else
  return 0;
#endif
}

void TimeService::netSyncTick() {
  unsigned long now = millis();

  // v6.2. One line per authority TRANSITION, from the single task that owns
  // the network side of the clock. Nothing is printed while the source is
  // steady, which is almost always, so this cannot flood the console.
  _maybeAnnounce();

  // ---- poll an armed SNTP request ---------------------------------------
  if (_phase == NTP_WAITING) {
    int64_t sysNow = (int64_t)time(nullptr);
    // Two independent acceptance signals, both version-portable (no sntp_*
    // headers, whose names moved between ESP32 core 2.x and 3.x):
    //   a) the clock was nonsense at arm time and is sane now → first sync
    //   b) the clock jumped away from free-running projection → a reply landed
    int64_t projected = _clockAtArm + (esp_timer_get_time() - _usAtArm) / 1000000LL;
    bool wasUnsane = _clockAtArm < _floorEpoch();
    bool jumped    = (sysNow - projected > 2) || (projected - sysNow > 2);

    if (sysNow >= _floorEpoch() && (wasUnsane || jumped)) {
      ingestNetwork(sysNow, TSRC_NTP);
      _netStarved  = false;
      _phase = NTP_IDLE;
      _nextAttempt = now + TIME_REFRESH_MS;
      return;
    }

    if (now - _armedAt >= NTP_POLL_WINDOW_MS) {
      _phase = NTP_IDLE;
      if (sysNow >= _floorEpoch()) {
        // SNTP kept the system clock disciplined and within 2 s of our
        // projection, so no jump was observable. Re-anchor anyway: the SNTP
        // clock is a better reference than our free-running holdover.
        ingestNetwork(sysNow, TSRC_NTP);
        _netStarved  = false;
        _nextAttempt = now + TIME_REFRESH_MS;
      } else {
        LOGW("TIME", "NTP: no reply within %lus — retry in %lus",
             NTP_POLL_WINDOW_MS / 1000UL, TIME_RETRY_MS / 1000UL);
        _netStarved  = true;
        _nextAttempt = now + TIME_RETRY_MS;
      }
    }
    return;
  }

  // ---- transport observation, EVERY tick, independent of the schedule ----
  // A successful sync pushes _nextAttempt six hours out. Without this block
  // the schedule gate below would return early for those six hours, so a link
  // that died two minutes after syncing would keep being reported as a live
  // NTP reference and would not be retried until the refresh fell due. Notice
  // the outage when it happens, not when the timer expires.
  bool wifiUp = (wifiPhase == WIFI_PH_UP && WiFi.status() == WL_CONNECTED);
  bool cellUp = false;
#if ENABLE_MODEM_FUNCTIONALITY
  // SIM FIX. Two defects on one line.
  //   1. The modemPowered leg went true the instant powerOnModem() returned —
  //      before init() had run, and permanently after a FAILED connectSIM().
  //      That authorised AT+CCLK? during the registration window, where it ate
  //      the +CREG / +CGREG URCs waitForNetwork() depends on.
  //   2. modemInternetAvailable() is AT+CGATT? + AT+CGPADDR. connectivityTick()
  //      already issues it once per tick through transportUp(); a second
  //      caller here doubled the AT load on the UART to ~100 Hz and starved
  //      the socket path.
  // Reuse the link state connectivityTick() just established on this same
  // task. Zero extra AT traffic, and cellUp can no longer be true while the
  // PDP context is down.
  cellUp = (!usingWiFi && linkOnline);
#endif

  if (!wifiUp && !cellUp) {
    _netStarved = true;                 // nothing can confirm the clock now
    // Pull a distant refresh back to the retry cadence, so the link returning
    // is picked up within a minute even if it returns by a path that does not
    // call forceResync().
    if (_nextAttempt && (long)(_nextAttempt - now) > (long)TIME_RETRY_MS)
      _nextAttempt = now + TIME_RETRY_MS;
  }

  // ---- decide whether to start an attempt --------------------------------
  if (_nextAttempt && (long)(now - _nextAttempt) < 0) return;

  // ---- T6: THE v6.2 SKIP REMOVAL -----------------------------------------
  // v6.1 returned here whenever source() == TSRC_GPS, on the reasoning that
  // there was no point fetching a value ingestNetwork() would only refuse.
  // That reasoning was sound and the conclusion was correct FOR THAT
  // RANKING. Under network-first authority it is exactly backwards: a unit
  // with a fix would never even ask the network, so the primary source could
  // not take the clock while the fallback was working. The gate is gone.
  //
  // This is the one place v6.2 can add traffic, so it is worth being precise
  // about how much. Over Wi-Fi: TIME_REFRESH_MS is 6 h, so four SNTP
  // exchanges a day, a few hundred bytes each. Over cellular: none at all —
  // AT+CCLK? is a local UART query against the NITZ value the modem already
  // holds, and does not open a socket or touch the PDP context.

  if (wifiUp) {
    _clockAtArm = (int64_t)time(nullptr);
    _usAtArm    = esp_timer_get_time();
    // (0, 0) = keep libc in UTC. TZ_OFFSET_SECONDS is applied only in
    // format(); shifting here as well would double-apply it.
    configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
    _phase   = NTP_WAITING;
    _armedAt = now;
    DBG("TIME", "NTP armed via WiFi (%s) — polling up to %lus",
        NTP_SERVER_1, NTP_POLL_WINDOW_MS / 1000UL);
    return;
  }

#if ENABLE_MODEM_FUNCTIONALITY
  if (cellUp) {
    int64_t e = _queryModemClock();
    if (e > 0) {
      ingestNetwork(e, TSRC_CELL);
      _netStarved  = false;
      _nextAttempt = now + TIME_REFRESH_MS;
    } else {
      _netStarved  = true;
      _nextAttempt = now + TIME_RETRY_MS;
    }
    return;
  }
#endif

  // No transport at all. Holdover continues; try again shortly. This counts
  // as starvation: nothing is confirming the clock, so source() must say so.
  _netStarved  = true;
  _nextAttempt = now + TIME_RETRY_MS;
}

// GROW-ONLY MQTT buffer. PubSubClient shares ONE buffer for TX and RX, so
// shrinking it while an echo is in flight silently drops the echo and turns
// every ACK into a timeout. Returns false if the allocation failed, which
// the caller must handle rather than publish into a short buffer.
static bool mqttEnsureBuffer(size_t need) {
  if (!mqtt) return false;
  if (need < 512) need = 512;
  if (mqtt->getBufferSize() >= need) return true;
  if (!mqtt->setBufferSize(need)) {
    LOGW("MQTT", "Buffer grow to %u bytes FAILED (heap %u) — batch will be split",
         (unsigned)need, (unsigned)ESP.getFreeHeap());
    return false;
  }
  DBG("MQTT", "Shared TX/RX buffer grown to %u bytes", (unsigned)need);
  return true;
}

// SyncEngine::_upload lives here because it needs leds + mqttEnsureBuffer.
void SyncEngine::_upload() {
  if (!_mqtt || !_mqtt->connected()) { status = SYNC_PAUSED; lastError = "MQTT down"; return; }
  if (_nInBatch <= 0 || _payloadLen == 0) { status = SYNC_PREPARING; return; }
  _retryToPrepare = false;

  if (!mqttEnsureBuffer(_payloadLen + 192)) {
    // Not enough heap for this batch. Halve it and try again next tick —
    // never publish a truncated payload, never advance past unsent records.
    if (_nInBatch > 1) {
      _nInBatch   = _nInBatch / 2;
      _payloadLen = buildBatchPayload(_recs, _nInBatch, _batch.seq, g_payload, PAYLOAD_CAP);
      totalInBatch = _nInBatch;
      LOGW("SYNC", "Reduced batch to %d record(s) to fit available heap", _nInBatch);
      return;
    }
    _fail("Insufficient heap for MQTT buffer");
    return;
  }

  String topic = _cfg->bulkTopic();
  _e2e->expectBulk(g_payload, _payloadLen, _batch.seq);

  bool ok = _mqtt->publish(topic.c_str(), g_payload, false);
  if (ok) {
    leds.notifyTx();
    LOGW("SYNC", "BULK → %s | %s[%u..%u] | %d record(s), %u bytes | awaiting %s",
         topic.c_str(), _batch.path, _offset, _offset + _nInBatch - 1,
         _nInBatch, (unsigned)_payloadLen,
         _cfg->cfg.ackMode == 1 ? "backend ACK" : "broker echo");
    _ackDue = millis() + ACK_TIMEOUT_MS;
    status  = SYNC_WAITING_ACK;
  } else {
    _e2e->pbPending = false;    // the packet never left the device
    _fail("Publish failed (socket)");
  }
}

// ============================================================================
//  MQTT plumbing
// ============================================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  e2e.onMessage(topic, payload, length);
}

void mqttSubscribeSelf() {
  if (!mqtt || !mqtt->connected()) return;
  bool a = mqtt->subscribe(configMgr.cfg.topic.c_str());
  bool b = mqtt->subscribe(configMgr.bulkTopic().c_str());
  bool c = mqtt->subscribe(configMgr.diagTopic().c_str());
  bool d = (configMgr.cfg.ackMode == 1) ? mqtt->subscribe(configMgr.ackTopic().c_str()) : true;
  DBG("E2E", "Self-subscribe live:%s bulk:%s diag:%s ack:%s",
      a ? "OK" : "FAIL", b ? "OK" : "FAIL", c ? "OK" : "FAIL", d ? "OK" : "FAIL");
}

void useWiFiTransport() {
  mqtt = &mqttWiFi; usingWiFi = true;
  mqtt->setServer(configMgr.cfg.broker.c_str(), configMgr.cfg.port);
  mqtt->setCallback(mqttCallback);
  mqtt->setKeepAlive(60);
  mqttEnsureBuffer(1024);
  syncEngine.attachMqtt(mqtt);
  DBG("MQTT", "Transport → WiFi (%s:%d)", configMgr.cfg.broker.c_str(), configMgr.cfg.port);
}

void useSimTransport() {
  mqtt = &mqttGSM; usingWiFi = false;
  mqtt->setServer(configMgr.cfg.broker.c_str(), configMgr.cfg.port);
  mqtt->setCallback(mqttCallback);
  mqtt->setKeepAlive(60);
  mqttEnsureBuffer(1024);
  syncEngine.attachMqtt(mqtt);
  DBG("MQTT", "Transport → SIM (%s:%d)", configMgr.cfg.broker.c_str(), configMgr.cfg.port);
}

// ============================================================================
//  LIVE PAYLOAD — schema locked to what the backend already ingests. Only
//  two things differ from a stored record: internet_available and status.
// ============================================================================
size_t buildLivePayload(const GpsRecord& r, char* out, size_t cap) {
  const char* uid = configMgr.cfg.deviceId.c_str();
  int n = snprintf(out, cap,
    "{\"username\":\"%s\",\"modelList\":[{\"username\":\"%s\",\"appCode\":\"DEMO\","
    "\"latitude\":%.7f,\"longitude\":%.7f,\"platform\":\"IoMT\",\"visitDate\":\"%s\","
    "\"visitTime\":\"%s\",\"networkType\":\"%s\",\"broadcastEnabled\":true,"
    "\"locationAccuracy\":%.2f,\"altitude_msl\":%.2f,\"speed_kmph\":%.2f,"
    "\"heading_deg\":%.2f,\"satellite_count\":%d,\"internet_available\":true,"
    "\"batteryPower\":%d,\"status\":\"live\","
    "\"time_valid\":%s,\"time_src\":\"%s\"}]}",
    uid, uid, r.lat, r.lng, r.ts, r.ts, netName(r.net),
    r.hdop, r.alt, r.spd, r.hdg, (int)r.sats, (int)r.batPct,
    r.tsrc == TSRC_NONE ? "false" : "true", timeSrcName(r.tsrc));
  return (n < 0) ? 0 : (size_t)n;
}

// ============================================================================
//  TELEMETRY TABLE — fixed-width, left-to-right, paste-into-Excel clean.
//  One row per generated record; header re-printed every 20 rows so a long
//  capture stays readable. Everything else that used to spam the console
//  (the per-record [DATA] trace) is behind the TRACE command.
// ============================================================================
static const char* TELEM_HEADER =
  "TIME      LATITUDE      LONGITUDE      SATS  HDOP   SPEED   BAT   NETWORK   GPS   SD   SYNC       QUEUE";

static const char* syncLabel(SyncStatus s) {
  switch (s) {
    case SYNC_IDLE:        return "IDLE";
    case SYNC_PREPARING:   return "PREP";
    case SYNC_UPLOADING:   return "UPLOAD";
    case SYNC_WAITING_ACK: return "WAIT-ACK";
    case SYNC_RETRYING:    return "RETRY";
    case SYNC_COMPLETED:   return "DONE";
    case SYNC_FAILED:      return "FAILED";
    case SYNC_PAUSED:      return "PAUSED";
  }
  return "?";
}

static void printTelemetryRow(const GpsRecord& r) {
  static uint16_t rows = 0;
  if ((rows++ % 20) == 0) sPrintf("%s\n", TELEM_HEADER);

  char bat[8];  snprintf(bat, sizeof(bat), "%d%%", (int)r.batPct);
  char qcol[16]; snprintf(qcol, sizeof(qcol), "%u/%u",
                          (unsigned)sdStore.queueFiles(), (unsigned)sdStore.tailRecords());

  const char* gpsCol = leds.gpsStale() ? "STALE"
                     : (!r.fix)        ? "NOFIX"
                     : (leds.quality() > 0.75f) ? "OK"
                     : (leds.quality() > 0.45f) ? "FAIR" : "WEAK";

  sPrintf("%-8s  %-13.7f %-14.7f %-5u %-6.2f %-7.2f %-5s %-9s %-5s %-4s %-10s %s\n",
          r.ts + 11,                       // HH:MM:SS
          r.lat, r.lng, (unsigned)r.sats, r.hdop, r.spd, bat,
          linkOnline ? (usingWiFi ? "WIFI" : "SIM") : "OFFLINE",
          gpsCol, sdStore.isOk() ? "OK" : "FAIL",
          syncLabel(syncEngine.status), qcol);

  if (traceRecords) {
    char raw[REC_LINE_MAX];
    SDStore::_serialise(r, raw, sizeof(raw));
    sPrintf("[TRACE] %s\n", raw);
  }
}

// ============================================================================
//  TASK 1 — SENSOR (core 1, prio 3)
//  Owns: GPS UART, TinyGPS++, the battery ADC, the LEDs, record creation.
//  Never touches SD, never touches the network. Nothing it does can be
//  delayed by an outage, a broker timeout or a slow SD card, which is the
//  whole point: acquisition must not depend on connectivity.
// ============================================================================
void taskSensor(void*) {
  esp_task_wdt_add(NULL);
  unsigned long nextRecord = millis() + 1500;   // let the first NMEA arrive
  unsigned long lastLed = 0, lastTimeIngest = 0;

  for (;;) {
    hbSensor = millis();
    esp_task_wdt_reset();

    // 1) GPS UART — drained every pass. 2048-byte RX buffer plus a 5 ms
    // service interval means NMEA cannot overflow at 9600 baud.
    while (gpsSerial.available()) gps.encode(gpsSerial.read());

    // 1b) GPS → time authority. Four times a second is ample against 1 Hz
    // NMEA and costs nothing. ingestGps() refuses anything stale or insane,
    // so a lost fix degrades to holdover instead of freezing the clock.
    if (millis() - lastTimeIngest >= 250) {
      lastTimeIngest = millis();
      timeSvc.ingestGps(gps);
    }

    // 2) Battery — the ONLY ADC sampling site in the firmware.
    battery.sample();

    // 3) LEDs — 50 Hz state machine, no blocking anywhere.
    if (millis() - lastLed >= 20) {
      lastLed = millis();
      leds.setBatteryPct(battery.percent());
      leds.setGps(gps.location.isValid() && gps.location.age() < 10000,
                  gps.satellites.isValid() ? gps.satellites.value() : 0,
                  gps.hdop.isValid() ? (float)gps.hdop.hdop() : 99.9f);
      leds.setNetMode(syncEngine.busy() ? NETLED_BULK
                    : linkOnline        ? NETLED_LIVE
                                        : NETLED_OFFLINE);
      leds.update();
    }

    // 4) Record generation on the configured interval.
    if ((long)(millis() - nextRecord) >= 0) {
      nextRecord += (unsigned long)configMgr.cfg.intervalMs;
      if ((long)(millis() - nextRecord) >= 0) nextRecord = millis() + configMgr.cfg.intervalMs;

      GpsRecord r;
      memset(&r, 0, sizeof(r));
      r.seq  = sdStore.nextSeq();
      // One authority, one formatter. Capture the source alongside the stamp
      // so a record stays self-describing after hours on the card.
      timeSvc.format(r.ts, sizeof(r.ts));
      r.tsrc = (uint8_t)timeSvc.source();
      r.fix  = gps.location.isValid() ? 1 : 0;
      r.lat  = gps.location.isValid()   ? gps.location.lat()    : 0.0;
      r.lng  = gps.location.isValid()   ? gps.location.lng()    : 0.0;
      r.alt  = gps.altitude.isValid()   ? gps.altitude.meters() : 0.0f;
      r.spd  = gps.speed.isValid()      ? gps.speed.kmph()      : 0.0f;
      r.hdg  = gps.course.isValid()     ? gps.course.deg()      : 0.0f;
      r.sats = gps.satellites.isValid() ? gps.satellites.value(): 0;
      r.hdop = gps.hdop.isValid()       ? (float)gps.hdop.hdop(): 99.9f;
      r.batPct = (uint8_t)battery.percent();
      // Honest network label: a record captured with no link says NONE.
      r.net  = linkOnline ? (usingWiFi ? NET_WIFI : NET_SIM) : NET_NONE;
      sdStore.persistSeqIfDue();

      if (xQueueSend(qRecords, &r, 0) != pdTRUE) {
        // Explicit overflow handling — never a silent discard. This can
        // only happen if the store task has been blocked for minutes.
        droppedRecords++;
        LOGW("QUEUE", "OVERFLOW — record seq %lu could not be queued "
                      "(store task stalled). Total lost this boot: %lu",
             (unsigned long)r.seq, (unsigned long)droppedRecords);
      }
      printTelemetryRow(r);
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ============================================================================
//  TASK 2 — STORE (core 1, prio 2)
//  The single logical owner of the SD FIFO and the only place that decides
//  whether a record goes live or to disk. Recycled records are drained
//  first so a failed live publish lands on the card ahead of anything
//  generated after it — FIFO order is preserved end to end.
// ============================================================================
static void routeRecord(const GpsRecord& r) {
  bool canLive = linkOnline
              && !sdStore.hasBacklog()      // stored data always outranks live
              && !liveBusy
              && !liveHoldActive            // previous live still unverified
              && uxQueueMessagesWaiting(qLive) == 0;

  if (canLive && xQueueSend(qLive, &r, 0) == pdTRUE) return;

  // This record is going to disk, so the link is down or a backlog exists.
  // Anything still queued for live publishing is OLDER than this record and
  // must reach the card first, or FIFO ordering breaks at the exact moment
  // connectivity is lost. The store task is the only writer, so draining
  // here (rather than from the network task) keeps the ordering exact.
  GpsRecord q;
  while (xQueueReceive(qLive, &q, 0) == pdTRUE) {
    if (!sdStore.append(q)) {
      droppedRecords++;
      LOGW("STORE", "CRITICAL: stranded live record seq %lu NOT persisted",
           (unsigned long)q.seq);
    }
  }

  if (!sdStore.append(r)) {
    droppedRecords++;
    LOGW("STORE", "CRITICAL: record seq %lu NOT persisted (SD unavailable). "
                  "Total lost this boot: %lu",
         (unsigned long)r.seq, (unsigned long)droppedRecords);
  }
}

void taskStore(void*) {
  esp_task_wdt_add(NULL);
  unsigned long lastHealth = 0, lastResync = millis(), lastEvict = 0;

  for (;;) {
    hbStore = millis();
    esp_task_wdt_reset();

    GpsRecord r;
    while (xQueueReceive(qRecycle, &r, 0) == pdTRUE) {
      if (!sdStore.append(r)) {
        droppedRecords++;
        LOGW("STORE", "CRITICAL: recycled record seq %lu NOT persisted",
             (unsigned long)r.seq);
      }
    }

    if (xQueueReceive(qRecords, &r, pdMS_TO_TICKS(50)) == pdTRUE) routeRecord(r);

    unsigned long now = millis();

    if (now - lastHealth > 30000UL) {
      lastHealth = now;
      bool was = sdStore.isOk();
      bool ok  = sdStore.healthCheck();
      if (!was && ok) LOGW("SD", "Card recovered — offline logging re-enabled");
      if (!ok)        LOGW("SD", "HEALTH: card NOT usable — records are being lost");
    }

    // Ground-truth reconciliation: RAM state is never trusted indefinitely.
    if (now - lastResync > 300000UL) { lastResync = now; sdStore.resyncIndex(); }

    if (now - lastEvict > 60000UL) { lastEvict = now; sdStore.evictIfNeeded(); }
  }
}

// ============================================================================
//  TASK 3 — NETWORK (core 0, prio 2)
//  Owns WiFi, the modem, MQTT, live publishing and the sync engine. It is
//  allowed to block (the modem AT stack does, unavoidably); nothing on the
//  data side depends on it. Supervised by a software liveness timer rather
//  than the hardware task WDT precisely because of those bounded blocks.
// ============================================================================
static bool transportUp() {
  if (netSim == NETSIM_FORCE_OFF) return false;
  return (wifiPhase == WIFI_PH_UP && WiFi.status() == WL_CONNECTED)
         || modemInternetAvailable();
}

static void connectivityTick() {
  if (forceSetupMode) { linkOnline = false; return; }

  if (netSim == NETSIM_FORCE_OFF) {
    if (mqtt && mqtt->connected()) mqtt->disconnect();
    linkOnline = false;
    netDiag.mqttStatus = "Simulated offline";
    return;
  }

  // Edge: STA link dropped while it was up.
  if (wifiPhase == WIFI_PH_UP && WiFi.status() != WL_CONNECTED) {
    LOGW("WIFI", "STA link LOST — background retry starts now "
                 "(hotspot, logging, dashboard unaffected)");
    wifiPhase = WIFI_PH_IDLE;
    wifiBackoff = 5000UL;
    wifiNextTry = millis();
    netDiag.mqttStatus = "Disconnected";
  }

  if (wifiPhase == WIFI_PH_IDLE && configMgr.cfg.wifiSsid.length() &&
      millis() >= wifiNextTry) {
    DBG("WIFI", "Connecting to '%s' (background, %lus timeout)",
        configMgr.cfg.wifiSsid.c_str(), WIFI_ATTEMPT_TIMEOUT_MS / 1000UL);
    WiFi.disconnect();                     // STA only — the AP is untouched
    WiFi.begin(configMgr.cfg.wifiSsid.c_str(), configMgr.cfg.wifiPass.c_str());
    wifiPhase = WIFI_PH_TRYING;
    wifiAttemptAt = millis();
  }

  if (wifiPhase == WIFI_PH_TRYING) {
    if (WiFi.status() == WL_CONNECTED) {
      wifiPhase = WIFI_PH_UP;
      wifiBackoff = 5000UL;
      netDiag.updateWiFiInfo();
      useWiFiTransport();
      mqttNextTry = millis();
      timeSvc.forceResync();     // network just arrived — sync the clock now
      LOGW("WIFI", "Connected | STA http://%s | Hotspot http://192.168.4.1",
           WiFi.localIP().toString().c_str());
    } else if (millis() - wifiAttemptAt > WIFI_ATTEMPT_TIMEOUT_MS) {
      WiFi.disconnect();
      wifiPhase = WIFI_PH_IDLE;
      wifiNextTry = millis() + wifiBackoff;
      DBG("WIFI", "Attempt timed out — next retry in %lus", wifiBackoff / 1000UL);
      wifiBackoff = (wifiBackoff * 2UL > WIFI_BACKOFF_MAX_MS) ? WIFI_BACKOFF_MAX_MS
                                                              : wifiBackoff * 2UL;
#if ENABLE_MODEM_FUNCTIONALITY
      if (millis() >= simNextTry) {
        simNextTry = millis() + SIM_RETRY_MS;
        connectModemFallback();            // bounded blocking, this task only
      }
#endif
    }
  }

#if ENABLE_MODEM_FUNCTIONALITY
  // SIM-only deployment. The fallback above lives inside the WIFI_PH_TRYING
  // timeout branch, and with no SSID configured the phase never leaves IDLE —
  // so the modem was never brought up at all, and the device sat OFFLINE
  // forever with a perfectly good SIM in it. Cover that case explicitly.
  if (!configMgr.cfg.wifiSsid.length() && wifiPhase == WIFI_PH_IDLE &&
      millis() >= simNextTry) {
    simNextTry = millis() + SIM_RETRY_MS;
    connectModemFallback();
  }
#endif

  bool up = transportUp();
  if (mqtt && up && !mqtt->connected() && millis() >= mqttNextTry) {
    DBG("MQTT", "Connecting %s:%d as '%s' via %s",
        configMgr.cfg.broker.c_str(), configMgr.cfg.port,
        mqttClientId.c_str(), usingWiFi ? "WiFi" : "SIM");
    if (mqtt->connect(mqttClientId.c_str())) {
      netDiag.mqttStatus = "Connected";
      mqttSubscribeSelf();
      syncEngine.resume(mqtt);
      LOGW("MQTT", "Connected — backlog sync resumed");
      if (!fullSelfTestDone) { fullSelfTestDone = true; runSelfTest(); }
    } else {
      netDiag.mqttStatus = "Disconnected";
      mqttNextTry = millis() + MQTT_RETRY_MS;
      DBG("MQTT", "Connect failed (state %d) — retry in %lus",
          mqtt->state(), MQTT_RETRY_MS / 1000UL);
    }
  }

  linkOnline = up && mqtt && mqtt->connected();
}

// Live publish + verification hold. A live record is not considered
// delivered until its echo comes back; if it does not, the record is
// re-queued to the SD FIFO instead of vanishing (root cause R5).
static void livePublishTick() {
  if (liveHoldActive) {
    if (!e2e.livePending) {                       // echo matched
      liveHoldActive = false;
    } else if (millis() > g_liveHoldDue) {
      e2e.livePending = false;
      e2e.liveLost++;
      liveHoldActive = false;
      LOGW("LIVE", "Record seq %lu unverified after %lus — re-queued to SD FIFO",
           (unsigned long)g_liveHold.seq, LIVE_ACK_MS / 1000UL);
      if (xQueueSend(qRecycle, &g_liveHold, 0) != pdTRUE) {
        droppedRecords++;
        LOGW("LIVE", "CRITICAL: recycle queue full, record seq %lu lost",
             (unsigned long)g_liveHold.seq);
      }
    }
    return;                                       // one live record at a time
  }

  if (!linkOnline || !mqtt) return;
  GpsRecord r;
  if (xQueueReceive(qLive, &r, 0) != pdTRUE) return;

  liveBusy = true;
  size_t len = buildLivePayload(r, g_liveBuf, sizeof(g_liveBuf));
  bool ok = false;
  if (len && mqttEnsureBuffer(len + 192)) {
    e2e.expectLive(g_liveBuf, len);
    ok = mqtt->publish(configMgr.cfg.topic.c_str(), g_liveBuf, false);
  }

  if (ok) {
    leds.notifyTx();
    g_liveHold = r;
    g_liveHoldDue = millis() + LIVE_ACK_MS;
    liveHoldActive = true;
  } else {
    e2e.livePending = false;
    LOGW("LIVE", "Publish failed for seq %lu — storing on SD", (unsigned long)r.seq);
    if (xQueueSend(qRecycle, &r, 0) != pdTRUE) {
      droppedRecords++;
      LOGW("LIVE", "CRITICAL: recycle queue full, record seq %lu lost",
           (unsigned long)r.seq);
    }
  }
  liveBusy = false;
}

void taskNet(void*) {
  unsigned long lastSimDiag = 0;
  for (;;) {
    hbNet = millis();

    connectivityTick();

#if ENABLE_MODEM_FUNCTIONALITY
    // Console "SIM" lands here. taskNet is the only task allowed to talk to
    // the modem, so the console sets a flag and the work happens here.
    if (simDiagRequest) { simDiagRequest = false; simDiagnostic(); }
#endif

    if (mqtt) mqtt->loop();      // drives inbound echoes → E2E verification
    livePublishTick();
    syncEngine.tick();

#if ENABLE_MODEM_FUNCTIONALITY
    if (modemPowered && millis() - lastSimDiag > 30000UL) {
      lastSimDiag = millis();
      netDiag.updateSIMInfo(queryAccessTech());
    }
#endif

    // LAST in the loop. mqtt->loop() and the AT+CPSI? diagnostic have both
    // finished with the modem by this point, so the AT+CCLK? transaction
    // cannot land in the middle of anything TinyGSM is still waiting on.
    timeSvc.netSyncTick();       // NTP over WiFi / AT+CCLK? over cellular

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ============================================================================
//  SETUP — nothing here blocks on the network. The device is fully
//  operational (acquiring, logging, hotspot, dashboard, console) before any
//  internet connection exists.
// ============================================================================
void setup() {
  delay(60000);
  Serial.begin(115200);
  delay(400);
  serialMutex = xSemaphoreCreateMutex();

  sPrintf("\n==========================================\n");
  sPrintf("BOTS BANGLA GPS TRACKER v%s (PRODUCTION)\n", FW_VERSION);
  sPrintf("==========================================\n");
  sPrintf("Serial console ready — type HELP\n");

  wdtInit();

  // ---- boot lifecycle steps 2-4 -----------------------------------------
  // Detect an incompatible/new firmware image, clear obsolete persistent
  // state, then recreate storage from this firmware's schema and defaults.
  // Placed here deliberately: after the watchdog is armed so a stall during
  // NVS work still reboots, and before configMgr.load(), sdStore.begin() and
  // every peripheral init — so nothing can have consumed stale state by the
  // time the decision is made.
  ProvisionResult prov = provisionPersistentState();
  logProvisioning(prov);

  configMgr.load();
  if (prov.wiped) {
    // The namespace is empty, so load() just returned this firmware's own
    // default for every field and applied its clamps. Persist that image now
    // rather than leaving NVS blank until somebody happens to hit Save, so
    // the stored configuration and the running configuration are identical
    // from the first second of the first boot.
    configMgr.save();
    LOGW("PROV", "Configuration recreated from firmware defaults and persisted");
  }
  battery.begin();                 // ADC config first - calibration needs to measure
  batteryCalibrationBoot();        // load / migrate / solve-once, then apply
  leds.begin();

  char cid[12];
  snprintf(cid, sizeof(cid), "-%08x", (uint32_t)esp_random());
  mqttClientId = configMgr.cfg.deviceId + String(cid);
  e2e.setAckMode(configMgr.cfg.ackMode, configMgr.ackTopic());
  DBG("BOOT", "MQTT client ID: %s | ACK mode: %s", mqttClientId.c_str(),
      configMgr.cfg.ackMode == 1 ? "backend ACK topic" : "broker echo");

  gpsSerial.setRxBufferSize(2048);
  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  initModemFunctionality();

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdStore.setRecordsPerFile(configMgr.cfg.batchSize);
  sdStore.setMaxStoragePct(configMgr.cfg.maxStoragePct);
  sdStore.setAutoDelete(configMgr.cfg.autoDelete);
  if (!sdStore.begin(SD_CS))
    LOGW("SD", "Mount FAILED at boot — health check retries every 30 s");

  syncEngine.begin(&sdStore, &configMgr, &e2e);
  netDiag.begin(ENABLE_MODEM_FUNCTIONALITY ? &modem : nullptr);
  webDash.attach(configMgr, sdStore, syncEngine, netDiag, rebootAt);

  pinMode(FORCE_SETUP_PIN, INPUT_PULLUP);
  delay(50);
  forceSetupMode = (digitalRead(FORCE_SETUP_PIN) == LOW);
  if (forceSetupMode)
    LOGW("BOOT", "FORCED SETUP MODE — STA/MQTT disabled until config is saved; "
                 "hotspot, dashboard and logging remain fully active");

  startAlwaysOnWeb();
  runSelfTest();

  qRecords = xQueueCreate(Q_RECORDS_DEPTH, sizeof(GpsRecord));
  qLive    = xQueueCreate(Q_LIVE_DEPTH,    sizeof(GpsRecord));
  qRecycle = xQueueCreate(Q_RECYCLE_DEPTH, sizeof(GpsRecord));
  if (!qRecords || !qLive || !qRecycle) {
    LOGW("BOOT", "FATAL: queue allocation failed — rebooting");
    delay(200);
    ESP.restart();
  }

  hbSensor = hbStore = hbNet = millis();
  wifiNextTry = millis();

  xTaskCreatePinnedToCore(taskSensor, "sensor", 6144, nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(taskStore,  "store",  8192, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(taskNet,    "net",   12288, nullptr, 2, nullptr, 0);

  sPrintf("[BOOT] SYSTEM READY — acquisition and preservation run in every state\n");
  sPrintf("%s\n", TELEM_HEADER);
}

// ============================================================================
//  LOOP (core 1, prio 1) — dashboard, captive DNS, serial console, task
//  supervision. Deliberately the LOWEST-value task: if it were to stop,
//  data acquisition and preservation would continue unaffected.
// ============================================================================
void loop() {
  wdtFeed();

  dnsServer.processNextRequest();
  server.handleClient();
  handleSerialConsole();

  // Software liveness supervision. The sensor and store tasks are on the
  // hardware watchdog; the network task is not, because a modem AT
  // transaction can legitimately block it for tens of seconds.
  unsigned long now = millis();
  if (now - hbNet > NET_TASK_STALL_MS) {
    LOGW("SUPERVISOR", "Network task stalled for %lus — rebooting",
         (now - hbNet) / 1000UL);
    delay(100);
    ESP.restart();
  }

  if (rebootAt && millis() > rebootAt) {
    sPrintf("[BOOT] Rebooting...\n");
    sdStore.persistSeqIfDue(true);
    delay(120);
    ESP.restart();
  }

  delay(2);
}

// ============================================================================
//  BOOT SELF-TEST — hardware legs run at boot; the E2E leg needs a live
//  MQTT session so it re-runs automatically on the first MQTT connect. The
//  probe goes to <topic>/diag so the live stream stays schema-clean.
// ============================================================================
void runSelfTest() {
  sPrintf("---------- SELF-TEST ----------\n");

  uint32_t chars = gps.charsProcessed();
  bool gpsUart = (chars > 10);
  sPrintf("GPS UART   : %s (%u NMEA chars)\n",
          gpsUart ? "OK" : "NO DATA — check RX16/TX17 and 9600 baud", chars);
  bool gpsFix = gps.location.isValid();
  sPrintf("GPS fix    : %s (%d sats)\n", gpsFix ? "YES" : "not yet",
          gps.satellites.isValid() ? gps.satellites.value() : 0);

  if (timeSvc.synced())
    sPrintf("Clock      : %s (BST) via %s, %lus since sync\n",
            timeSvc.formatStr().c_str(), timeSrcName(timeSvc.refSource()),
            (unsigned long)timeSvc.sinceSyncSec());
  else
    sPrintf("Clock      : UNSYNCED — awaiting network sync (or a GPS fix as "
            "fallback); records are marked time_valid:false until then\n");

  bool sdOk = sdStore.isOk() || sdStore.healthCheck();
  sPrintf("SD card    : %s (%u pending batch file(s), %d%% used)\n",
          sdOk ? "OK" : "FAIL", sdStore.queueFiles(), sdStore.usedPercent());

  float bv = battery.volts();
  bool batOk = (bv > 2.8f && bv < (BATT_V_FULL + 0.10f));   // S21 charges to 4.40 V
  sPrintf("Battery    : %.3fV (%d%%) cal %.4f — %s\n", bv, battery.percent(),
          battery.getCalibration(),
          batOk ? "OK" : "IMPLAUSIBLE — check divider on GPIO34, then VCAL");

#if ENABLE_CHARGE_SENSE
  sPrintf("Charge     : charging=%s full=%s (CHRG GPIO%d / STDBY GPIO%d, "
          "external 10k pull-ups REQUIRED)\n",
          battery.charging() ? "YES" : "no", battery.chargeFull() ? "YES" : "no",
          CHRG_SENSE_PIN, FULL_SENSE_PIN);
#else
  sPrintf("Charge     : sensing disabled at build time\n");
#endif

  bool e2eRan = false, e2eOk = false;
  if (mqtt && mqtt->connected()) {
    e2eRan = true;
    char probe[160];
    snprintf(probe, sizeof(probe), "{\"diag\":\"selftest\",\"device\":\"%s\",\"ms\":%lu}",
             configMgr.cfg.deviceId.c_str(), millis());
    mqttEnsureBuffer(strlen(probe) + 160);
    e2e.expectLive(probe, strlen(probe));
    bool pub = mqtt->publish(configMgr.diagTopic().c_str(), probe);
    unsigned long until = millis() + 4000;
    while (millis() < until && e2e.livePending) { mqtt->loop(); delay(10); }
    e2eOk = !e2e.livePending && pub;
    e2e.livePending = false;    // never leave a stale probe hash armed
    sPrintf("E2E echo   : %s\n", e2eOk ? "VERIFIED — publish path proven end to end"
                                       : "NO ECHO — broker reachable but echo timed out");
  } else {
    sPrintf("E2E echo   : skipped (no MQTT yet) — re-runs on first connect\n");
  }

  static char js[288];
  snprintf(js, sizeof(js),
    "{\"ran\":true,\"gpsUart\":%s,\"gpsFix\":%s,\"sd\":%s,\"battery\":%s,"
    "\"batV\":%.2f,\"e2eRan\":%s,\"e2eOk\":%s,\"charging\":%s,\"queueFiles\":%u}",
    gpsUart ? "true" : "false", gpsFix ? "true" : "false", sdOk ? "true" : "false",
    batOk ? "true" : "false", bv, e2eRan ? "true" : "false", e2eOk ? "true" : "false",
    battery.charging() ? "true" : "false", sdStore.queueFiles());
  selfTestJson = String(js);
  sPrintf("---------- SELF-TEST DONE ----------\n");
}

// ============================================================================
//  SERIAL CONSOLE — line based, non-blocking, runs on the loop task.
// ============================================================================
void handleSerialConsole() {
  static String buf = "";
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      buf.trim();
      if (buf.length()) processSerialCommand(buf);
      buf = "";
    } else if (buf.length() < 96) buf += c;
  }
}

String resolveLogPath(String arg) {
  arg.trim();
  if (!arg.length()) return "";
  bool numeric = true;
  for (size_t i = 0; i < arg.length(); i++) if (!isdigit(arg[i])) { numeric = false; break; }
  if (numeric) {
    static String paths[64];
    int n = sdStore.listAllLogPaths(paths, 64);
    int idx = arg.toInt();
    if (idx < 1 || idx > n) { sPrintf("[SD] No file #%d (LS shows %d)\n", idx, n); return ""; }
    return paths[idx - 1];
  }
  if (arg.startsWith("/")) return arg;
  return String(DIR_QUEUE) + "/" + arg;
}

void processSerialCommand(String cmd) {
  String verb = cmd, arg = "";
  int sp = cmd.indexOf(' ');
  if (sp > 0) { verb = cmd.substring(0, sp); arg = cmd.substring(sp + 1); arg.trim(); }
  verb.toUpperCase();

  if (verb == "HELP") {
    sPrintf("[CONSOLE] Commands\n"
            "  STAT            live status\n"
            "  NET             connectivity manager state\n"
            "  SD              card statistics\n"
            "  QUEUE           FIFO state (files, tail, sync offset)\n"
            "  LS              list batch files (pending + sent)\n"
            "  CAT <n|path>    dump a batch file raw (JSONL)\n"
            "  PLAY <n|path>   pretty-print a batch file as a table\n"
            "  DEL <n|path>    delete a batch file\n"
            "  SYNC            force a sync attempt now\n"
            "  SIM             staged cellular bring-up test (does not touch WiFi)\n"
            "  TIME            clock source, age, and next sync attempt\n"
            "  TIME SYNC       force a network time sync attempt now\n"
            "  NETSIM OFF|ON|AUTO   simulate an outage without touching the radio\n"
            "  TRACE ON|OFF    per-record raw JSON trace\n"
            "  VCAL            battery ADC diagnostics + calibration commands\n"
            "  TEST            re-run the self-test\n");
    return;
  }

  if (verb == "STAT") {
    sPrintf("[STAT] Device %s | fw v%s | uptime %lus | heap %u\n",
            configMgr.cfg.deviceId.c_str(), FW_VERSION, millis() / 1000UL,
            (unsigned)ESP.getFreeHeap());
    sPrintf("[STAT] GPS  : fix %s | sats %d | hdop %.2f | quality %.0f%% | %s\n",
            gps.location.isValid() ? "YES" : "NO",
            gps.satellites.isValid() ? gps.satellites.value() : 0,
            gps.hdop.isValid() ? gps.hdop.hdop() : 99.9,
            leds.quality() * 100.0f, leds.gpsStale() ? "STALE >60s" : "fresh");
    {
      // §6: never present holdover as equally trustworthy as a fresh sync.
      // Show what set the clock, and how long ago, so drift is diagnosable.
      uint32_t age = timeSvc.sinceSyncSec();
      if (!timeSvc.synced()) {
        sPrintf("[STAT] Time : UNSYNCED — no source has ever set the clock "
                "(records ship as %s, time_valid:false)\n", TS_UNSYNCED);
      } else {
        sPrintf("[STAT] Time : %s (BST, UTC+6) | now %s | ref %s %lus ago\n",
                timeSvc.formatStr().c_str(), timeSrcName(timeSvc.source()),
                timeSrcName(timeSvc.refSource()), (unsigned long)age);
      }
    }
    sPrintf("[STAT] Batt : %.3fV (%d%%) gain %.4f offset %+.4f | charging %s | full %s\n",
            battery.volts(), battery.percent(), battery.getCalibration(),
            battery.getCalibrationOffset(),
            battery.charging() ? "YES" : "no", battery.chargeFull() ? "YES" : "no");
    sPrintf("[STAT] Link : online %s | transport %s | MQTT %s | netsim %s\n",
            linkOnline ? "YES" : "NO",
            usingWiFi ? "WiFi" : (modemPowered ? "SIM" : "none"),
            (mqtt && mqtt->connected()) ? "connected" : "down",
            netSim == NETSIM_FORCE_OFF ? "FORCED-OFF"
              : netSim == NETSIM_FORCE_ON ? "FORCED-ON" : "AUTO");
    sPrintf("[STAT] Sync : %s | synced %u | E2E live %u/%u (lost %u) bulk %u/%u\n",
            syncLabel(syncEngine.status), syncEngine.syncedRecords,
            e2e.liveVerified, e2e.liveSent, e2e.liveLost, e2e.pbVerified, e2e.pbSent);
    sPrintf("[STAT] SD   : %s | %d%% used | pending %u file(s) + %u tail record(s)\n",
            sdStore.isOk() ? "OK" : "FAIL", sdStore.usedPercent(),
            sdStore.queueFiles(), sdStore.tailRecords());
    sPrintf("[STAT] Loss : queue-drop %lu | sd-write-err %lu | storage-evicted %lu\n",
            (unsigned long)droppedRecords, (unsigned long)sdStore.writeErrors(),
            (unsigned long)sdStore.evicted());
    return;
  }

  if (verb == "NET") {
    const char* ph = (wifiPhase == WIFI_PH_UP) ? "UP"
                   : (wifiPhase == WIFI_PH_TRYING) ? "TRYING" : "IDLE";
    sPrintf("[NET] WiFi phase %s | SSID '%s' | STA %s | RSSI %d dBm\n", ph,
            configMgr.cfg.wifiSsid.c_str(), WiFi.localIP().toString().c_str(),
            (int)WiFi.RSSI());
    sPrintf("[NET] Hotspot BB-TRACKER-%s @ 192.168.4.1 | clients %d\n",
            configMgr.cfg.deviceId.c_str(), WiFi.softAPgetStationNum());
    sPrintf("[NET] MQTT %s (%s:%d) | live '%s' | bulk '%s'\n",
            (mqtt && mqtt->connected()) ? "connected" : "down",
            configMgr.cfg.broker.c_str(), configMgr.cfg.port,
            configMgr.cfg.topic.c_str(), configMgr.bulkTopic().c_str());
    sPrintf("[NET] Next WiFi retry %lds | backoff %lus | SIM %s%s\n",
            (wifiPhase == WIFI_PH_IDLE) ? ((long)wifiNextTry - (long)millis()) / 1000L : 0L,
            wifiBackoff / 1000UL, modemPowered ? "powered" : "off",
            forceSetupMode ? " | FORCED SETUP MODE" : "");
    return;
  }

  if (verb == "QUEUE") {
    sPrintf("[QUEUE] pending batch files : %u\n", sdStore.queueFiles());
    sPrintf("[QUEUE] tail (partial) file : %s (%u/%d records)\n",
            sdStore.tailPath().length() ? sdStore.tailPath().c_str() : "(none)",
            sdStore.tailRecords(), configMgr.cfg.batchSize);
    sPrintf("[QUEUE] backlog present     : %s\n", sdStore.hasBacklog() ? "YES" : "no");
    sPrintf("[QUEUE] sync state          : %s on %s at record %u\n",
            syncLabel(syncEngine.status),
            syncEngine.currentFile()[0] ? syncEngine.currentFile() : "(nothing)",
            syncEngine.offsetInFile());
    sPrintf("[QUEUE] records delivered   : %u this boot\n", syncEngine.syncedRecords);
    sPrintf("[QUEUE] next record seq     : %lu\n", (unsigned long)sdStore.totalRecords());
    return;
  }

  if (verb == "LS") {
    static String paths[64];
    int n = sdStore.listAllLogPaths(paths, 64);
    sPrintf("[SD] %d batch file(s):\n", n);
    for (int i = 0; i < n; i++)
      sPrintf("  %2d. %-46s %5u records%s\n", i + 1, paths[i].c_str(),
              sdStore.countLines(paths[i]),
              (paths[i] == sdStore.tailPath()) ? "  [TAIL - still filling]" : "");
    if (!n) sPrintf("  (none)\n");
    return;
  }

  if (verb == "CAT" || verb == "PLAY") {
    String path = resolveLogPath(arg);
    if (!path.length()) { sPrintf("[SD] Usage: %s <n|path> — run LS first\n", verb.c_str()); return; }
    if (!sdStore.isOk()) { sPrintf("[SD] Card not available\n"); return; }
    uint32_t total = sdStore.countLines(path);
    sPrintf("[SD] ---- %s (%u records) ----\n", path.c_str(), total);
    if (verb == "PLAY")
      sPrintf("  SEQ       TIMESTAMP (BST)       LATITUDE      LONGITUDE      SPD    SATS  BAT   NET\n");
    GpsRecord rr[20];
    for (uint32_t off = 0; off < total; off += 20) {
      int got = sdStore.readRecords(path.c_str(), (uint16_t)off, 20, rr);
      if (got <= 0) break;
      for (int i = 0; i < got; i++) {
        if (verb == "CAT") {
          char raw[REC_LINE_MAX];
          SDStore::_serialise(rr[i], raw, sizeof(raw));
          sPrintf("%s\n", raw);
        } else {
          sPrintf("  %-9lu %-21s %-13.7f %-14.7f %-6.2f %-5u %-5u %s\n",
                  (unsigned long)rr[i].seq, rr[i].ts, rr[i].lat, rr[i].lng,
                  rr[i].spd, (unsigned)rr[i].sats, (unsigned)rr[i].batPct,
                  netName(rr[i].net));
        }
      }
      wdtFeed();
    }
    sPrintf("[SD] ---- end ----\n");
    return;
  }

  if (verb == "DEL") {
    String path = resolveLogPath(arg);
    if (!path.length()) { sPrintf("[SD] Usage: DEL <n|path>\n"); return; }
    sPrintf("[SD] %s %s\n", sdStore.deleteFile(path) ? "Deleted" : "FAILED to delete",
            path.c_str());
    return;
  }

  if (verb == "SD") {
    SDStore::LogStats s = sdStore.getStats();
    sPrintf("[SD] Card %s | %llu MB used / %llu MB total (%d%%)\n",
            sdStore.isOk() ? "OK" : "FAIL",
            (unsigned long long)(sdStore.usedBytes()  / (1024ULL * 1024ULL)),
            (unsigned long long)(sdStore.totalBytes() / (1024ULL * 1024ULL)),
            sdStore.usedPercent());
    sPrintf("[SD] Files %u | records %u | log bytes %llu\n",
            s.fileCount, s.totalLines, (unsigned long long)s.totalLogBytes);
    sPrintf("[SD] Oldest %s\n[SD] Newest %s\n",
            s.oldestFile.length() ? s.oldestFile.c_str() : "-",
            s.newestFile.length() ? s.newestFile.c_str() : "-");
    sPrintf("[SD] Auto-delete %s at %d%% | evicted %lu | write errors %lu\n",
            configMgr.cfg.autoDelete ? "ON" : "OFF", configMgr.cfg.maxStoragePct,
            (unsigned long)sdStore.evicted(), (unsigned long)sdStore.writeErrors());
    return;
  }

  if (verb == "SYNC") { syncEngine.triggerNow(); sPrintf("[SYNC] Triggered\n"); return; }

  if (verb == "NETSIM") {
    String a = arg; a.toUpperCase();
    if (a == "OFF")       { netSim = NETSIM_FORCE_OFF; sPrintf("[NETSIM] Simulated OFFLINE — radio untouched, records go to SD\n"); }
    else if (a == "ON")   { netSim = NETSIM_FORCE_ON;  sPrintf("[NETSIM] Simulated ONLINE — real link must be up\n"); }
    else if (a == "AUTO") { netSim = NETSIM_AUTO;      sPrintf("[NETSIM] AUTO — using the real link state\n"); }
    else sPrintf("[NETSIM] Usage: NETSIM OFF|ON|AUTO\n");
    return;
  }

  if (verb == "TRACE") {
    String a = arg; a.toUpperCase();
    traceRecords = (a == "ON");
    sPrintf("[TRACE] Per-record raw trace %s\n", traceRecords ? "ON" : "OFF");
    return;
  }

  if (verb == "VCAL") {
    String a = arg; a.toUpperCase();

    // ---- diagnostics. This is the root-cause tool: compare "ADC pin" against
    // a DMM reading taken at GPIO34 to separate an ADC error from a divider
    // error, and compare "Uncalibrated" against the battery terminals.
    if (a.length() == 0 || a == "RAW") {
      sPrintf("[VCAL] ADC pin      : %.1f mV   <- measure GPIO%d to GND, compare\n",
              battery.rawPinMillivolts(), BATT_ADC_PIN);
      sPrintf("[VCAL] Divider      : R1 %.0fk / R2 %.0fk, nominal ratio x%.4f\n",
              BATT_R1 / 1000.0f, BATT_R2 / 1000.0f,
              (BATT_R1 + BATT_R2) / BATT_R2);
      sPrintf("[VCAL] Uncalibrated : %.3f V   <- measure the battery terminals, compare\n",
              battery.uncalibratedVolts());
      sPrintf("[VCAL] Correction   : gain %.4f, offset %+.4f V\n",
              battery.getCalibration(), battery.getCalibrationOffset());
      sPrintf("[VCAL] Reported     : %.3f V (%d%%)\n",
              battery.volts(), battery.percent());
      if (batteryCal.p1True > 2.5f)
        sPrintf("[VCAL] Pending P1   : true %.3f V @ uncalibrated %.3f V "
                "(run VCAL P2 <v> at least %.2f V away)\n",
                batteryCal.p1True, batteryCal.p1Raw, BATT_CAL_MIN_SPAN_V);
      sPrintf("[VCAL] Status       : %s | source ref %.3f V | firmware "
              "constant %.3f V\n",
              batteryCal.valid ? "VALID" : "NOT CALIBRATED",
              batteryCal.ref, (float)BATTERY_CAL_REFERENCE_VOLTAGE);
      sPrintf("[VCAL] Commands     : VCAL <v> | VCAL P1 <v> | VCAL P2 <v> | "
              "VCAL RESET | VCAL RAW | VCAL DIAG\n");
      return;
    }

    if (a == "DIAG") { batteryDiagnostics(); return; }

    if (a == "RESET") {
      battery.setCalibration(1.0f, 0.0f);
      bcalErase();
      batteryCal = BatteryCal();
      sPrintf("[VCAL] Calibration ERASED: gain 1.0000, offset 0.\n");
      sPrintf("[VCAL] Reboot now and the firmware will recalibrate against "
              "BATTERY_CAL_REFERENCE_VOLTAGE (%.3f V) if it is set.\n",
              (float)BATTERY_CAL_REFERENCE_VOLTAGE);
      return;
    }

    // ---- two-point, step 1: remember where we are ------------------------
    if (a.startsWith("P1")) {
      float v = arg.substring(2).toFloat();
      if (v < 2.5f || v > 4.60f) {
        sPrintf("[VCAL] Usage: VCAL P1 <measured_volts> (2.5-4.6)\n");
        return;
      }
      batteryCal.p1True = v;
      batteryCal.p1Raw  = battery.uncalibratedVolts();
      bcalSave(batteryCal);
      sPrintf("[VCAL] Point 1 stored: true %.3f V at uncalibrated %.3f V\n",
              batteryCal.p1True, batteryCal.p1Raw);
      sPrintf("[VCAL] Now charge or discharge the pack by at least %.2f V, then "
              "run VCAL P2 <measured_volts>. The point survives a reboot.\n",
              BATT_CAL_MIN_SPAN_V);
      return;
    }

    // ---- two-point, step 2: solve gain and offset ------------------------
    if (a.startsWith("P2")) {
      float v2t = arg.substring(2).toFloat();
      if (v2t < 2.5f || v2t > 4.60f) {
        sPrintf("[VCAL] Usage: VCAL P2 <measured_volts> (2.5-4.6)\n");
        return;
      }
      if (batteryCal.p1True < 2.5f) {
        sPrintf("[VCAL] No point 1 stored — run VCAL P1 <volts> first\n");
        return;
      }
      float v2r    = battery.uncalibratedVolts();
      float before = battery.volts();
      if (!battery.calibrateTwoPoint(batteryCal.p1True,
                                     batteryCal.p1Raw, v2t, v2r)) {
        sPrintf("[VCAL] REFUSED — the two readings are %.3f V apart (need %.2f V) "
                "or the solve was implausible. Point 1 kept; move the pack "
                "further and retry.\n",
                fabsf(v2r - batteryCal.p1Raw), BATT_CAL_MIN_SPAN_V);
        return;
      }
      batteryCal.gain   = battery.getCalibration();
      batteryCal.offset = battery.getCalibrationOffset();
      batteryCal.ref    = v2t;
      batteryCal.p1True = 0.0f;
      batteryCal.p1Raw  = 0.0f;
      bcalSave(batteryCal);
      sPrintf("[VCAL] %.3fV → %.3fV (gain %.4f, offset %+.4f V persisted)\n",
              before, battery.volts(), batteryCal.gain, batteryCal.offset);
      return;
    }

    // ---- single point, gain only (the original behaviour) ----------------
    float v = arg.toFloat();
    if (v < 2.5f || v > 4.60f) {
      sPrintf("[VCAL] Usage: VCAL <measured_volts> (2.5-4.6). Measure the ACTUAL\n"
              "[VCAL] battery terminal voltage with a multimeter first.\n"
              "[VCAL] Run bare VCAL for diagnostics and the full command list.\n");
      return;
    }
    float before = battery.volts();
    float cal = battery.calibrateTo(v);
    batteryCal.gain   = cal;
    batteryCal.offset = 0.0f;
    batteryCal.ref    = v;
    bcalSave(batteryCal);
    sPrintf("[VCAL] %.3fV → %.3fV (gain %.4f persisted, offset cleared)\n",
            before, battery.volts(), cal);
    return;
  }

  if (verb == "TIME") {
    if (arg.equalsIgnoreCase("SYNC")) {
      timeSvc.forceResync();
      sPrintf("[TIME] Network sync forced — next taskNet tick will attempt it\n");
      return;
    }
    bool ok = timeSvc.synced();
    sPrintf("[TIME] Clock  : %s%s\n", timeSvc.formatStr().c_str(),
            ok ? " (BST, UTC+6)" : "  <-- NOTHING HAS EVER SET THE CLOCK");
    sPrintf("[TIME] Source : effective %s | reference %s | %lus since last sync\n",
            timeSrcName(timeSvc.source()), timeSrcName(timeSvc.refSource()),
            (unsigned long)timeSvc.sinceSyncSec());
    sPrintf("[TIME] Auth   : NTP/CELL > GPS > HOLDOVER > UNSYNCED  "
            "(v6.2 network-first; GPS takes the clock only once the network "
            "reference is starved or older than %lus)\n",
            (unsigned long)NET_FRESH_S);
    sPrintf("[TIME] GPS    : date %s | time %s | age %lums (refused above %lums)\n",
            gps.date.isValid() ? "valid" : "never seen",
            gps.time.isValid() ? "valid" : "never seen",
            (unsigned long)(gps.time.age() > 86400000UL ? 0 : gps.time.age()),
            GPS_TIME_MAX_AGE_MS);
    sPrintf("[TIME] Net    : phase %s | WiFi %s | modem %s | servers %s, %s, %s\n",
            timeSvc.phaseName(),
            (wifiPhase == WIFI_PH_UP && WiFi.status() == WL_CONNECTED) ? "up" : "down",
            modemPowered ? (modemInternetAvailable() ? "GPRS up" : "powered") : "off",
            NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
    sPrintf("[TIME] Floor  : dates before %d-01-01 are refused from every source\n",
            TIME_BUILD_YEAR);
    return;
  }

  if (verb == "SIM") {
    simDiagRequest = true;
    sPrintf("[SIM] Bring-up test queued — staged results print from the "
            "network task in a few seconds. Wi-Fi/MQTT are not disturbed.\n");
    return;
  }

  if (verb == "TEST") { runSelfTest(); return; }

  sPrintf("[CONSOLE] Unknown command '%s' — type HELP\n", verb.c_str());
}

// ============================================================================
//  MODEM / SIM — all cellular code is isolated behind the build switch and
//  runs exclusively on the network task, where a blocking AT transaction
//  costs nothing to GPS acquisition, SD logging, LEDs or the dashboard.
// ============================================================================
void initModemFunctionality() {
#if ENABLE_MODEM_FUNCTIONALITY
  modemSerial.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  pinMode(PWRKEY, OUTPUT);
  digitalWrite(PWRKEY, LOW);
  DBG("SIM", "Modem UART ready (RX%d/TX%d) — powered on demand", MODEM_RX, MODEM_TX);
#else
  DBG("SIM", "Modem disabled at build time (WiFi-only firmware)");
#endif
}

#if ENABLE_MODEM_FUNCTIONALITY
// PWRKEY on the A7670C/SIM7600 is a TOGGLE, not an ON switch. v6.2 pulsed it
// unconditionally on the first fallback attempt of every boot. That is right
// after a cold power-up and exactly wrong after a SOFT reset — the supervisor
// reboot in loop(), a watchdog panic, /api/restart, the queue-alloc
// ESP.restart() — because the modem is still running from the previous
// session. The pulse then switches it OFF, modemPowered is latched true so no
// second pulse is ever issued, and every modem.init() for the rest of the
// session fails on a modem that is simply powered down.
//
// Probe first; pulse only if nothing answers. Then wait for AT to actually
// respond rather than guessing: an A7670C needs 5-12 s from PWRKEY to
// AT-ready, and the old fixed delay(3000) gave up on a modem that was about
// to become available.
static bool powerOnModem() {
  if (modemPowered) return true;

  if (modem.testAT(1500L)) {
    DBG("SIM", "Modem already powered (soft reset) — PWRKEY pulse skipped");
    modemPowered = true;
    return true;
  }

  DBG("SIM", "Powering modem (PWRKEY pulse)");
  digitalWrite(PWRKEY, HIGH);
  delay(1200);
  digitalWrite(PWRKEY, LOW);

  for (int i = 1; i <= 15; i++) {
    delay(1000);
    if (modem.testAT(500L)) {
      DBG("SIM", "Modem answered AT after %d s", i);
      modemPowered = true;
      return true;
    }
  }

  LOGW("SIM", "No AT response %ds after PWRKEY pulse — check PWRKEY polarity "
              "and the modem VBAT rail", 15);
  return false;
}
#endif

bool connectSIM() {
#if ENABLE_MODEM_FUNCTIONALITY
  if (!powerOnModem())                  { DBG("SIM", "power-on FAILED"); return false; }
  if (!modem.init())                    { DBG("SIM", "init FAILED"); return false; }
  if (!modem.waitForNetwork(30000UL))   { DBG("SIM", "no network registration"); return false; }
  if (!modem.gprsConnect(configMgr.cfg.apn.c_str(), "", "")) {
    DBG("SIM", "GPRS connect FAILED (APN '%s')", configMgr.cfg.apn.c_str());
    return false;
  }
  LOGW("SIM", "GPRS UP — IP %s", modem.getLocalIP().c_str());
  return true;
#else
  return false;
#endif
}

bool modemInternetAvailable() {
#if ENABLE_MODEM_FUNCTIONALITY
  return modemPowered && modem.isGprsConnected();
#else
  return false;
#endif
}

bool connectModemFallback() {
#if ENABLE_MODEM_FUNCTIONALITY
  // Already up? Do NOT re-run connectSIM(). TinyGSM's gprsConnect() opens with
  // gprsDisconnect() -> AT+NETCLOSE, which closes EVERY socket on the modem,
  // the live MQTT connection included. v6.2 reached this function every
  // SIM_RETRY_MS for as long as WiFi kept failing — and WiFi keeps failing
  // forever on a SIM deployment — so a working cellular link was demolished
  // and rebuilt once a minute, indefinitely. That alone reads as "SIM does
  // not work".
  //
  // The same path is also the worst-case block on taskNet: init (~10 s) +
  // waitForNetwork (30 s) + CGATT (60 s) + CGACT (60 s) + CGDCONT/NETOPEN
  // lands within a few seconds of NET_TASK_STALL_MS, so a slow re-attach can
  // trip the supervisor in loop() and reboot the device mid-connection.
  if (modemInternetAvailable()) return true;

  DBG("SIM", "WiFi unavailable — attempting SIM fallback");
  if (!connectSIM()) return false;
  useSimTransport();
  mqttNextTry = millis();
  timeSvc.forceResync();         // carrier time is available as soon as we register
  return true;
#else
  return false;
#endif
}

// Access technology straight from the modem (AT+CPSI?). v5.2 never
// populated sim.networkType, which is why the dashboard's SIM panel showed
// a blank Network Type row.
// SIM FIX: same conversion as _queryModemClock(), same reason. The old body
// ran "while (modemSerial.available()) modemSerial.read();" every 30 s, which
// discarded whatever socket data PubSubClient had not collected yet and left
// late replies in the buffer to desync the next TinyGSM command.
String queryAccessTech() {
#if ENABLE_MODEM_FUNCTIONALITY
  if (!modemPowered) return "";
  String resp;
  modem.sendAT("+CPSI?");
  if (modem.waitResponse(1200L, resp) != 1) return "";
  int i = resp.indexOf("+CPSI:");
  if (i < 0) return "";
  int s = i + 6, e = resp.indexOf(',', s);
  if (e < 0) e = resp.indexOf('\r', s);
  if (e < 0) return "";
  String tech = resp.substring(s, e);
  tech.trim();
  return tech;                    // e.g. "LTE", "GSM", "NO SERVICE"
#else
  return "";
#endif
}

// ============================================================================
//  SIM DIAGNOSTIC — console "SIM".
//
//  WHY THIS EXISTS. connectModemFallback() is only reachable from the
//  WIFI_PH_TRYING timeout branch in connectivityTick(). Once Wi-Fi associates
//  the phase becomes WIFI_PH_UP and that branch is never entered again, so on
//  a unit with working Wi-Fi the modem is NEVER powered, init() NEVER runs,
//  and modemInternetAvailable() returns false without asking the modem
//  anything. The cellular path is therefore untested and untestable in normal
//  operation — it looks broken when nothing has tried it.
//
//  This runs the real bring-up, one stage at a time, and does NOT call
//  useSimTransport(), so a working Wi-Fi MQTT session is left alone. Each
//  stage names its own failure. Runs on taskNet; hbNet is refreshed between
//  stages so the NET_TASK_STALL_MS supervisor cannot fire mid-test.
// ============================================================================
void simDiagnostic() {
#if ENABLE_MODEM_FUNCTIONALITY
  LOGW("SIM", "---------- SIM BRING-UP TEST ----------");
  hbNet = millis();

  if (!powerOnModem()) {
    LOGW("SIM", "1/5 POWER  FAIL — no AT response after the PWRKEY pulse.");
    LOGW("SIM", "    Hardware, not firmware: wrong PWRKEY polarity for this "
                "board, or the modem VBAT rail browned out on inrush.");
    LOGW("SIM", "---------- END ----------");
    return;
  }
  LOGW("SIM", "1/5 POWER  OK — modem answers AT");
  hbNet = millis();

  if (!modem.init()) {
    LOGW("SIM", "2/5 INIT   FAIL — modem answers AT but init() refused. "
                "Check that the SIM tray is seated.");
    LOGW("SIM", "---------- END ----------");
    return;
  }
  LOGW("SIM", "2/5 INIT   OK");
  hbNet = millis();

  SimStatus ss = modem.getSimStatus();
  LOGW("SIM", "3/5 SIM    %s",
       ss == SIM_READY  ? "READY"
     : ss == SIM_LOCKED ? "PIN-LOCKED — remove the PIN on this SIM (this "
                          "firmware does not send one)"
                        : "NOT DETECTED — reseat the card");
  if (ss != SIM_READY) { LOGW("SIM", "---------- END ----------"); return; }
  hbNet = millis();

  if (!modem.waitForNetwork(30000UL)) {
    LOGW("SIM", "4/5 REG    FAIL — no registration in 30 s (CSQ %d). Antenna "
                "not fitted, no coverage, or the SIM is not provisioned.",
         modem.getSignalQuality());
    LOGW("SIM", "---------- END ----------");
    return;
  }
  LOGW("SIM", "4/5 REG    OK — operator '%s', CSQ %d, tech %s",
       modem.getOperator().c_str(), modem.getSignalQuality(),
       queryAccessTech().c_str());
  hbNet = millis();

  if (!modem.gprsConnect(configMgr.cfg.apn.c_str(), "", "")) {
    LOGW("SIM", "5/5 GPRS   FAIL — the network rejected APN '%s'.",
         configMgr.cfg.apn.c_str());
    LOGW("SIM", "    This is the most common cause. Bangladesh APNs: "
                "Grameenphone 'gpinternet' | Robi & Airtel 'internet' | "
                "Banglalink 'blweb' | Teletalk 'gprsunl'. Set it on the "
                "dashboard Config tab, then run SIM again.");
    LOGW("SIM", "---------- END ----------");
    return;
  }

  LOGW("SIM", "5/5 GPRS   OK — IP %s", modem.getLocalIP().c_str());
  LOGW("SIM", "Cellular internet is working. The modem stays attached and "
              "will carry MQTT automatically the moment Wi-Fi drops.");
  LOGW("SIM", "---------- END ----------");
  hbNet = millis();
#else
  LOGW("SIM", "Modem excluded at build time (ENABLE_MODEM_FUNCTIONALITY 0)");
#endif
}

// ============================================================================
//  ALWAYS-ON WEB — hotspot + captive DNS + dashboard, started once at boot
//  and never stopped. The radio stays in WIFI_AP_STA for the life of the
//  device so the STA leg can reconnect in the background while the AP leg
//  keeps serving http://192.168.4.1 in every state.
//
//  Hardware note: AP and STA share one radio. When the STA associates, the
//  AP hops to the router's channel and hotspot clients reconnect within a
//  few seconds. That is a chip constraint, not a firmware bug.
// ============================================================================
void startAlwaysOnWeb() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);

  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  String apName = "BB-TRACKER-" + configMgr.cfg.deviceId;
  WiFi.softAP(apName.c_str(), configMgr.cfg.apPass.c_str());

  dnsServer.start(53, "*", apIP);
  webDash.startRuntime(server, configMgr);

  LOGW("WEB", "Hotspot '%s' → http://192.168.4.1 (available in EVERY state)",
       apName.c_str());
}