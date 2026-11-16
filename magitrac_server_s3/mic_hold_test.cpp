#include "mic_hold_test.h"
#include <SD.h>            // must precede M5Unified (M5GFX rebinds File I/O otherwise)
#include <M5Unified.h>
#include "audio_codec.h"
#include <esp_heap_caps.h>
#include <math.h>

// Longest take.  LEFT channel only, 16-bit at 32 kHz = 64 KB per second.
static const uint32_t MH_MAX_SECONDS = 30;
static const size_t   MH_MAX_FRAMES  = (size_t)AUDIO_CODEC_RATE_HZ * MH_MAX_SECONDS;   // 1.92 MB
static const int      MH_BLOCK       = 256;     // frames per codec read / write (8 ms)
static const int      MH_TOUCH_EVERY = 4;       // poll the touch panel every 4 blocks (32 ms)
static const size_t   MH_MIN_FRAMES  = AUDIO_CODEC_RATE_HZ / 5;   // ignore takes under 0.2 s

static const uint16_t MH_BG   = 0x0011;   // dark blue, as the server's screens
static const uint16_t MH_REC  = 0xC000;   // red
static const uint16_t MH_PLAY = 0x0400;   // green

struct MhStats {
    size_t  frames  = 0;
    int32_t peak    = 0;      // largest |sample|
    double  sumSq   = 0;      // for RMS
    int64_t sum     = 0;      // for the DC offset
    uint32_t clipped = 0;     // samples at (or within 8 of) full scale
    double  sumSqR  = 0;      // right channel (not wired to the mic) — noise reference
};

static float dbfs(double v) {            // v = amplitude, full scale = 32768
    return (v < 1.0) ? -96.0f : (float)(20.0 * log10(v / 32768.0));
}

static bool touched() {
    M5.update();
    return M5.Touch.getDetail().isPressed();
}

static void banner(uint16_t bg, const char* line1, const char* line2) {
    auto& lcd = M5.Display;
    lcd.fillScreen(bg);
    lcd.setTextColor(TFT_WHITE, bg);
    lcd.setTextSize(3);
    lcd.setCursor(12, 24);  lcd.print("MIC TEST");
    lcd.setTextSize(2);
    lcd.setCursor(12, 80);  lcd.print(line1);
    lcd.setCursor(12, 106); lcd.print(line2);
}

static void showStats(const MhStats& st) {
    auto& lcd = M5.Display;
    double rms  = st.frames ? sqrt(st.sumSq  / (double)st.frames) : 0;
    double rmsR = st.frames ? sqrt(st.sumSqR / (double)st.frames) : 0;
    long   dc   = st.frames ? (long)(st.sum / (int64_t)st.frames) : 0;
    float  secs = (float)st.frames / (float)AUDIO_CODEC_RATE_HZ;
    float  pk   = dbfs((double)st.peak);

    lcd.setTextColor(TFT_WHITE);
    lcd.setTextSize(2);
    int y = 150;
    lcd.setCursor(12, y); lcd.printf("Length %.1f s", secs);                       y += 26;
    lcd.setCursor(12, y); lcd.printf("Peak %3d%% %.0fdB",
                                     (int)(st.peak * 100L / 32768L), pk);          y += 26;
    lcd.setCursor(12, y); lcd.printf("RMS  %.0f dB", dbfs(rms));                   y += 26;
    lcd.setCursor(12, y); lcd.printf("DC   %+ld", dc);                             y += 26;
    lcd.setCursor(12, y); lcd.printf("Clipped %lu", (unsigned long)st.clipped);

    Serial.printf("[MICTEST] %.2f s  L: peak %ld (%.1f%% = %.1f dBFS)  rms %.0f (%.1f dBFS)  "
                  "dc %+ld  clipped %lu  |  R (not the mic): rms %.0f (%.1f dBFS)\n",
                  secs, (long)st.peak, (float)st.peak * 100.0f / 32768.0f, pk,
                  rms, dbfs(rms), dc, (unsigned long)st.clipped, rmsR, dbfs(rmsR));
}

void micHoldLoop() {
    Serial.println("[MICTEST] hold the screen to record, let go to play");

    // Both buffers in PSRAM — internal RAM is the radios', and they are on.
    int16_t* rec = (int16_t*)heap_caps_malloc(MH_MAX_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int16_t* io  = (int16_t*)heap_caps_malloc(MH_BLOCK * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!audioCodecReady() || !rec || !io) {
        banner(MH_REC, audioCodecReady() ? "No PSRAM for" : "Audio module",
                       audioCodecReady() ? "the recording" : "not found");
        Serial.println("[MICTEST] cannot run (codec or PSRAM missing) — halting");
        while (true) delay(1000);
    }

    banner(MH_BG, "Hold the screen", "to record");
    M5.Display.setTextSize(1);
    M5.Display.setCursor(12, 300);
    M5.Display.print("Flash the server firmware to exit");

    for (;;) {
        if (!touched()) { delay(5); continue; }

        // ── Record while the finger is down ─────────────────────────────────
        // Nothing is drawn while recording: LCD traffic is one less thing
        // that could find its way into the take.
        banner(MH_REC, "RECORDING", "let go to play");
        MhStats st;
        bool down = true;
        int  blk  = 0;
        while (down && st.frames + MH_BLOCK <= MH_MAX_FRAMES) {
            if (!audioCodecRecord((uint8_t*)io, MH_BLOCK * 4)) { delay(2); continue; }
            int16_t* dst = rec + st.frames;
            for (int i = 0; i < MH_BLOCK; i++) {
                int32_t l = io[i * 2], r = io[i * 2 + 1];
                dst[i] = (int16_t)l;
                int32_t a = l < 0 ? -l : l;
                if (a > st.peak) st.peak = a;
                if (a >= 32760) st.clipped++;
                st.sum    += l;
                st.sumSq  += (double)l * (double)l;
                st.sumSqR += (double)r * (double)r;
            }
            st.frames += MH_BLOCK;
            if (++blk % MH_TOUCH_EVERY == 0) down = touched();
        }
        if (st.frames < MH_MIN_FRAMES) {             // a tap, not a take
            banner(MH_BG, "Hold the screen", "to record");
            while (touched()) delay(5);
            continue;
        }

        // ── Play it back (LEFT on both outputs) ─────────────────────────────
        banner(MH_PLAY, "PLAYING", "hold to record again");
        showStats(st);
        while (touched()) delay(5);                  // (only when the 30 s limit ended the take)
        bool again = false;
        blk = 0;
        for (size_t pos = 0; pos < st.frames && !again; pos += MH_BLOCK) {
            for (int i = 0; i < MH_BLOCK; i++) {
                int16_t s = rec[pos + i];
                io[i * 2] = s; io[i * 2 + 1] = s;
            }
            if (!audioCodecPlay((const uint8_t*)io, MH_BLOCK * 4)) delay(2);
            if (++blk % MH_TOUCH_EVERY == 0) again = touched();   // a new hold cuts playback
        }
        if (!again) {
            // Leave the figures up; a quarter-second of silence flushes the
            // tail of the take out of the output buffer.
            memset(io, 0, MH_BLOCK * 2 * sizeof(int16_t));
            for (int i = 0; i < 32; i++) audioCodecPlay((const uint8_t*)io, MH_BLOCK * 4);
            banner(MH_BG, "Hold the screen", "to record");
            showStats(st);
        }
        // `again`: loop round — the finger is down, so recording starts at once.
    }
}
