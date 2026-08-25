// ============================================================================
//  secrets.example.h — TEMPLATE. Copy to secrets.h and fill in.
//
//      cp secrets.example.h secrets.h
//
//  secrets.h is listed in .gitignore and must never be committed.
//
//  WHAT THESE ARE
//  These are the FIRST-BOOT DEFAULTS only. On a device that has already been
//  provisioned, NVS wins and these values are never read — the dashboard and
//  the serial console are the normal way to change configuration in the field.
//  They matter on a virgin board, after a factory reset, and after a
//  CONFIG_SCHEMA_VERSION bump.
//
//  WHY THE TOPIC IS IN HERE
//  broker.hivemq.com is a public broker with no authentication. The topic
//  string is therefore the only thing standing between the device's live
//  position feed and anyone who cares to subscribe. Treat it as a credential.
// ============================================================================
#pragma once

// ---- Wi-Fi station credentials -------------------------------------------
// Leave DEFAULT_WIFI_SSID as "" to boot with no station config at all; the
// device still raises its own AP and the dashboard is reachable there.
#define DEFAULT_WIFI_SSID   "your-wifi-ssid"
#define DEFAULT_WIFI_PASS   "your-wifi-password"

// ---- MQTT ----------------------------------------------------------------
// Live records publish to DEFAULT_MQTT_TOPIC. Bulk/offline batches go to the
// same topic in this build (see ConfigManager::bulkTopic).
#define DEFAULT_MQTT_TOPIC  "tracker/gps/your-unique-topic"

// ---- Device identity -----------------------------------------------------
// Appears in every payload, in the AP SSID, and on the dashboard.
#define DEFAULT_DEVICE_ID   "00000"

// ---- SoftAP -------------------------------------------------------------
// WPA2 requires 8 characters minimum.
#define DEFAULT_AP_PASS     "changeme8"

// ---- Dashboard PIN gates -------------------------------------------------
// Exactly 4 digits each. dev unlocks the full route/role matrix; user is
// read-mostly. See docs/CONSOLE.md for what each role can reach.
#define DEFAULT_DEV_PIN     "1010"
#define DEFAULT_USER_PIN    "0000"
