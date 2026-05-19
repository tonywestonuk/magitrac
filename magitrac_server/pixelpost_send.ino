// pixelpost_send.ino — outgoing messages to pixel_post devices.
//
// Pixel_post expects every frame as [32B HMAC-SHA256][4B uint32 timestamp LE][payload].
// The HMAC key is the same 32 bytes that pixel_post stores in its C3 eFuse
// (HMAC_KEY4) and that the M5Paper pixel_post_controller hardcodes; we use it
// here verbatim so any paired pixel_post will accept our frames.
//
// Sends are broadcast (FF:FF:FF:FF:FF:FF), since pixel_post's model is one-to-many.
//
// HMAC compute is ~430 us on this classic ESP32, too long to run on the MIDI
// task.  All outbound pixel_post traffic is routed through a FreeRTOS queue +
// low-priority worker task pinned to core 0, so the MIDI task on core 1 is
// never blocked.  Producers (sequencer, pairing loop, UI) just enqueue.

#include <esp_now.h>
#include <esp_wifi.h>
#include <mbedtls/md.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include "mic_spectrum.h"

// Shared 32-byte HMAC key — must match pixel_post's eFuse HMAC_KEY4.
// Copied verbatim from pixel_post_controller.ino.
static const uint8_t PIXELPOST_HMAC_KEY[32] = {
    0x0a, 0x8d, 0x29, 0x33, 0xb0, 0x89, 0x36, 0x30,
    0xf4, 0xfc, 0xe9, 0x5c, 0x29, 0xed, 0xc0, 0xf9,
    0xd5, 0xd8, 0x0e, 0x6e, 0xca, 0xe0, 0x10, 0x3d,
    0xeb, 0x20, 0x6b, 0xb6, 0xa6, 0x08, 0x60, 0x1e
};

// Pixel_post message types we send.  Values match pixel_post.ino.
#define PIXELPOST_MSG_TAPPED         201
#define PIXELPOST_MSG_SELECT_EFFECT  202
#define PIXELPOST_MSG_MOVE           203
#define PIXELPOST_MSG_SLIDER         204
#define PIXELPOST_MSG_MAGITRAC       208

// ── Queue + worker ──────────────────────────────────────────────────────────

// One discriminated struct on the queue — SEND carries an outgoing payload,
// RECV carries a full received frame (HMAC+ts+payload) to be HMAC-verified
// and used for Lamport-clock sync.  Both paths run on the worker task so
// no HMAC compute touches either the WiFi task or the MIDI task.
enum PpCmdKind : uint8_t { PPCMD_SEND = 0, PPCMD_RECV = 1 };

struct PixelPostCmd {
    PpCmdKind kind;
    uint8_t   len;          // payload bytes (SEND) or frame bytes (RECV)
    uint8_t   data[64];     // SEND: <=4; RECV: 37..~50 (HMAC+ts+payload)
};

static QueueHandle_t        sPixelPostQueue = NULL;
// Persistent mbedtls HMAC context, owned by the pixelpost worker task.
// Initialised once at pixelpostInit() — avoids per-send heap churn that
// would slowly fragment the heap on busy sequences.
static mbedtls_md_context_t sHmacCtx;
static bool                 sHmacCtxReady = false;

// Logical (Lamport) clock — bumped on every send, and bumped from any
// HMAC-verified received broadcast.  Survives across send/recv, only reset
// on full reboot.  See pixel_post.ino for the matching protocol.
static volatile uint32_t    sMyClock      = 0;

static void ensureBroadcastPeer() {
    static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    if (esp_now_is_peer_exist(BCAST)) return;
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, BCAST, 6);
    p.channel = 0;        // follow current WiFi channel
    p.ifidx   = WIFI_IF_STA;
    p.encrypt = false;
    esp_err_t r = esp_now_add_peer(&p);
    if (r != ESP_OK) {
        Serial.printf("[PP] add broadcast peer err=%d\n", (int)r);
    }
}

// Build [32B HMAC][4B ts LE][payload] and broadcast it.
// Only called from the worker task — direct callers should use enqueue.
static void pixelpostSendNow(const uint8_t* payload, size_t payloadLen) {
    if (payloadLen == 0 || payloadLen > 200) return;
    if (!sHmacCtxReady) return;
    ensureBroadcastPeer();

    uint32_t ts = ++sMyClock;

    uint8_t buf[256];
    memcpy(buf + 32, &ts, 4);
    memcpy(buf + 36, payload, payloadLen);

    // Reuse the persistent context — hmac_starts() resets state; no heap
    // alloc per call.
    mbedtls_md_hmac_starts(&sHmacCtx, PIXELPOST_HMAC_KEY, sizeof(PIXELPOST_HMAC_KEY));
    mbedtls_md_hmac_update(&sHmacCtx, buf + 32, 4 + payloadLen);
    mbedtls_md_hmac_finish(&sHmacCtx, buf);   // HMAC into first 32 bytes

    static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    esp_err_t r = esp_now_send(BCAST, buf, 36 + payloadLen);
    if (r != ESP_OK) {
        Serial.printf("[PP] esp_now_send err=%d\n", (int)r);
    }
}

