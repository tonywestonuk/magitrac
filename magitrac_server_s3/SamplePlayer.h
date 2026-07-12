#pragma once
#include <stdint.h>

// ── SamplePlayer — non-blocking WAV playback via ES8388 codec ─────────────────
//
// CoreS3 + Module Audio v2.2.  Runs a FreeRTOS task on Core 0 so the MIDI
// sequencer (Core 1) is never blocked.
//
// Both play() and stop() are fire-and-forget: triggering a new sample while
// one is playing interrupts the first (single-voice polyphony).
//
// WAVs are mono 16-bit PCM at any sample rate; linear-interp resampled to
// the codec's 32 kHz fixed rate, mono → stereo (same value on L+R).
//
// Codec is shared full-duplex with mic_spectrum — no mutex needed, both
// directions stream concurrently on the same I²S peripheral.

void samplePlayerInit();                    // call once in setup() (after audioCodecInit)
void samplePlayerPlay(const char* path, float pitchSemitones = 0,   // 0 = native pitch (tracker C-4); ±semitones transposes (fractional = cents-accurate root tuning)
                      uint8_t volume = 127);                        // 0..127, squared taper (GM CC7-ish); 127 = unity
void samplePlayerStop();                    // signal stop, returns immediately
bool samplePlayerIsPlaying();
