// PairNVS.cpp — NVS pairing storage + HMAC-SHA256 helpers
#include "PairNVS.h"
#include <Preferences.h>
#include <mbedtls/md.h>
#include <string.h>

// ── NVS ───────────────────────────────────────────────────────────────────────

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

// ── HMAC ─────────────────────────────────────────────────────────────────────

void hmacSha256_8(const uint8_t* secret16,
                  const uint8_t* data, size_t dataLen,
                  uint8_t* out8) {
    uint8_t full[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_setup(&ctx, info, /*hmac=*/1);
    mbedtls_md_hmac_starts(&ctx, secret16, 16);
    mbedtls_md_hmac_update(&ctx, data, dataLen);
    mbedtls_md_hmac_finish(&ctx, full);
    mbedtls_md_free(&ctx);
    memcpy(out8, full, 8);
}

bool hmacVerify8(const uint8_t* secret16,
                 const uint8_t* data, size_t dataLen,
                 const uint8_t* expected8) {
    uint8_t computed[8];
    hmacSha256_8(secret16, data, dataLen, computed);
    // Constant-time compare
    uint8_t diff = 0;
    for (int i = 0; i < 8; i++) diff |= (computed[i] ^ expected8[i]);
    return diff == 0;
}
