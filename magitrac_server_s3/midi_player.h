#pragma once

void sequencerStart();    // full reset to row 0 + play (use when loading a new song)
void sequencerResume();   // play from current position
void sequencerStop();     // stop at current position
void sequencerPanic();        // send All Notes Off (CC 123) on all 16 MIDI channels
void sequencerQueueBlock(uint8_t pattern);  // queue block to play after current ends
void sequencerCancelQueue();                // cancel queued block
int8_t sequencerQueuedBlock();              // -1 if none, else queued pattern index
void sequencerReset();    // snap to row 0 of current pattern without starting
void sequencerPause();    // freeze position, keep all state — no state callback fired
void sequencerUnpause();  // resume from frozen position
void sequencerSeek(uint8_t pattern, uint8_t row);  // jump to position and play that row (scrub)
void sequencerGoto(uint8_t pattern, uint8_t row);  // position only; tick handles WAIT/play if running

// ── Column preview ───────────────────────────────────────────────────────────
// Loop one column of one pattern at song bpm/speed for audition while editing.
// Mutually exclusive with normal playback — entering preview stops the song,
// and starting the song stops preview.  Performer-MIDI is ignored while
// previewing.
void sequencerStartPreview(uint8_t pattern, uint8_t col);
void sequencerStopPreview();
bool sequencerPreviewActive();

// ── Single-note audition ─────────────────────────────────────────────────────
// Play the note at (pattern, row, col) for ~500ms then auto note-off.  No-op
// if the song is running or preview is active (those modes are already audible).
void sequencerAuditionNote(uint8_t pattern, uint8_t row, uint8_t col);

// Fire-and-forget raw MIDI note-on (no note-off, no song lookup).  Channel is
// 1..16; note/velocity are 0..127.  Used by the client's drum-track audition.
// Notes are buffered for SEQ_RAW_AUD_HOLD_MS after the first arrival, then
// fired together — this quantises the within-step jitter that WiFi adds to
// what should be a simultaneous chord.
void sequencerAuditionRawNote(uint8_t channel, uint8_t note, uint8_t velocity);
// Call every main-loop iteration to drain the audition queue when its
// hold timer expires.
void sequencerRawAuditionTick();

bool     sequencerIsRunning();
uint16_t sequencerCurrentBPM();  // live BPM — updated each performer snap
void     sequencerSetBPM(uint16_t bpm);  // set live BPM immediately (e.g. from config edit)
void sequencerTick();
void sequencerPollMidiIn();  // call every loop() — reads performer MIDI in
void seqSendSlotEnable();    // send CC 115 ch16 with current performerMask

// Test mode — emulates a perfectly timed performer sending C-4 at a fixed interval.
// intervalMs = time between notes (e.g. 500 = C-4 every 500ms).
// Pass 0 to disable.
void sequencerSetTestMode(uint32_t intervalMs);

// Configure the SAM2695 synth embedded on the M5MIDI module to respond only
// to MIDI channel 10 (drum kit on Part 0).  Sends GS Reset + 15 "part to
// channel = OFF" SysEx bursts.  Call once at boot after midi.begin().  Lost
// on SAM2695 power-cycle (no NVS in the chip) — re-send each server boot.
// Downstream synths (e.g. Nord A1) safely ignore Roland-flavoured SysEx
// because of the manufacturer-ID byte (0x41 = Roland).
void sam2695MuteAllExcept10();

// Optional callback — called each time the sequencer advances to a new row.
// Use this to forward position to a connected client.
void seqSetRowCallback(void (*cb)(uint8_t pattern, uint8_t row));

// Optional callback — called when sequencer starts (playing=true) or stops (playing=false).
void seqSetStateCallback(void (*cb)(bool playing));

// Optional callback — called when a MIDI note-on is received while the sequencer is stopped.
// Use this to forward the note to the client for step-entry editing.
void seqSetMidiNoteInCallback(void (*cb)(uint8_t midiNote, uint8_t velocity));

// Optional callback — fires once per row tick while preview is active.
// `row` is the row that was just played.
void seqSetPreviewRowCallback(void (*cb)(uint8_t row));

#ifdef UNIT_TEST
// White-box test hooks — not for production use
void    _test_onPerformerNote(uint8_t midiNote);
uint8_t _test_getRow();
uint8_t _test_getPattern();
bool    _test_isWaiting();
bool    _test_isRunning();
#endif
