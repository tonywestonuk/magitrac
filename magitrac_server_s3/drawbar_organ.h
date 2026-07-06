#pragma once
#include <stdint.h>

// Polyphonic additive ("drawbar") organ synth for the CoreS3 server.
//
// Nine partials per held note at the classic Hammond footage ratios
// (16' 5 1/3' 8' 4' 2 2/3' 2' 1 3/5' 1 1/3' 1'), each weighted by its
// drawbar setting 0..8.  A dedicated FreeRTOS task renders 16-bit stereo
// blocks straight into the shared ES8388 codec via audioCodecPlay() — it
// only touches the codec while organActive() is true, so it never fights
// SamplePlayer's WAV task (that runs on a different screen).
//
// Live MIDI drives it through a lock-free queue: the MIDI task (Core 1)
// calls organNoteOn/organNoteOff, the synth task (Core 0) drains the queue
// at the top of each block.  Latency is one block (~4 ms).

#define ORGAN_DRAWBARS 9
#define ORGAN_TYPE_COUNT 5   // 0 DRAWBAR, 1 TONEWHEEL, 2 CLAUDE, 3 NEBULA, 4 SAMPLE (spectral resynth)

// Footage labels for the UI, low harmonic → high (matches the panel order).
extern const char* const ORGAN_FOOTAGE[ORGAN_DRAWBARS];

// Create the sine table + spawn the synth task.  Call once in setup() after
// audioCodecInit().  Idle (no codec writes) until organSetActive(true).
void organInit();

// Gate the audio task.  true → start rendering and own the codec; false →
// release all voices and stop writing.  Call samplePlayerStop() before
// enabling so the WAV task isn't mid-write.
void organSetActive(bool on);
bool organActive();

// Live note events — safe to call from the MIDI task.  Enqueued, applied by
// the synth task.  velocity 0 on note-on is treated as note-off.
void organNoteOn(uint8_t note, uint8_t velocity);
void organNoteOff(uint8_t note);

// Drawbar registration, 0..8 per bar (index 0 = 16', 8 = 1').
int  organGetDrawbar(int i);
void organSetDrawbar(int i, int value);

// Number of currently sounding voices (for the header readout).
int  organVoiceCount();

// Voice model (organ type).  DRAWBAR = ideal additive; TONEWHEEL = Hammond
// physics (tempered/beating partials, octave foldback, key click).  A type
// change applies to notes played after it.
void organSetType(int t);
int  organGetType();
const char* organTypeName(int t);

// Global effects (apply to either type).
// vibChorus: 0=off, 1..3 = V1/V2/V3 (vibrato), 4..6 = C1/C2/C3 (chorus)
// leslie:    0=stop, 1=slow (chorale), 2=fast (tremolo)
// drive:     0=off, 1=on (tube-style soft-clip warmth)
void organSetVibChorus(int v);
void organSetLeslie(int v);
void organSetDrive(int v);
int  organGetVibChorus();
int  organGetLeslie();
int  organGetDrive();

// Per-type knob params (0..8).  Meaning depends on the active type:
// TONEWHEEL [0]=click;  NEBULA [0]=detune, [1]=glide, [2]=bright.
void organSetParam(int idx, int value);
int  organGetParam(int idx);
