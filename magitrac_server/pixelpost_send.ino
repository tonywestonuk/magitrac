// pixelpost_send.ino — outgoing ESP-NOW messages to pixel_post devices.
//
// Wire format (post-2026-05-28 redesign — see pixelpost_proto.h):
//   [4B uint32 LE timestamp = millis()][payload]
// No HMAC, no replay protection.  Posts lock their effect clock to the
// timestamp on every received packet.
//
// Coexists with MagiLink: gTransportEspNow.begin() in setup() initialises
// ESP-NOW; we just register a broadcast peer and use raw esp_now_send.
// All callers (mic_spectrum, midi_player) enqueue rather than send inline
// so time-critical tasks don't block on the ESP-NOW driver.

#include <Arduino.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <pixelpost_proto.h>
#include <string.h>
#include "mic_spectrum.h"        // spectrumSetActive
#include "MagiCommsEspNow.h"     // gTransportEspNow

extern MagiCommsEspNow gTransportEspNow;

// ── Queue ───────────────────────────────────────────────────────────────────
// Payload-only — the worker stamps the timestamp at send time so it's the
// true millis() at TX rather than at enqueue.  32 bytes covers every
// magitrac_server pixelpost message type with headroom (largest is
// SPECTRUM at 6 bytes; biggest plausible future is ~10).
#define PP_TX_PAYLOAD_MAX  32
#define PP_TX_QUEUE_DEPTH  16

struct PpTxFrame {
    uint8_t len;
    uint8_t data[PP_TX_PAYLOAD_MAX];
};

static QueueHandle_t sTxQueue = nullptr;

// ── Worker ──────────────────────────────────────────────────────────────────
static void pixelpostTaskFn(void*) {
    const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    PpTxFrame frame;
    uint8_t wire[4 + PP_TX_PAYLOAD_MAX];
    uint32_t lastErrMs = 0;
    for (;;) {
        if (xQueueReceive(sTxQueue, &frame, portMAX_DELAY) != pdTRUE) continue;
        if (frame.len == 0 || frame.len > PP_TX_PAYLOAD_MAX) continue;

        uint32_t ts = millis();
        memcpy(wire,     &ts,        sizeof(ts));
        memcpy(wire + 4, frame.data, frame.len);
        esp_err_t err = esp_now_send(bcast, wire, 4 + frame.len);
        if (err != ESP_OK) {
            uint32_t now = millis();
            if (now - lastErrMs > 1000) {
                lastErrMs = now;
                Serial.printf("[PP-TX] esp_now_send err=%d\n", (int)err);
            }
        }
    }
}

// ── RX hook (via MagiCommsEspNow's pre-filter spy) ──────────────────────────
// Frame: [4B timestamp][payload].  We only act on PP_MSG_SELECT_EFFECT so
// the mic stays off unless a needsMic effect is selected.  Driven by the
// pixel_post_controller's broadcasts.  Runs from the ESP-NOW recv-task
// context; spectrumSetActive is semaphore-driven so it's safe here.
static void pixelpostRecvCb(const uint8_t* data, int len) {
    if (len < 5) return;
    uint8_t type = data[4];
    if (type != PP_MSG_SELECT_EFFECT) return;
    if (len < 6) return;
    uint8_t idx = data[5];
    bool needsMic = pixelPostEffectNeedsMic(idx);
    Serial.printf("[PP-RX] SELECT_EFFECT idx=%u needsMic=%d\n",
                  (unsigned)idx, (int)needsMic);
    spectrumSetActive(needsMic);
}

// ── Init ────────────────────────────────────────────────────────────────────
// Called from magitrac_server.ino setup() after gTransportEspNow.begin().
// ESP-NOW is already initialised at that point; we just add the broadcast
// peer (if not already added by MagiCommsEspNow) and spawn the worker.
void pixelpostInit() {
    const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    if (!esp_now_is_peer_exist(bcast)) {
        // WIFI_IF_STA exists in both SERVER_AP (WIFI_AP_STA) and
        // EXTERNAL_AP (WIFI_STA) modes, and ifidx is just the source-MAC
        // selector — radio TX still uses the current channel either way,
        // which is what posts listen on.  Mirrors MagiCommsEspNow.cpp so
        // a single registered broadcast peer serves both paths.
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, bcast, 6);
        peer.channel = 0;            // 0 = current channel
        peer.ifidx   = WIFI_IF_STA;
        peer.encrypt = false;
        esp_err_t err = esp_now_add_peer(&peer);
        if (err != ESP_OK) {
            Serial.printf("[PP-TX] add broadcast peer err=%d\n", (int)err);
        }
    }

    sTxQueue = xQueueCreate(PP_TX_QUEUE_DEPTH, sizeof(PpTxFrame));
    if (!sTxQueue) {
        Serial.println("[PP-TX] xQueueCreate FAILED");
        return;
    }
    xTaskCreatePinnedToCore(pixelpostTaskFn, "pp-tx",
        /*stack=*/2048, nullptr, /*prio=*/1, nullptr, /*core=*/0);

    // Hook the per-frame spy on the ESP-NOW transport so we see
    // PP_MSG_SELECT_EFFECT from the controller and can activate the
    // mic for needsMic effects.
    gTransportEspNow.setOnBroadcastSpy(pixelpostRecvCb);

    Serial.println("[PP-TX] init OK");
}

// ── Enqueue helpers ─────────────────────────────────────────────────────────
// Drop on full queue rather than block — callers are on the mic / MIDI
// tasks and must not stall.
static void enqueue(const uint8_t* data, size_t len) {
    if (!sTxQueue) return;
    if (len == 0 || len > PP_TX_PAYLOAD_MAX) return;
    PpTxFrame frame;
    frame.len = (uint8_t)len;
    memcpy(frame.data, data, len);
    xQueueSend(sTxQueue, &frame, 0);
}

// ── Public API ──────────────────────────────────────────────────────────────
void pixelpostEnqueue(const uint8_t* payload, size_t len) {
    enqueue(payload, len);
}

void pixelpostSendPairingBeacon() {
    uint8_t buf[1] = { PP_MSG_MAGITRAC };
    enqueue(buf, sizeof(buf));
}

void pixelpostSendSelectEffect(uint8_t idx) {
    uint8_t buf[2] = { PP_MSG_SELECT_EFFECT, idx };
    enqueue(buf, sizeof(buf));
}

void pixelpostSendTapped() {
    uint8_t buf[1] = { PP_MSG_TAPPED };
    enqueue(buf, sizeof(buf));
}

void pixelpostSendSlider(uint8_t value, bool pressed) {
    uint8_t buf[3] = { PP_MSG_SLIDER, value, (uint8_t)(pressed ? 1 : 0) };
    enqueue(buf, sizeof(buf));
}

void pixelpostSendMove(uint8_t x, uint8_t y, bool pressed) {
    uint8_t buf[4] = { PP_MSG_MOVE, x, y, (uint8_t)(pressed ? 1 : 0) };
    enqueue(buf, sizeof(buf));
}
