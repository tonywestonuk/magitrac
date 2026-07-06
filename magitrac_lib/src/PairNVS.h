// PairNVS.h — persistent pairing storage
//
// Three things land in NVS once a pairing has happened:
//
//   • The paired-peer identity (mac + secret) — same for both devices.
//     Secret is currently zeros (no per-frame signing); slot kept so
//     signing can be re-added later without re-pairing.
//
//   • The WiFi credentials (ssid + psk) + apMode (server hosts AP, or
//     external AP).  The magitrac client owns these via the WiFi settings
//     page; pairing delivers them to the server in MsgPairOffer.  Both
//     sides write them to their own NVS so each can come up correctly on
//     reboot without the other present.
//
// No IP fields stored — both devices' static IPs are constants in
// MagiMsg.h (MAGI_SERVER_IP, MAGI_CLIENT_IP).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ── Namespaces ───────────────────────────────────────────────────────────────
// ns: "magitrac_srv" (server) or "magitrac_cli" (client)

// ── Paired-peer identity ─────────────────────────────────────────────────────

// Load stored pairing.  Returns true if a valid pairing exists.
bool pairNvsLoad(const char* ns, uint8_t* mac6, uint8_t* secret16);

// Persist pairing.  Sets the paired flag.
void pairNvsSave(const char* ns, const uint8_t* mac6, const uint8_t* secret16);

// Forget pairing (clear paired flag).  Leaves the WiFi creds intact so a
// re-pair attempt can default to the last-known network in the WiFi page.
void pairNvsClear(const char* ns);

// ── WiFi credentials ─────────────────────────────────────────────────────────
//
// apMode (uint8): 0 = SERVER_AP (server hosts the AP), 1 = EXTERNAL_AP
// (both server and client join an external AP).  Drives the boot-time
// branch in each sketch's setup().

bool pairNvsLoadCreds(const char* ns,
                      char    ssid_out[33],
                      char    psk_out[64],
                      uint8_t* ap_mode_out);

void pairNvsSaveCreds(const char* ns,
                      const char* ssid,
                      const char* psk,
                      uint8_t     ap_mode);

// Drop the creds flag.  Next boot falls back to ESP-NOW-only (pair-ready)
// mode.  Does not touch the {mac, secret} pairing — call pairNvsClear
// separately if you also want to forget the paired peer's identity.
void pairNvsClearCreds(const char* ns);

// ── DEBUG: dump every known key in `ns` to Serial ────────────────────────────
// Safe to call at any time; opens the namespace read-only.
void pairNvsDump(const char* ns);