// Verify HMAC on an incoming broadcast and, if valid, sync sMyClock from
// the embedded timestamp.  Runs on the worker task — never on WiFi or MIDI.
static void pixelpostHandleRecv(const uint8_t* frame, size_t frameLen) {
    if (!sHmacCtxReady) return;
    if (frameLen < 37) return;   // [32 HMAC][4 ts][>=1 payload]

    uint8_t expected[32];
    mbedtls_md_hmac_starts(&sHmacCtx, PIXELPOST_HMAC_KEY, sizeof(PIXELPOST_HMAC_KEY));
    mbedtls_md_hmac_update(&sHmacCtx, frame + 32, frameLen - 32);
    mbedtls_md_hmac_finish(&sHmacCtx, expected);
    if (memcmp(expected, frame, 32) != 0) return;  // not a pixel_post frame

    // Quiet log: only print verified frames for non-routine traffic.  Skips
    // MSG_SPECTRUM (0xCF, 30 Hz from controller) and MSG_TICK_SYNC (0xC8,
    // a few Hz from each pixel_post) which would otherwise dominate.
    if (frameLen >= 37 && frame[36] != 0xCF && frame[36] != 0xC8) {
        Serial.printf("[PP] verified type=0x%02X len=%d\n",
                      frame[36], (int)frameLen);
    }

    uint32_t ts;
    memcpy(&ts, frame + 32, 4);
    if (ts > sMyClock) sMyClock = ts;   // no log — bumps every few frames

    // Mirror the magitrac-side gating: if the M5Paper controller just
    // broadcast SELECT_EFFECT, track its choice so the mic follows whoever
    // last set the effect.  Payload is at offset 36 onward.
    size_t payloadLen = frameLen - 36;
    const uint8_t* payload = frame + 36;
    if (payloadLen >= 2 && payload[0] == PIXELPOST_MSG_SELECT_EFFECT) {
        spectrumSetActive(payload[1] == 13);
    }
}

static void pixelpostWorker(void*) {
    PixelPostCmd cmd;
    while (true) {
        if (xQueueReceive(sPixelPostQueue, &cmd, portMAX_DELAY) == pdTRUE) {
            if (cmd.kind == PPCMD_SEND)      pixelpostSendNow(cmd.data, cmd.len);
            else if (cmd.kind == PPCMD_RECV) pixelpostHandleRecv(cmd.data, cmd.len);
        }
    }
}

// Spy hook — fires on the WiFi task for every raw broadcast.  Keep this
// tiny: copy and queue.  HMAC verification happens on the worker.
static void pixelpostBroadcastSpy(const uint8_t* data, int len) {
    if (!sPixelPostQueue) return;
    if (len < 37 || len > (int)sizeof(((PixelPostCmd*)0)->data)) return;
    PixelPostCmd cmd;
    cmd.kind = PPCMD_RECV;
    cmd.len  = (uint8_t)len;
    memcpy(cmd.data, data, len);
    xQueueSend(sPixelPostQueue, &cmd, 0);   // drop if full
}

void pixelpostInit() {
    if (sPixelPostQueue) return;

    // One-time HMAC context setup — internal allocations happen here, not
    // per-send, so the worker doesn't churn the heap.
    mbedtls_md_init(&sHmacCtx);
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (mbedtls_md_setup(&sHmacCtx, info, /*hmac=*/1) == 0) {
        sHmacCtxReady = true;
    } else {
        Serial.println("[PP] mbedtls_md_setup FAILED — pixel_post disabled");
    }

    sPixelPostQueue = xQueueCreate(32, sizeof(PixelPostCmd));
    // Core 0 = WiFi/system core (MIDI sequencer runs on core 1 via midiTaskFn,
    // so this worker never preempts MIDI).  Low priority (1) — sequencer and
    // WiFi outrank it.
    xTaskCreatePinnedToCore(pixelpostWorker, "ppw", 4096, NULL, 1, NULL, 0);

    // Register the broadcast spy so we sync sMyClock from pixel_post's
    // HMAC'd MSG_TICK_SYNC broadcasts.  Without this, every magitrac
    // reboot would drop the first N sends until our counter caught up
    // to whatever pixel_post had cached for our MAC.
    extern MagiCommsEspNow gTransport;
    gTransport.setOnBroadcastSpy(pixelpostBroadcastSpy);
}

// Non-blocking enqueue.  Drops silently if the queue is full (best-effort).
void pixelpostEnqueue(const uint8_t* payload, size_t len) {
    if (!sPixelPostQueue) return;
    if (len == 0 || len > sizeof(((PixelPostCmd*)0)->data)) return;
    PixelPostCmd cmd;
    cmd.kind = PPCMD_SEND;
    cmd.len  = (uint8_t)len;
    memcpy(cmd.data, payload, len);
    xQueueSend(sPixelPostQueue, &cmd, 0);
}

// ── Public message helpers (all go through the queue) ───────────────────────

void pixelpostSendPairingBeacon() {
    uint8_t msg = PIXELPOST_MSG_MAGITRAC;
    pixelpostEnqueue(&msg, 1);
}

void pixelpostSendSelectEffect(uint8_t idx) {
    uint8_t msg[2] = { PIXELPOST_MSG_SELECT_EFFECT, idx };
    pixelpostEnqueue(msg, sizeof(msg));
    // Effect 13 = SoundSpectrum.  Magitrac just told the posts to switch —
    // gate the mic to match.  Any other effect turns the mic off.
    spectrumSetActive(idx == 13);
}

void pixelpostSendTapped() {
    uint8_t msg = PIXELPOST_MSG_TAPPED;
    pixelpostEnqueue(&msg, 1);
}

void pixelpostSendSlider(uint8_t value, bool pressed) {
    uint8_t msg[3] = { PIXELPOST_MSG_SLIDER, value, (uint8_t)(pressed ? 1 : 0) };
    pixelpostEnqueue(msg, sizeof(msg));
}

void pixelpostSendMove(uint8_t x, uint8_t y, bool pressed) {
    uint8_t msg[4] = { PIXELPOST_MSG_MOVE, x, y, (uint8_t)(pressed ? 1 : 0) };
    pixelpostEnqueue(msg, sizeof(msg));
}
