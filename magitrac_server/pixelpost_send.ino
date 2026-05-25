// pixelpost_send.ino — outgoing messages to pixel_post devices.
//
// STUBBED 2026-05-25 — the previous ESP-NOW broadcast implementation
// was hammering esp_now_send() while the server runs in TCP-paired
// mode (where ESP-NOW is never initialised), producing constant
// `ESPNOW: not init` error spam that choked Serial and starved the
// TCP write path enough to drop the magitrac connection.
//
// Pixelpost is migrating to a TCP-based path so the ESP-NOW
// implementation has been removed wholesale.  Call sites
// (midi_player.cpp, magitrac_server.ino) keep using these function
// signatures; re-add bodies that route over the new transport when
// ready.

#include <stdint.h>
#include <stddef.h>

void pixelpostInit() {
    // no-op — pending TCP-pixelpost migration
}

void pixelpostSendPairingBeacon()              {}
void pixelpostSendSelectEffect(uint8_t /*idx*/) {}
void pixelpostSendTapped()                     {}
void pixelpostSendSlider(uint8_t /*value*/, bool /*pressed*/) {}
void pixelpostSendMove(uint8_t /*x*/, uint8_t /*y*/, bool /*pressed*/) {}

void pixelpostEnqueue(const uint8_t* /*payload*/, size_t /*len*/) {}
