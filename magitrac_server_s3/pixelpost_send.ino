// pixelpost_send.ino — PPOST state broadcaster (see pixelpost_proto.h).
//
// One packet shape, broadcast on every state change AND every 2 seconds as
// a heartbeat.  No ACKs; posts never reply.  Replaces the per-event scheme
// (SELECT_EFFECT / MOVE / SLIDER / TAPPED / SPECTRUM / BEAT / DISCOVER /
// HERE_I_AM / SET_ORDINAL / IDENTIFY / FIRMWARE_UPDATE) that existed up to
// 2026-06-08.

#include <Arduino.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <pixelpost_proto.h>
#include <string.h>
#include "mic_spectrum.h"        // spectrumSetActive
#include "MagiCommsEspNow.h"     // gTransportEspNow

extern MagiCommsEspNow gTransportEspNow;

// OTA creds + URL are no longer hardcoded here — the in-the-field flow serves
// firmware off the server's OWN softAP and passes ssid/psk/url into
// pixelpostSendFirmwareUpdate() (see field_flash.ino).

// ── State cache ─────────────────────────────────────────────────────────────
// Mutated from sequencer + UI tasks; snapshotted under a spinlock at send time.

static portMUX_TYPE  sLock = portMUX_INITIALIZER_UNLOCKED;
static PpStatePacket sState = {
    { 'P', 'P', 'O', 'S', 'T' },
    /*channelId=*/0,
    /*tick=*/0,
    /*effectId=*/0,
    /*touchX=*/0,
    /*touchY=*/0,
    /*slider=*/255,    // default full brightness — matches the posts' control_data
                       // default; 0 here would make untouched/low look "off"
    /*flags=*/0,
    /*postCount=*/5,    // sensible default until the client sets one
    /*flashCtrl=*/0,    // 0 = LayerFlashSoftener disabled on the post side
};

// Tracks the last time a PPOST went out (heartbeat OR on-change).  The
// heartbeat task uses it to wait the full PP_HEARTBEAT_MS gap from the
// most recent send rather than firing on a fixed wall-clock cadence —
// so a busy stretch of setter calls doesn't get layered with extra
// heartbeats right behind, and the airwaves quiet down to 1 packet per
// 2 s as soon as the activity stops.
static volatile uint32_t sLastSendMs = 0;

// POWER_OFF auto-clear deadline.  Posts that hear POWER_OFF=1 deep-sleep
// until they're power-cycled; if the server kept the flag set forever, a
// freshly-rebooted post would immediately sleep again on the next heartbeat
// and could never be brought back.  After 30 s we clear the bit, so future
// PPOSTs carry POWER_OFF=0 and a power-cycled post stays awake.
// 0 = no clear pending.
static volatile uint32_t sPowerOffClearAtMs = 0;
static const uint32_t    POWER_OFF_HOLD_MS  = 30000;

// ── Send ────────────────────────────────────────────────────────────────────
static void pixelpostSendStateNow() {
    PpStatePacket pkt;
    taskENTER_CRITICAL(&sLock);
    pkt = sState;
    taskEXIT_CRITICAL(&sLock);
    pkt.tick = millis();   // stamped at TX time, not cache-update time

    const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    esp_err_t err = esp_now_send(bcast, (const uint8_t*)&pkt, sizeof(pkt));
    if (err != ESP_OK) {
        static uint32_t lastErrMs = 0;
        uint32_t now = millis();
        if (now - lastErrMs > 1000) {
            lastErrMs = now;
            Serial.printf("[PP-TX] esp_now_send err=%d\n", (int)err);
        }
    }
    sLastSendMs = pkt.tick;
}

// ── Heartbeat ───────────────────────────────────────────────────────────────
// Fires whenever PP_HEARTBEAT_MS has elapsed since the most recent send.
// Any setter call resets that clock — so heavy live-control bursts don't
// stack heartbeat packets on top of the on-change traffic.  Also services
// the POWER_OFF auto-clear timer (checked on every wakeup → at most 2 s
// lag past the 30 s deadline).
static void pixelpostTaskFn(void*) {
    for (;;) {
        uint32_t now = millis();
        if (sPowerOffClearAtMs != 0 && (int32_t)(now - sPowerOffClearAtMs) >= 0) {
            taskENTER_CRITICAL(&sLock);
            sState.flags &= ~PP_FLAG_POWER_OFF;
            taskEXIT_CRITICAL(&sLock);
            sPowerOffClearAtMs = 0;
            pixelpostSendStateNow();
            vTaskDelay(pdMS_TO_TICKS(PP_HEARTBEAT_MS));
            continue;
        }

        uint32_t since = now - sLastSendMs;
        if (since >= PP_HEARTBEAT_MS) {
            pixelpostSendStateNow();
            vTaskDelay(pdMS_TO_TICKS(PP_HEARTBEAT_MS));
        } else {
            vTaskDelay(pdMS_TO_TICKS(PP_HEARTBEAT_MS - since));
        }
    }
}

