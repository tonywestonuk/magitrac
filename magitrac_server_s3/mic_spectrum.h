// mic_spectrum.h — mic capture + FFT bands + beat detect, broadcast to the
// pixel posts as PPOSB (pixelpostSendAudio).
//
// CoreS3 + Module Audio v2.2 — there is no onboard MEMS mic; the external
// capsule on the module's mic input drives the ES8388's LEFT channel
// (LINPUT1), captured via audio_codec.
//
// Lifecycle: spectrumInit() spawns the task; spectrumSetActive(true) turns
// the band processing on.  Gated in pixelpost_send.ino: on whenever the
// current effect needs the mic (pixelPostEffectNeedsMic), off otherwise.
//
// Codec is shared full-duplex with SamplePlayer — no I²S0 mutex any more.
#pragma once
#include <stdint.h>
#include "chord_detect.h"

void spectrumInit();
void spectrumSetActive(bool active);

// ── Chord recogniser ──────────────────────────────────────────────────────
// A second consumer of the same mic task.  The task captures and (when this
// is on) runs a larger 4096-pt FFT over a sliding window, folds it to a
// chroma vector and matches a chord — independent of the pixelpost bands.
// Both can be active at once; the single mic task owns the codec either way.
void     chordSetActive(bool active);
uint32_t chordResultSeq();        // bumps once per completed analysis
ChordResult chordGetResult();     // latest result (copied under a lock)

// ── Oscilloscope ──────────────────────────────────────────────────────────
// A third independent consumer of the mic task (same ref-count gate).  Each
// captured frame is reduced to one DC-blocked, lightly-averaged sample per
// display column — a clean time-domain trace.  SCOPE_COLS matches the 240-px
// display width (one column per pixel).
#define SCOPE_COLS 240
void     scopeSetActive(bool active);
uint32_t scopeResultSeq();        // bumps once per captured frame
// Copies the latest trace (one int16 per column, DC-removed) under a lock.
// The scope reads the LEFT channel (the mic) — same source as the chord
// recogniser and the pixelpost bands.
void     scopeGetTrace(int16_t* out, int cols);
