// PairNVS.cpp — NVS pairing storage
#include "PairNVS.h"
#include <Preferences.h>
#include <Arduino.h>

// ── Paired-peer identity ─────────────────────────────────────────────────────

bool pairNvsLoad(const char* ns, uint8_t* mac6, uint8_t* secret16) {
    Preferences prefs;
    prefs.begin(ns, /*readOnly=*/true);
    bool paired = (prefs.getUChar("paired", 0) == 1);
    if (paired) {
        prefs.getBytes("mac",    mac6,     6);
        prefs.getBytes("secret", secret16, 16);
    }
    prefs.end();
    return paired;
}

void pairNvsSave(const char* ns, const uint8_t* mac6, const uint8_t* secret16) {
    Preferences prefs;
    prefs.begin(ns, /*readOnly=*/false);
    prefs.putBytes("mac",    mac6,     6);
    prefs.putBytes("secret", secret16, 16);
    prefs.putUChar("paired", 1);
    prefs.end();
}

void pairNvsClear(const char* ns) {
    Preferences prefs;
    prefs.begin(ns, /*readOnly=*/false);
    prefs.putUChar("paired", 0);
    prefs.end();
}

// ── WiFi credentials ────────────────────────────────────────────────────────

bool pairNvsLoadCreds(const char* ns,
                      char    ssid_out[33],
                      char    psk_out[64],
                      uint8_t* ap_mode_out) {
    Preferences prefs;
    prefs.begin(ns, /*readOnly=*/true);
    bool ok = (prefs.getUChar("creds", 0) == 1);
    if (ok) {
        prefs.getString("ssid",   ssid_out, 33);
        prefs.getString("psk",    psk_out,  64);
        *ap_mode_out = prefs.getUChar("apmode", 0);
    }
    prefs.end();
    return ok;
}

void pairNvsSaveCreds(const char* ns,
                      const char* ssid,
                      const char* psk,
                      uint8_t     ap_mode) {
    Preferences prefs;
    prefs.begin(ns, /*readOnly=*/false);
    prefs.putString("ssid",   ssid);
    prefs.putString("psk",    psk);
    prefs.putUChar ("apmode", ap_mode);
    prefs.putUChar ("creds",  1);
    prefs.end();
}

void pairNvsClearCreds(const char* ns) {
    Preferences prefs;
    prefs.begin(ns, /*readOnly=*/false);
    prefs.putUChar("creds", 0);
    prefs.end();
}

// ── DEBUG dump ──────────────────────────────────────────────────────────────

static void dumpUChar(Preferences& p, const char* key) {
    if (!p.isKey(key)) { Serial.printf("  %-7s: absent\n", key); return; }
    Serial.printf("  %-7s: val=%u\n", key, (unsigned)p.getUChar(key, 0));
}

static void dumpBytes(Preferences& p, const char* key, size_t expectedLen) {
    if (!p.isKey(key)) { Serial.printf("  %-7s: absent\n", key); return; }
    size_t n = p.getBytesLength(key);
    uint8_t buf[64];
    if (n > sizeof(buf)) n = sizeof(buf);
    size_t got = p.getBytes(key, buf, n);
    Serial.printf("  %-7s: len=%u expected=%u hex=", key,
                  (unsigned)got, (unsigned)expectedLen);
    for (size_t i = 0; i < got; ++i) Serial.printf("%02X ", buf[i]);
    Serial.println();
}

static void dumpString(Preferences& p, const char* key) {
    if (!p.isKey(key)) { Serial.printf("  %-7s: absent\n", key); return; }
    String s = p.getString(key);
    Serial.printf("  %-7s: len=%u val='%s'\n",
                  key, (unsigned)s.length(), s.c_str());
}

void pairNvsDump(const char* ns) {
    Preferences prefs;
    bool ok = prefs.begin(ns, /*readOnly=*/true);
    Serial.printf("[NVS-DUMP] ns=%s open=%d\n", ns, ok ? 1 : 0);
    if (!ok) {
        Serial.println("[NVS-DUMP] (namespace missing or corrupt)");
        return;
    }
    dumpUChar (prefs, "paired");
    dumpBytes (prefs, "mac",    6);
    dumpBytes (prefs, "secret", 16);
    dumpUChar (prefs, "creds");
    dumpString(prefs, "ssid");
    if (prefs.isKey("psk")) {
        size_t plen = prefs.getString("psk").length();
        Serial.printf("  %-7s: len=%u (redacted)\n", "psk", (unsigned)plen);
    } else {
        Serial.printf("  %-7s: absent\n", "psk");
    }
    dumpUChar (prefs, "apmode");
    prefs.end();
    Serial.println("[NVS-DUMP] end");
}
