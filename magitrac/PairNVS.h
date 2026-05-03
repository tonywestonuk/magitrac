// PairNVS.h — persistent pairing storage and HMAC helpers
// Copy this file to both magitrac_server and magitrac projects.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ── NVS helpers ───────────────────────────────────────────────────────────────
// ns: "magitrac_srv" (server) or "magitrac_cli" (client)

// Load stored pairing. Returns true if a valid pairing exists.
bool pairNvsLoad(const char* ns, uint8_t* mac6, uint8_t* secret16);

// Persist pairing.
void pairNvsSave(const char* ns, const uint8_t* mac6, const uint8_t* secret16);

// Forget pairing (clear paired flag).
void pairNvsClear(const char* ns);

// ── HMAC helpers ──────────────────────────────────────────────────────────────
// Compute HMAC-SHA256(secret16, data, dataLen) → first 8 bytes → out8.
void hmacSha256_8(const uint8_t* secret16,
                  const uint8_t* data, size_t dataLen,
                  uint8_t* out8);

// Constant-time comparison. Returns true if first 8 bytes of HMAC match expected8.
bool hmacVerify8(const uint8_t* secret16,
                 const uint8_t* data, size_t dataLen,
                 const uint8_t* expected8);
