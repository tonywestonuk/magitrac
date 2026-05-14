#pragma once

// ── SamplePlayer — non-blocking WAV playback via I2S built-in DAC ─────────────
//
// Runs a FreeRTOS task on Core 0 so the MIDI sequencer (Core 1) is never blocked.
// SD access from the WAV task is not mutex-guarded — avoid saving/loading songs
// while a sample is playing.
//
// Both play() and stop() are fire-and-forget: triggering a new sample while
// one is playing interrupts the first (single-voice polyphony).

void samplePlayerInit();                    // call once in setup()
void samplePlayerPlay(const char* path);    // queue file, returns immediately
void samplePlayerStop();                    // signal stop, returns immediately
bool samplePlayerIsPlaying();
