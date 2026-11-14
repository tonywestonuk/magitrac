// PairNVS.h — persistent pairing storage
//
// Two things land in NVS once a pairing has happened:
//
//   • The paired-peer identity (mac + secret) — each device stores the
//     other's MAC.  Secret is currently zeros (no per-frame signing); slot
//     kept so signing can be re-added later without re-pairing.
//
//   • The WiFi credentials (ssid + psk) + apMode (server hosts AP, or
//     external AP).  In SERVER_AP mode they are the server's own
//     Magitrac_XXXX softAP creds, handed to the client in MsgPairChallenge;
//     in EXTERNAL_AP mode they are what the user typed on the client's WiFi
//     settings page.  Either way the client sends them (back) in
//     MsgPairOffer and both sides write them to their own NVS, so each can
//     come up correctly on reboot without the other present.
//
// No IP fields stored.  In SERVER_AP mode both devices use the fixed
// addresses in MagiMsg.h (MAGI_SERVER_IP_*, MAGI_CLIENT_IP_*); in EXTERNAL_AP
// mode both take DHCP and the client finds the server from its
// MSG_SERVER_ANNOUNCE beacon.
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
// (No sketch calls this at present — there is no "forget pairing" UI.)
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

// ── DEBUG: dump every known key in `ns` to Serial ────────────────────────────
// Safe to call at any time; opens the namespace read-only.  The PSK is
// printed as a length only.  Not called from any sketch — drop a call in
// by hand when chasing a pairing / creds problem.
void pairNvsDump(const char* ns);
