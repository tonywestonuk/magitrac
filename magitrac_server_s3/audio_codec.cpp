#include "audio_codec.h"
#include <Arduino.h>
#include <Wire.h>
#include <M5Module_Audio.h>
#include "driver/i2s_std.h"

// Shared ES8388 codec on the M5Stack Module Audio v2.2 stacked on CoreS3.
// SamplePlayer (TX) and mic_spectrum (RX) both share it full-duplex.
//
// Input mapping (ADC input pair 1, captured via ADC_INPUT_LINPUT1_RINPUT1):
// There is NO onboard MEMS mic — the only audio input is the module's stereo
// mic input (electret-capable, with the switchable bias described below).
// The new mic (2026-06-25) drives the LEFT channel (LINPUT1), so every consumer
// (chord recogniser, pixelpost bands, scope) reads the LEFT channel — it's the
// single source for all mic-reactive features.  RIGHT (RINPUT1) is captured but
// unused.  (History: an earlier capsule used RIGHT only, settled 2026-06-23.)
//
// The MIC/LINE bias is a switchable pull-up: the module STM32's pin PB7 (net
// LIN_MIC_PU_EN) drives Q2(NMOS)→Q1(PMOS) to connect AUDIO_VDD through R16
// (3.3k) onto the input, biasing the electret capsule.  We reach PB7 over I²C
// via the STM32's register 0x00 (setMICStatus):
//     AUDIO_MIC_OPEN  → PB7 high → bias pull-up ON  → MIC   (what we want)
//     AUDIO_MIC_CLOSE → PB7 low  → bias pull-up OFF → LINE
// The M5 library's "open/close" naming is a simplification; functionally it is
// mic-vs-line.  Side effect: the bias puts a DC pedestal on the raw samples
// (the silkscreen "OFFSET") — mic_spectrum AC-couples it out for the scope/FFT.
//
// Line-out to the live amp goes via the same ES8388 DAC, and the internal
// CoreS3 ES7210 path is blocked while Module Audio is stacked (G13/G14
// contention — see project memory).

static M5ModuleAudio sDev;
static bool          sReady = false;

// Low-latency I2S.  M5's library defaults to a 6×240-frame DMA pool (~45 ms at
// 32 kHz) which dominates the note→sound latency.  We let M5 do the ES8388 /
// STM32 register setup over I2C (its codec-only begin() overload), but drive
// the audio data path ourselves through a small DMA pool for ~8 ms latency.
// TX and RX are independent handles on the same controller (full-duplex), so
// SamplePlayer (TX), the organ synth (TX) and mic_spectrum (RX) keep working
// exactly as before — just with a shorter pipeline.
static i2s_chan_handle_t sTx = nullptr;
static i2s_chan_handle_t sRx = nullptr;

// Apply the full ES8388 + STM32 input/output config.  Idempotent — safe to
// re-run, which is exactly what the boot-time verify loop below relies on.
static void configureCodec() {
    sDev.setHPMode(AUDIO_HPMODE_NATIONAL);             // National TRRS — quietest coupling (verified on rig)
    sDev.setMICStatus(AUDIO_MIC_OPEN);                 // PB7 high → mic bias on → MIC mode
    sDev.setMicInputLine(ADC_INPUT_LINPUT1_RINPUT1);   // mic + line-in share this pair
    sDev.setMicGain(MIC_GAIN_15DB);              // 15 dB — 24 dB overdrove the input into clipping/distortion
    sDev.setMicAdcVolume(100);
    sDev.setSpeakerVolume(100);
    sDev.setSpeakerOutput(DAC_OUTPUT_ALL);       // OUT1 + OUT2 — speaker + line out (live amp needs OUT2)
    sDev.setBitsSample(ES_MODULE_ADC_DAC, BIT_LENGTH_16BITS);
    sDev.setSampleRate(SAMPLE_RATE_32K);
    sDev.setMICStatus(AUDIO_MIC_OPEN);            // belt-and-braces re-assert
}

// DMA pool sizes (frames).  LARGE rides out SD read jitter for SamplePlayer;
// SMALL is the low-latency pool for the live organ.  We switch between them at
// runtime (organ active vs not) — the two are never streaming at once.
#define DMA_LARGE_DESC 6
#define DMA_LARGE_LEN  240               // 6×240 = 1440 frames ≈ 45 ms
#define DMA_SMALL_DESC 4
#define DMA_SMALL_LEN  64                // 4×64  = 256  frames ≈ 8 ms

static volatile bool s_reconfiguring = false;
static bool          s_lowLatency    = false;

// Bring up our own full-duplex I2S channel pair (auto-assigned controller).
// Pins come from M5's M-Bus map (same ones M5's begin() uses).
static bool i2sInitDma(int descNum, int frameNum) {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    uint8_t bck = M5.getPin(m5::pin_name_t::mbus_pin24);   // SCLK
    uint8_t mck = M5.getPin(m5::pin_name_t::mbus_pin22);   // MCLK
#else
    uint8_t bck = M5.getPin(m5::pin_name_t::mbus_pin22);
    uint8_t mck = M5.getPin(m5::pin_name_t::mbus_pin24);
#endif
    uint8_t di   = M5.getPin(m5::pin_name_t::mbus_pin26);  // DIN  (mic)
    uint8_t ws   = M5.getPin(m5::pin_name_t::mbus_pin21);  // LRCK
    uint8_t dout = M5.getPin(m5::pin_name_t::mbus_pin23);  // DOUT (speaker)

    // I2S_NUM_AUTO: let the driver pick a free controller (M5 may hold NUM_0).
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = descNum;
    chan_cfg.dma_frame_num = frameNum;
    chan_cfg.auto_clear    = true;   // output silence (not stale) on a TX underrun
    if (i2s_new_channel(&chan_cfg, &sTx, &sRx) != ESP_OK) return false;

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_CODEC_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)mck,
            .bclk = (gpio_num_t)bck,
            .ws   = (gpio_num_t)ws,
            .dout = (gpio_num_t)dout,
            .din  = (gpio_num_t)di,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    // Full-duplex: both directions share BCLK/WS/MCLK, so init both the same way.
    if (i2s_channel_init_std_mode(sTx, &std_cfg) != ESP_OK) return false;
    if (i2s_channel_init_std_mode(sRx, &std_cfg) != ESP_OK) return false;
    if (i2s_channel_enable(sTx) != ESP_OK) return false;
    if (i2s_channel_enable(sRx) != ESP_OK) return false;
    Serial.printf("[CODEC] I2S up (%dx%d = %d frames)\n", descNum, frameNum, descNum * frameNum);
    return true;
}

