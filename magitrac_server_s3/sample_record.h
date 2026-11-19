#pragma once
#include <stdint.h>

// ADD SAMPLE — records a new sample through the mic, for the client's samples
// page (MSG_SAMPLE_REC / MSG_SAMPLE_REC_STATUS in MagiMsg.h).
//
// The client ARMs it; the server then shows its PRESS AND HOLD screen (the
// .ino draws it while sampleRecState() is ARMED, RECORDING or REVIEW and feeds
// the glass to the functions below).  Letting go plays the take back (REVIEW)
// with KEEP / CANCEL / RESAMPLE; KEEP hands it to the client (TAKE).  The take goes into PSRAM via
// the mic task (micRecStart), is written to a scratch WAV on the card when the
// glass is let go, auditioned from there at the client's request, and on SAVE
// renamed into /samples and given an id by the manifest.

// MagiLink worker: note the client's request.  Acted on by sampleRecTick.
void sampleRecRequest(uint8_t op, const char* name);

// Loop task, every pass.  Returns true when the sample cache needs a refresh
// (/samples gained a sample, or the cache was emptied to make room).
bool sampleRecTick();

// The hold screen — loop task only.
void     sampleRecPress();     // glass held (ARMED) / RESAMPLE held (REVIEW): record
void     sampleRecRelease();   // let go: write the take (~0.5 s), REVIEW, play it
void     sampleRecReplay();    // REVIEW: play the take again
void     sampleRecKeep();      // REVIEW → TAKE: the client names and saves it
void     sampleRecCancel();    // CANCEL / bezel: throw it all away, tell the client
uint8_t  sampleRecState();     // SampleRecState
uint8_t  sampleRecPitchState(); // SampleRecPitch — the take's pitch analysis
uint8_t  sampleRecPitchNote();  // FOUND: the nearest MIDI note
int8_t   sampleRecPitchCents(); //   and the cents off it
int16_t  sampleRecTuneCents();  // the TUNE that puts it on middle C (0 unless FOUND)

// MIDI task, on every note-on and note-off (velocity 0).  While the server's
// REVIEW screen is up, the keys play the take, pitched (60 = middle C once
// tuned), as polyphonic RAM voices that fade out when the key is let go.
// True = it was the take's: the note does nothing else.
bool     sampleRecKeysNote(uint8_t note, uint8_t velocity);
uint8_t  sampleRecLevel();     // mic level 0..100, updated every 50 ms
uint8_t  sampleRecLastErr();   // the err of the last status sent
bool     sampleRecClipped();   // the take has touched full scale (RECORDING / REVIEW)
uint32_t sampleRecFrames();    // recorded so far (RECORDING), the take's (REVIEW), else 0
