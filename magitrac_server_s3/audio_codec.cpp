#include "audio_codec.h"
#include <Arduino.h>
#include <Wire.h>
#include <M5Module_Audio.h>

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

bool audioCodecInit() {
    if (sReady) return true;

    // Wire1 (I²C_NUM_1) — same internal bus M5GFX uses for touch / AXP /
    // AW9523.  M5.begin() must have run first.  Retry: the module's STM32 may
    // not have finished booting yet, and the bus is shared/contended.
    bool begun = false;
    for (int attempt = 0; attempt < 5; attempt++) {
        if (sDev.begin(Wire1)) { begun = true; break; }
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

    sReady = true;       // mark ready regardless so playback still works
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
    if (!sReady) return false;
    return sDev.play(buf, (int)bytes);
}

bool audioCodecRecord(uint8_t* buf, size_t bytes) {
    if (!sReady) return false;
    return sDev.record(buf, (int)bytes);
}
