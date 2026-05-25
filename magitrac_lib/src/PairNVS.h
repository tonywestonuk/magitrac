// PairNVS.h — persistent pairing storage
//
// The original flat {mac, secret} schema (back from the ESP-NOW-only era)
// is preserved unchanged.  Two new optional fields are added on top for
// the TCP transport:
//
//   • Server-side ("magitrac_srv"): the AP creds + static IP issued at
//     pairing time, so the server can rejoin the client's SoftAP on every
//     boot without re-pairing.
//
//   • Client-side ("magitrac_cli"): the client's *own* AP creds (generated
//     once on first boot and persisted forever) plus a "next host octet"
//     counter for handing out IPs to additional peers.  The magitrac_server
//     always gets 192.168.0.2 (reserved), so the counter starts at 3 and
//     is reserved for future pixelpost peers when they migrate to TCP.
//
// The 16-byte shared secret is still exchanged and persisted even though
// nothing currently signs with it — kept so per-frame signing can be added
// back later without re-pairing.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ── Namespaces ───────────────────────────────────────────────────────────────
// ns: "magitrac_srv" (server) or "magitrac_cli" (client)

// ── Original {mac, secret} pairing (unchanged) ───────────────────────────────

// Load stored pairing.  Returns true if a valid pairing exists.
bool pairNvsLoad(const char* ns, uint8_t* mac6, uint8_t* secret16);

// Persist pairing.  Sets the paired flag.
void pairNvsSave(const char* ns, const uint8_t* mac6, const uint8_t* secret16);

// Forget pairing (clear paired flag).  Leaves the AP creds and IP counter
// intact — those are device-lifetime, not pairing-lifetime.
void pairNvsClear(const char* ns);

// ── Server-side: TCP creds (the AP it should join) ───────────────────────────
//
// Returns true if creds have been stored (i.e. a TCP-era pairing has
// completed at least once).  Persists across pairNvsClear() so we keep
// the last-known network in case the user wants to re-pair against it.

bool pairNvsLoadCreds(const char* ns,
                      char    ssid_out[33],
                      char    psk_out[64],
                      uint8_t my_ip_out[4],
                      uint8_t gw_ip_out[4]);

void pairNvsSaveCreds(const char* ns,
                      const char*    ssid,
                      const char*    psk,
                      const uint8_t  my_ip[4],
                      const uint8_t  gw_ip[4]);

// Drop the TCP creds flag so the next boot falls back to ESP-NOW
// (pair-ready) mode.  Does not touch the {mac, secret} pairing or the
// client-side AP info — call pairNvsClear separately if you also want
// to forget the paired peer's identity.
void pairNvsClearCreds(const char* ns);

// ── Client-side: own AP info + next-IP counter ───────────────────────────────
//
// The PSK is generated randomly on first boot and persisted forever — the
// client treats it like its own identity.  next_host_octet starts at 3
// because .1 is the AP itself and .2 is the reserved magitrac_server slot.

bool pairNvsLoadApInfo(const char* ns,
                       char     ssid_out[33],
                       char     psk_out[64],
                       uint8_t* next_host_octet_out);

void pairNvsSaveApInfo(const char* ns,
                       const char* ssid,
                       const char* psk,
                       uint8_t     next_host_octet);

// ── DEBUG: dump every known key in `ns` to Serial ────────────────────────────
// Temporary aid for the NVS-corruption investigation.  Prints each key with
// presence, length, and value (PSK in plaintext — local hardware only).
// Safe to call at any time; opens the namespace read-only.
void pairNvsDump(const char* ns);
