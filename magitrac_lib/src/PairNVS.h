// PairNVS.h — persistent pairing storage
//
// Per-frame signing was removed when the transport switched to pure ESP-NOW
// (link-layer ACK + MAC filtering only).  The 16-byte secret is still
// exchanged during the pairing ceremony and stored here so signing can be
// re-added later without forcing every device to re-pair.
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
