// PairNVS.cpp — NVS pairing storage
#include "PairNVS.h"
#include <Preferences.h>

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