static bool i2sInit() {
    s_lowLatency = false;
    return i2sInitDma(DMA_LARGE_DESC, DMA_LARGE_LEN);   // boot with the SD-robust pool
}

// Swap the DMA pool size at runtime (organ on → small, off → large).  Guarded so
// the audio tasks aren't writing while we tear the channel down: s_reconfiguring
// makes play()/record() no-op, then we wait out any in-flight (blocking) write.
void audioCodecSetLowLatency(bool low) {
    if (!sReady || low == s_lowLatency) return;
    s_reconfiguring = true;
    vTaskDelay(pdMS_TO_TICKS(60));      // > worst-case in-flight write (large pool ~45 ms)
    if (sTx) { i2s_channel_disable(sTx); }
    if (sRx) { i2s_channel_disable(sRx); }
    if (sTx) { i2s_del_channel(sTx); sTx = nullptr; }
    if (sRx) { i2s_del_channel(sRx); sRx = nullptr; }
    s_lowLatency = low;
    if (low) i2sInitDma(DMA_SMALL_DESC, DMA_SMALL_LEN);
    else     i2sInitDma(DMA_LARGE_DESC, DMA_LARGE_LEN);
    s_lowLatency = low;
    s_reconfiguring = false;
}

bool audioCodecInit() {
    if (sReady) return true;

    // Bring up our low-latency I2S master FIRST, so MCLK/BCLK/WS are already
    // clocking the bus when we configure the ES8388 (matches M5's begin()
    // ordering — clocks, then codec).  Fatal: no audio without it.
    if (!i2sInit()) {
        Serial.println("[CODEC] custom I2S init FAILED — no audio");
        return false;
    }

    // Wire1 (I²C_NUM_1) — same internal bus M5GFX uses for touch / AXP /
    // AW9523.  M5.begin() must have run first.  Retry: the module's STM32 may
    // not have finished booting yet, and the bus is shared/contended.
    // Codec-only begin (the 5-arg overload): does the ES8388/STM32 register
    // setup over I2C but does NOT bring up M5's I2S driver — we own that.
    uint8_t sda = M5.getPin(m5::pin_name_t::in_i2c_sda);
    uint8_t scl = M5.getPin(m5::pin_name_t::in_i2c_scl);
    bool begun = false;
    for (int attempt = 0; attempt < 5; attempt++) {
        if (sDev.begin(Wire1, sda, scl)) { begun = true; break; }
        Serial.printf("[CODEC] begin attempt %d failed — retrying\n", attempt + 1);
        delay(100);
    }
    if (!begun) {
        Serial.println("[CODEC] M5ModuleAudio.begin FAILED — module not detected");
        return false;
    }

    // Configure, then VERIFY by read-back.  setMICStatus() is a fire-and-forget
    // I²C write with no retry in the library; right after power-on the STM32
    // sometimes doesn't latch the mic-bias enable, which boots the capsule
    // un-biased → flat scope until a power cycle.  getMICStatus() round-trips
    // the same register over I²C, so we re-apply the (idempotent) config and
    // re-read until the bias write is confirmed — a proper read-after-write,
    // not a guessed delay.
    bool biasOk = false;
    for (int attempt = 0; attempt < 6 && !biasOk; attempt++) {
        delay(40);                   // let the STM32 settle between tries
        configureCodec();
        biasOk = (sDev.getMICStatus() == AUDIO_MIC_OPEN);
        if (!biasOk)
            Serial.printf("[CODEC] mic-bias read-back mismatch (attempt %d) — re-applying\n",
                          attempt + 1);
    }

    Serial.printf("[CODEC] ES8388 ready @ 32 kHz / 16-bit stereo; mic-bias verified=%d\n",
                  biasOk ? 1 : 0);

    sReady = true;       // bias verify is non-fatal; the I2S init above is the hard gate
    return true;
}

bool audioCodecReady() { return sReady; }

void audioCodecSetMicBias(bool micMode) {
    if (!sReady) return;
    sDev.setMICStatus(micMode ? AUDIO_MIC_OPEN : AUDIO_MIC_CLOSE);
}

void audioCodecSetHPMode(bool american) {
    if (!sReady) return;
    sDev.setHPMode(american ? AUDIO_HPMODE_AMERICAN : AUDIO_HPMODE_NATIONAL);
}

bool audioCodecPlay(const uint8_t* buf, size_t bytes) {
    if (!sReady || !sTx || s_reconfiguring) return false;
    size_t written = 0;
    return i2s_channel_write(sTx, buf, bytes, &written, portMAX_DELAY) == ESP_OK;
}

bool audioCodecRecord(uint8_t* buf, size_t bytes) {
    if (!sReady || !sRx || s_reconfiguring) return false;
    size_t got = 0;
    return i2s_channel_read(sRx, buf, bytes, &got, portMAX_DELAY) == ESP_OK;
}