// ── RX hook ─────────────────────────────────────────────────────────────────
// We listen to our own broadcasts (loopback) AND to the controller's so the
// mic is activated whenever the live effect needs it, regardless of source.
static void pixelpostRecvCb(const uint8_t* data, int len) {
    if (len < (int)sizeof(PpStatePacket)) return;
    if (memcmp(data, PP_MAGIC, PP_MAGIC_LEN) != 0) return;
    const PpStatePacket* pkt = (const PpStatePacket*)data;
    spectrumSetActive(pixelPostEffectNeedsMic(pkt->effectId));
}

// ── Init ────────────────────────────────────────────────────────────────────
void pixelpostInit() {
    const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    if (!esp_now_is_peer_exist(bcast)) {
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

    xTaskCreatePinnedToCore(pixelpostTaskFn, "pp-tx",
        /*stack=*/2048, nullptr, /*prio=*/1, nullptr, /*core=*/0);

    gTransportEspNow.setOnBroadcastSpy(pixelpostRecvCb);

    Serial.println("[PP-TX] init OK");
}

// ── Public setters ──────────────────────────────────────────────────────────
// Each setter updates the cache and triggers an immediate send.  Sends are
// cheap (16 bytes, non-blocking ESP-NOW); if a send drops on the wire, the
// heartbeat covers it within 2 s.

void pixelpostSetEffect(uint8_t idx) {
    bool changed;
    taskENTER_CRITICAL(&sLock);
    changed = (sState.effectId != idx);
    sState.effectId = idx;
    taskEXIT_CRITICAL(&sLock);
    if (changed) {
        spectrumSetActive(pixelPostEffectNeedsMic(idx));
        pixelpostSendStateNow();
    }
}

void pixelpostSetTouchpad(uint8_t x, uint8_t y, bool touched) {
    taskENTER_CRITICAL(&sLock);
    sState.touchX = x;
    sState.touchY = y;
    if (touched) sState.flags |=  PP_FLAG_TOUCHED;
    else         sState.flags &= ~PP_FLAG_TOUCHED;
    taskEXIT_CRITICAL(&sLock);
    pixelpostSendStateNow();
}

void pixelpostSetSlider(uint8_t value) {
    bool changed;
    taskENTER_CRITICAL(&sLock);
    changed = (sState.slider != value);
    sState.slider = value;
    taskEXIT_CRITICAL(&sLock);
    if (changed) pixelpostSendStateNow();
}

// ── Audio (spectrum + beat) broadcast — PPOSB ─────────────────────────────
// Called from mic_spectrum.cpp at the end of each FFT cycle.  No heartbeat,
// no on-quiet refresh — when the mic is off the airwaves are silent.  Posts
// drop these unless they're running a mic-reactive effect.
//
// `bands` MUST point to PP_AUDIO_BANDS bytes — declared as a plain pointer
// (not `bands[PP_AUDIO_BANDS]`) so Arduino-CLI's auto-prototype pass for
// .ino files doesn't trip on PP_AUDIO_BANDS before <pixelpost_proto.h> is
// included.
void pixelpostSendAudio(const uint8_t* bands,
                        uint32_t beatSeq, uint8_t beatStrength) {
    PpAudioPacket pkt;
    memcpy(pkt.magic, PP_AUDIO_MAGIC, sizeof(pkt.magic));
    taskENTER_CRITICAL(&sLock);
    pkt.channelId = sState.channelId;
    taskEXIT_CRITICAL(&sLock);
    pkt.tick         = millis();
    memcpy(pkt.bands, bands, PP_AUDIO_BANDS);
    pkt.beatSeq      = beatSeq;
    pkt.beatStrength = beatStrength;

    const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    esp_now_send(bcast, (const uint8_t*)&pkt, sizeof(pkt));
}

// ── Firmware update broadcast ──────────────────────────────────────────────
// Builds a PPOSF packet from caller-supplied creds + URL (the server now
// serves the firmware off its OWN softAP — see field_flash.ino) and the
// current channelId, then sends it 3 times with 200 ms gaps to ride out
// broadcast loss.  Runs on a one-shot task so the trigger returns immediately.
static char sFwSsid[PP_OTA_SSID_LEN + 1];
static char sFwPwd [PP_OTA_PASSWORD_LEN + 1];
static char sFwUrl [PP_OTA_URL_LEN + 1];

static void pixelpostFwTaskFn(void*) {
    PpFirmwarePacket pkt = {};
    memcpy(pkt.magic, PP_FW_MAGIC, sizeof(pkt.magic));
    taskENTER_CRITICAL(&sLock);
    pkt.channelId = sState.channelId;
    taskEXIT_CRITICAL(&sLock);
    strncpy(pkt.ssid,     sFwSsid, sizeof(pkt.ssid));
    strncpy(pkt.password, sFwPwd,  sizeof(pkt.password));
    strncpy(pkt.url,      sFwUrl,  sizeof(pkt.url));

    const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    for (int i = 0; i < 3; i++) {
        esp_err_t err = esp_now_send(bcast, (const uint8_t*)&pkt, sizeof(pkt));
        Serial.printf("[PP-TX] PPOSF send %d err=%d\n", i, (int)err);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    vTaskDelete(nullptr);
}

void pixelpostSendFirmwareUpdate(const char* ssid, const char* pwd, const char* url) {
    strncpy(sFwSsid, ssid ? ssid : "", sizeof(sFwSsid)); sFwSsid[sizeof(sFwSsid) - 1] = 0;
    strncpy(sFwPwd,  pwd  ? pwd  : "", sizeof(sFwPwd));  sFwPwd [sizeof(sFwPwd)  - 1] = 0;
    strncpy(sFwUrl,  url  ? url  : "", sizeof(sFwUrl));  sFwUrl [sizeof(sFwUrl)  - 1] = 0;
    Serial.printf("[PP-TX] firmware update: ssid='%s' url='%s'\n", sFwSsid, sFwUrl);
    xTaskCreate(pixelpostFwTaskFn, "pp-fw", 4096, nullptr, 1, nullptr);
}

void pixelpostSetPostCount(uint8_t count) {
    bool changed;
    taskENTER_CRITICAL(&sLock);
    changed = (sState.postCount != count);
    sState.postCount = count;
    taskEXIT_CRITICAL(&sLock);
    if (changed) pixelpostSendStateNow();
}

void pixelpostSetFlashCtrl(uint8_t value) {
    bool changed;
    taskENTER_CRITICAL(&sLock);
    changed = (sState.flashCtrl != value);
    sState.flashCtrl = value;
    taskEXIT_CRITICAL(&sLock);
    if (changed) pixelpostSendStateNow();
}

// ── Live manual override (performance-page light strip) ─────────────────────
// While active, the PXL POST track's effect/slider/touchpad writes are ignored
// (gated in midi_player.cpp via pixelpostManualActive()), so the performer's
// chosen effect holds.  RELEASE hands control back to the track.
static volatile bool sManual = false;

bool pixelpostManualActive() { return sManual; }
void pixelpostManualRelease() { sManual = false; }

// NEXT / PREV — grab control and step the effect, wrapping over the catalogue.
// Restores full brightness in case a prior POW blacked it out.
void pixelpostManualCycle(int8_t delta) {
    sManual = true;
    uint8_t cur;
    taskENTER_CRITICAL(&sLock);
    cur = sState.effectId;
    taskEXIT_CRITICAL(&sLock);
    int n = (int)PIXELPOST_EFFECT_COUNT;
    if (n < 1) n = 1;
    int nx = ((int)cur + (int)delta) % n;
    if (nx < 0) nx += n;
    pixelpostSetSlider(255);            // full brightness for the new effect
    pixelpostSetEffect((uint8_t)nx);
}

// WHITE — momentary full-white.  Press grabs control, selects the "WHITE"
// effect (looked up by name so a catalogue reorder can't mis-point us) and sends
// a touchpad-press; the post-side effect ramps to 100% white while pressed.
// Release sends the touchpad-up so the effect fades back to black.  The ramp/fade
// timing lives entirely in the post effect (la_whitefill).
void pixelpostManualWhite(bool on) {
    sManual = true;
    if (on) {
        uint8_t idx = (uint8_t)(PIXELPOST_EFFECT_COUNT - 1);   // fallback = last effect
        for (size_t i = 0; i < PIXELPOST_EFFECT_COUNT; i++) {
            if (strcmp(PIXELPOST_EFFECTS[i].name, "WHITE") == 0) {
                idx = PIXELPOST_EFFECTS[i].index;
                break;
            }
        }
        pixelpostSetEffect(idx);
        pixelpostSetTouchpad(128, 128, /*touched=*/true);    // → ramp up to white
    } else {
        pixelpostSetTouchpad(128, 128, /*touched=*/false);   // → fade down to black
    }
}

void pixelpostSetPowerOff(bool off) {
    taskENTER_CRITICAL(&sLock);
    if (off) sState.flags |=  PP_FLAG_POWER_OFF;
    else     sState.flags &= ~PP_FLAG_POWER_OFF;
    taskEXIT_CRITICAL(&sLock);
    // Schedule the auto-clear — repeated setOff(true) calls keep extending
    // the deadline.  setOff(false) cancels it.
    sPowerOffClearAtMs = off ? (millis() + POWER_OFF_HOLD_MS) : 0;
    pixelpostSendStateNow();
}
