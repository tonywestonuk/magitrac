#pragma once
#include <stdint.h>
#include <stddef.h>

// Shared ES8388 codec on the M5Stack Module Audio v2.2.  Init once at
// boot (after M5.begin() + Wire is up), then SamplePlayer (TX) and
// mic_spectrum (RX) both share the codec full-duplex.  No I²S0 mutex
// needed any more — ES8388 has independent TX/RX paths on the same I²S
// peripheral, and the IDF driver runs separate DMA channels for each
// direction.

bool audioCodecInit();           // returns true on success
bool audioCodecReady();          // safe to call from anywhere

// Switch the module's input bias (STM32 PB7 / LIN_MIC_PU_EN, via reg 0x00):
// true  → bias pull-up ON  → MIC mode (electret biased)
// false → bias pull-up OFF → LINE mode
// Global (affects every reader of the codec) — restore to true after use.
void audioCodecSetMicBias(bool micMode);

// Headphone MODE line (STM32 PA.2 → MUX, register 0x10): false = National
// (OMTP), true = American (CTIA).  Swaps the TRRS jack mic/ground routing —
// a diagnostic for the output→input feedback coupling.  Default is National.
void audioCodecSetHPMode(bool american);

// Blocking write — call from a task only.  `bytes` should be a multiple
// of 4 (16-bit stereo frames).  Returns false on failure.
bool audioCodecPlay(const uint8_t* buf, size_t bytes);

// Blocking read — call from a task only.  `bytes` should be a multiple
// of 4 (16-bit stereo frames).  Returns false on failure.
bool audioCodecRecord(uint8_t* buf, size_t bytes);

// Swap the I2S DMA pool: low=true → small (~8 ms, low latency, for the organ);
// low=false → large (~45 ms, rides out SD jitter, for SamplePlayer).  Tears the
// I2S channel down + back up, so the audio task must not be streaming across the
// call — drive it from the organ activate/deactivate path (mutually exclusive
// with sample playback).  Cheap no-op if already in the requested mode.
void audioCodecSetLowLatency(bool low);

// Fixed at codec init time — both directions share these.
static const uint32_t AUDIO_CODEC_RATE_HZ = 32000;   // matches mic_spectrum FFT
