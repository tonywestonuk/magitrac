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

// Blocking write — call from a task only.  `bytes` should be a multiple
// of 4 (16-bit stereo frames).  Returns false on failure.
bool audioCodecPlay(const uint8_t* buf, size_t bytes);

// Blocking read — call from a task only.  `bytes` should be a multiple
// of 4 (16-bit stereo frames).  Returns false on failure.
bool audioCodecRecord(uint8_t* buf, size_t bytes);

// Fixed at codec init time — both directions share these.
static const uint32_t AUDIO_CODEC_RATE_HZ = 32000;   // matches mic_spectrum FFT
