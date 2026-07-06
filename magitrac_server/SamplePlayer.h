#pragma once

// ── SamplePlayer — non-blocking WAV playback via I2S PDM TX ──────────────────
//
// Runs a FreeRTOS task on Core 0 so the MIDI sequencer (Core 1) is never
// blocked.  SD access from the WAV task is not mutex-guarded — avoid
// saving/loading songs while a sample is playing.
//
// Both play() and stop() are fire-and-forget: triggering a new sample while
// one is playing interrupts the first (single-voice polyphony).
//
// The I²S channel is created/destroyed per sample play, so I2S0 is free
// for mic_spectrum (PDM RX) between sample plays.  Each play produces a
// small pop on begin/end — investigated extensively, no software-side
// fix found (modulator hardware transient).
//
// SD/DAC buffers (~5 KB) are heap-allocated at the start of each play
// and freed on completion — nothing sits in BSS when idle.

void samplePlayerInit();                    // call once in setup()
void samplePlayerPlay(const char* path);    // queue file, returns immediately
void samplePlayerStop();                    // signal stop, returns immediately
bool samplePlayerIsPlaying();

// Release the DAC channel so PDM mic (mic_spectrum) can claim I²S0.
// Safe to call when no sample is playing.  Called automatically at the
// end of every play; exposed for the mic-start path too.
void samplePlayerReleaseDac();
