// midi_player.cpp — sequencer + performer sync for M5Stack server

#include <Arduino.h>
#include "TrackerData.h"
#include "MagiMsg.h"
#include "midi_player.h"
#include "debug_log.h"
#include "hal/uart_ll.h"
#ifdef ARDUINO
#include "driver/uart.h"   // IDF UART driver — used only to configure the peripheral
#endif
#include "SamplePlayer.h"
#include "SampleManifest.h"
#include "drawbar_organ.h"
#include "proc_sounds.h"   // PROC_SOUND_COUNT — organ-column program → voice map

// Direct UART2 hardware FIFO access — bypasses Arduino's software ring buffer
// to minimise performer-note latency (saves ~50-200 µs per byte).
#define MIDI_UART_NUM 1
static uart_dev_t* midiHw = UART_LL_GET_HW(MIDI_UART_NUM);

// ── MIDI note offset ──────────────────────────────────────────────────────────
#define TRACKER_TO_MIDI_OFFSET 11

// ── Sequencer state ───────────────────────────────────────────────────────────
static bool     seqRunning       = false;
static bool     seqPaused        = false;   // freeze-in-place; doesn't affect seqRunning
static uint8_t  seqPattern       = 0;
static uint8_t  seqRow           = 0;
static uint32_t seqNextRowMs     = 0;
static bool     seqWaiting       = false;
static uint32_t seqWaitStartMs   = 0;    // millis() when WAIT began (for timeout)
static uint32_t seqWaitTimeoutMs = 500;  // timeout for the *current* WAIT (per variant: WAIT=500, WAT1=1000, WAT2=2000)
static int8_t   seqTranspose     = 0;
static uint16_t seqCurrentBPM    = 100;   // live BPM — derived from performer timing
// A0xx tempo override: latched by seqPlayRow when a row carries effect 0xA0
// on an output column; the snap path restores seqCurrentBPM to this after
// seqRecordSnap() so the attribute beats a same-row WAIT-derived tempo.
static bool     seqA0RowOverride = false;
static uint16_t seqA0RowBpm      = 0;

// Block-end navigation — legacy variable, kept for reset paths only.
// -1 = BLK- (go to previous), 0 = loop current, 1 = BLK+ (go to next)
static int8_t   seqEndNav        = 0;

// Performance mode: queued block override — if >= 0, this block plays after the
// current block ends (overrides blockEndNav). Reset to -1 after use.
static int8_t   seqQueuedPattern = -1;

// Subroutine-style return target — updated whenever seqPattern transitions to
// a new block; read by NAV_RNT at end-of-block to jump back to the previous
// caller.  0xFF = unset (no prior transition recorded since start/reset).
static uint8_t  seqReturnFrom    = 0xFF;

// Monotonic row counter — increments every time any row is played.
// Only the delta between two snaps matters, so absolute value is irrelevant.
static uint32_t seqAbsRow        = 0;

// Last performer snap — used to derive BPM on the next snap.
static uint32_t seqLastSnapMs     = 0;
static uint32_t seqLastSnapAbsRow = 0;
static bool     seqHaveLastSnap   = false;

// Rolling history of the last 4 BPMs derived from performer snaps.  Pushed
// from seqRecordSnap whenever it actually updates seqCurrentBPM.  Used by the
// AVRG col-0 effect to set a row's tempo to the recent average — useful for
// entering a new block at the same speed the performer was already at.
static const uint8_t SEQ_BPM_HISTORY_SIZE = 4;
static uint16_t seqBpmHistory[SEQ_BPM_HISTORY_SIZE] = {};
static uint8_t  seqBpmHistoryHead  = 0;
static uint8_t  seqBpmHistoryCount = 0;

static inline void seqPushBpmHistory(uint16_t bpm) {
    seqBpmHistory[seqBpmHistoryHead] = bpm;
    seqBpmHistoryHead = (seqBpmHistoryHead + 1) % SEQ_BPM_HISTORY_SIZE;
    if (seqBpmHistoryCount < SEQ_BPM_HISTORY_SIZE) seqBpmHistoryCount++;
}

static inline uint16_t seqAverageBpmHistory() {
    if (seqBpmHistoryCount == 0) return 0;
    uint32_t sum = 0;
    for (uint8_t i = 0; i < seqBpmHistoryCount; i++) sum += seqBpmHistory[i];
    return (uint16_t)(sum / seqBpmHistoryCount);
}

// Snap cooldown — after any WAIT/SYNC snap, ignore further snaps until this time.
// Prevents a chord or fumbled handful of keys from triggering multiple snaps;
// only the first note in the burst fires, the rest are ignored for snap purposes.
// Notes during cooldown still apply transpose.
static uint32_t seqSnapCooldownUntil = 0;

// Test mode — emulates a perfectly timed performer
static bool     seqTestMode       = false;
static uint32_t seqTestIntervalMs = 0;
static uint32_t seqTestNextMs     = 0;

// Last pitch class seen from performer (0–11, 0xFF = none yet)
// Used for KEY-CHG detection — fires when pitch class changes (octave-independent).
static uint8_t  seqLastPitchClass    = 0xFF;

// Noise-marker consumption — col-0 notes without WAIT/SYNC effect ("expected
// noise") swallow a matching performer note instead of letting it snap forward.
// Once consumed in the current pattern pass, the marker is skipped on subsequent
// walks so a later real WAIT can still be reached.  High-water mark by row;
// reset when seqRow decreases (wrap/jump/timeout) or seqPattern changes.
//
// seqLastSnapRow anchors the walk start to the SECTION (the span between two
// WAIT/SYNC snaps), not the playhead.  Without this, once the tick advances
// past a noise marker the walk starts past it and the marker becomes invisible
// — so noise consumption only worked while the performer was ahead of the
// tick.  By walking from the row after the last snap, noise markers remain
// visible to the walk for the whole section, regardless of where the tick is.
static uint8_t  seqNoiseConsumedRow  = 0xFF;   // 0xFF = nothing consumed yet
static uint8_t  seqLastSnapRow       = 0xFF;   // 0xFF = no snap in this pass
static uint8_t  seqLastSeenPat       = 0xFF;
static uint8_t  seqLastSeenRow       = 0xFF;

// Persistent position in the active pattern's note list.
// Always points to the next node to be processed (row >= seqRow).
// NOTE_NULL means we are past the last note in the pattern — no notes will
// fire until seqReposition() is called (happens on row wrap, pattern change,
// or performer snap).
static uint16_t seqNextNoteIdx       = NOTE_NULL;

// Last note playing per output column (0 = none)
static uint8_t  seqActiveNote[MAX_COLUMNS] = {};
// Per-column pixel_post "pressed" state. A NOTE row sets it on, NOTE_OFF
// turns it off, in-between rows (velocity-only / attr-only) ride the current
// state when emitting MSG_SLIDER / MSG_MOVE.
static bool     seqPpPressed[MAX_COLUMNS] = {};
// Per-column last-sent pixel_post effect index (0..95 = note - 1).
// 0xFF = "never sent yet, fire SELECT_EFFECT on next note".  NOTE_OFF does
// NOT touch this — effect choice persists across releases.
static uint8_t  seqPpLastEffect[MAX_COLUMNS];   // initialised at sequencerStart
// MIDI channel (0-based) for each column's active note — needed for correct note-off
static uint8_t  seqActiveChan[MAX_COLUMNS] = {};
// Retrigger (ratchet) state per output column.  A note with EFFECT_RTRG fires
// its first hit at row time (the normal note-on) and then `left` more hits at
// `intervalMs` spacing, subdividing the row — so "play twice" is two evenly
// spaced hits across the row.  Cleared by seqNoteOff (the next note/OFF cancels
// any roll still in flight).
static uint8_t  seqRetrigLeft[MAX_COLUMNS]       = {};   // hits remaining after the first
static uint32_t seqRetrigNextMs[MAX_COLUMNS]     = {};
static uint32_t seqRetrigIntervalMs[MAX_COLUMNS] = {};
static uint8_t  seqRetrigNote[MAX_COLUMNS]       = {};
static uint8_t  seqRetrigVel[MAX_COLUMNS]        = {};
static uint8_t  seqRetrigCh[MAX_COLUMNS]         = {};
// Per-channel bank/program/volume state — 0xFF = not set (force send on next note)
static uint8_t  seqChanBank[16];
static uint8_t  seqChanProg[16];
static uint8_t  seqChanVol[16];

// Forward declaration — defined below sequencerStart()
static void seqSendColumnSetup(uint8_t pattern, bool force = false);

// Optional row-advance callback — set via seqSetRowCallback()
static void (*seqRowCallback)(uint8_t pattern, uint8_t row) = nullptr;

// Optional play/stop state callback — set via seqSetStateCallback()
static void (*seqStateCallback)(bool playing) = nullptr;

// Optional MIDI-note-in callback — fires when note-on arrives while stopped
static void (*seqMidiNoteInCallback)(uint8_t midiNote, uint8_t velocity) = nullptr;

// Optional raw note monitor — fires for every live note-on/off on any channel,
// independent of run state (drawbar organ).  See header.
static void (*seqRawNoteCallback)(bool on, uint8_t midiNote, uint8_t velocity) = nullptr;

// ── Preview state ─────────────────────────────────────────────────────────────
// Single-column looping audition for the column-note editor.  Mutually
// exclusive with normal playback.  Performer-MIDI is ignored while active.
static bool     seqPreview         = false;
static uint8_t  seqPreviewPat      = 0;
static uint8_t  seqPreviewCol      = 0;
static uint8_t  seqPreviewRow      = 0;
static uint32_t seqPreviewNextMs   = 0;
static void   (*seqPreviewRowCallback)(uint8_t row) = nullptr;

// ── Audition state ────────────────────────────────────────────────────────────
// Single-note feedback for cell-tap entry.  Only one audition runs at a time;
// a new audition cancels any previous one.
static uint8_t  seqAuditionCol  = 0xFF;     // 0xFF = no audition active
static uint32_t seqAuditionEndMs = 0;

// ── External references ───────────────────────────────────────────────────────
extern uint8_t        srvActiveBuf[];
extern uint32_t       srvActiveBufLen;
extern bool           srvHasActive;
extern Instrument     srvInstruments[];

// Last MIDI status byte written — enables running status on output.  Reset
// to 0 on sequencer stop (and after the SAM2695 SysEx burst / raw forwards)
// so the first message after the reset always sends its status.  Defined up
// here so the raw-forward + SysEx helpers below can clear it; original site
// was further down with the rest of the sequencer-state statics.
static uint8_t  seqOutStatus = 0;

// Direct UART TX — write a byte straight to the hardware FIFO, busy-waiting
// only if the 128-byte FIFO is full (rare at MIDI rates).
static inline void midiTx(uint8_t b) {
    while (uart_ll_get_txfifo_len(midiHw) == 0) {}
    uart_ll_write_txfifo(midiHw, &b, 1);
}

// Bring up UART1 for MIDI without a HardwareSerial wrapper.  The IDF driver is
// only used to configure the peripheral (clock, pins, framing); both RX and TX
// then go through the uart_ll FIFO directly.  We silence the driver's RX
// interrupts so its ISR never drains the FIFO out from under sequencerPollMidiIn.
void sequencerMidiBegin(int rxPin, int txPin) {
#ifdef ARDUINO
    uart_config_t cfg = {};
    cfg.baud_rate  = 31250;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_XTAL;   // S3: APB-independent, no baud drift (matches Arduino HAL)
    uart_driver_install((uart_port_t)MIDI_UART_NUM, 256, 0, 0, nullptr, 0);
    uart_param_config((uart_port_t)MIDI_UART_NUM, &cfg);
    uart_set_pin((uart_port_t)MIDI_UART_NUM, txPin, rxPin,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_ll_disable_intr_mask(midiHw, UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT);
#else
    (void)rxPin; (void)txPin;
#endif
}

// Forward arbitrary MIDI bytes straight to the FIFO (MSG_MIDI_DATA path).
// Raw bytes carry their own status, so invalidate the running-status cache
// afterwards — otherwise the sequencer's next note-on is emitted statusless
// against whatever running status these bytes left the synth in.
void midiSendRawBytes(const uint8_t* bytes, int n) {
    for (int i = 0; i < n; i++) midiTx(bytes[i]);
    seqOutStatus = 0;
}

void sam2695MuteAllExcept10() {
    // 1) GS Reset — puts the SAM2695 in a known GS state.  Universal
    //    Roland address: F0 41 10 42 12 40 00 7F 00 41 F7.
    static const uint8_t gsReset[] = {
        0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7
    };
    for (size_t i = 0; i < sizeof(gsReset); i++) midiTx(gsReset[i]);
    delay(50);   // let the chip process the reset before reconfiguring

    // 2) Mute parts 1..15 by routing them to "channel OFF" (nn = 0x10).
    //    Part 0 remains on its default MIDI channel 10 (drum kit).
    //    SysEx: F0 41 00 42 12 40 (10+P) 02 10 chk F7
    //    GS checksum = (128 - ((0x40 + (0x10+P) + 0x02 + 0x10) & 0x7F)) & 0x7F
    for (uint8_t p = 1; p <= 15; p++) {
        const uint8_t addrPart = 0x10 + p;
        const uint8_t sum = (0x40 + addrPart + 0x02 + 0x10) & 0x7F;
        const uint8_t chk = (uint8_t)((128 - sum) & 0x7F);
        midiTx(0xF0); midiTx(0x41); midiTx(0x00); midiTx(0x42); midiTx(0x12);
        midiTx(0x40); midiTx(addrPart); midiTx(0x02); midiTx(0x10);
        midiTx(chk); midiTx(0xF7);
    }
    seqOutStatus = 0;   // any subsequent NOTE_ON must re-send its status byte
    Serial.println("[MIDI] SAM2695 configured — channel 10 (drums) only");
}


// Write a MIDI status byte only when it differs from the last one sent.
static inline void seqWriteStatus(uint8_t status) {
    if (status != seqOutStatus) {
        midiTx(status);
        seqOutStatus = status;
    }
}

// ── Helpers ───────────────────────────────────────────────────────────────────
static inline const Song* seqSong() {
    return reinterpret_cast<const Song*>(srvActiveBuf + sizeof(SongFileHeader));
}

// Chan sentinels marking a column as playing a sample (SFX) or the onboard
// organ.  Distinct from any real 0..15 midi channel.
#define SEQ_CHAN_SFX   0xFF
#define SEQ_CHAN_ORGAN 0xFE

// Tracker note whose sample plays at its recorded (native) pitch.  Note
// values start at 1 = C-0, so C-4 = 1 + 4*12 = 49; the pitch handed to
// samplePlayerPlay() is (cell note − this) in semitones.
#define SAMPLE_ROOT_NOTE 49

// Live tune-by-ear audition (sequencerAuditionRawNote SFX branch): the MIDI
// note that started the currently-sounding sample, so the local MIDI parser
// can stop it on that key's note-off (key down = sound, key up = stop).
// Monophonic last-note rule: releasing an OLDER held key doesn't cut the
// newer note.  0xFF = none.  Written on the comms task, read on the loop
// task — single volatile byte.
static volatile uint8_t seqSfxAudNote = 0xFF;

static void seqSfxAudNoteOff(uint8_t note) {
    if (seqRunning) return;   // guard: a stale note must never cut song SFX
    if (note == seqSfxAudNote) {
        seqSfxAudNote = 0xFF;
        samplePlayerStop();   // fades out, no click
    }
}

// Pitch (fractional semitones) for an SFX cell note: semitone tracking around
// C-4, plus the column's TRANSPOSE (whole semitones, same field as MIDI
// columns) and TUNE offset (cents from native — brings an arbitrarily-pitched
// recording in tune with the band).  All zero = native, the pre-existing
// behaviour.
static float sfxPitchFor(const ColumnSettings& cs, uint8_t note) {
    return (float)((int)note - SAMPLE_ROOT_NOTE + (int)cs.transpose)
         + (float)cs.sfxTuneCents * 0.01f;
}

// ── Organ codec ownership ─────────────────────────────────────────────────────
// The organ synth and SamplePlayer can't both write the codec (no mixer), so
// the sequencer grabs the organ only when a song/preview/audition needs it and
// releases only what it grabbed — if the organ screen (or the client's organ
// page) already owns the synth, seqOrganGrab is a no-op and release leaves it
// alone.  While the organ is active, SFX columns stay silent.
static bool seqOrganOwned = false;

static void seqOrganGrab() {
    if (organActive()) return;         // screen/page already owns it
    samplePlayerStop();                // codec must be idle before the DMA swap
    organSetActive(true);
    seqOrganOwned = true;
}

static void seqOrganRelease() {
    if (!seqOrganOwned) return;
    organSetActive(false);
    seqOrganOwned = false;
}

// True if any unmuted column routes to the organ (the song "needs" the organ).
static bool seqSongUsesOrgan() {
    const Song* s = seqSong();
    for (int c = 1; c < MAX_COLUMNS; c++)
        if (s->columns[c].midiChannel == ORGAN_CHANNEL && !s->columns[c].mute)
            return true;
    return false;
}

// Apply the song's OrganPatch (voice, sliders, drawbars, effects) to the
// synth.  Deduped per setting, so reapplying every block transition is free —
// and because the client writes the patch in the same gesture as any live
// organ-page tweak, a reapply never fights the player's hands.
// voice: 0..3 = the synth types, 4+k = procedural sound k (PROC type).
static void seqOrganApplySongPatch() {
    const OrganPatch& op = seqSong()->organ;
    if (!(op.flags & 1)) return;             // pre-v20 song — keep current sound
    uint8_t prog = op.voice;
    if (prog < ORGAN_TYPE_COUNT - 1) {
        if (organGetType() != prog) organSetType(prog);
    } else {
        int snd = prog - (ORGAN_TYPE_COUNT - 1);
        if (snd >= PROC_SOUND_COUNT) snd = PROC_SOUND_COUNT - 1;
        if (organGetType() != ORGAN_TYPE_COUNT - 1) organSetType(ORGAN_TYPE_COUNT - 1);
        if (organGetProcSound() != snd) organSetProcSound(snd);
    }
    for (int k = 0; k < ORGAN_PATCH_SLIDERS; k++)
        if (organGetParam(k) != op.sliders[k]) organSetParam(k, op.sliders[k]);
    for (int b = 0; b < ORGAN_PATCH_BARS; b++)
        if (organGetDrawbar(b) != op.bars[b]) organSetDrawbar(b, op.bars[b]);
    if (organGetVibChorus() != op.vibChorus) organSetVibChorus(op.vibChorus);
    if (organGetLeslie()    != op.leslie)    organSetLeslie(op.leslie);
    if (organGetDrive()     != op.drive)     organSetDrive(op.drive);
    if (organGetReverb()    != op.reverb)    organSetReverb(op.reverb);
}

static void seqNoteOff(int col) {
    seqRetrigLeft[col] = 0;            // cancel any ratchet still rolling on this column
    if (seqActiveNote[col] == 0) return;
    if (seqActiveChan[col] == SEQ_CHAN_SFX) {
        samplePlayerStop();
    } else if (seqActiveChan[col] == SEQ_CHAN_ORGAN) {
        organNoteOff(seqActiveNote[col]);
    } else {
        uint8_t note = seqActiveNote[col];
        seqWriteStatus((uint8_t)(0x80 | seqActiveChan[col]));
        midiTx(note);
        midiTx((uint8_t)0);
    }
    seqActiveNote[col] = 0;
    seqActiveChan[col] = 0;
}

// Fire any retrigger hits that have come due.  Each hit is a note-off of the
// current sounding note immediately followed by a fresh note-on at the same
// pitch/velocity — a re-articulation, so the column "plays the note again"
// inside its row.  Runs straight to MIDI (not via seqNoteOff, which would
// cancel the roll).  Only MIDI output columns arm this (see seqPlayRow).
static void seqProcessRetrigs(uint32_t now) {
    for (int col = 1; col < MAX_COLUMNS; col++) {
        if (seqRetrigLeft[col] == 0) continue;
        if ((int32_t)(now - seqRetrigNextMs[col]) < 0) continue;
        // Overdue by more than one interval means the main loop stalled past
        // this roll's row — drop the remaining hits rather than burst stale
        // re-articulations after the row is already gone.
        if ((int32_t)(now - seqRetrigNextMs[col]) > (int32_t)seqRetrigIntervalMs[col]) {
            seqRetrigLeft[col] = 0;
            continue;
        }
        uint8_t ch   = seqRetrigCh[col];
        uint8_t note = seqRetrigNote[col];
        uint8_t vel  = seqRetrigVel[col];
        seqWriteStatus((uint8_t)(0x80 | ch));
        midiTx(note);
        midiTx((uint8_t)0);
        seqWriteStatus((uint8_t)(0x90 | ch));
        midiTx(note);
        midiTx(vel);
        seqActiveNote[col] = note;
        seqActiveChan[col] = ch;
        seqRetrigLeft[col]--;
        seqRetrigNextMs[col] += seqRetrigIntervalMs[col];
    }
}

// Scan the active pattern's circular list from its head and position
// seqNextNoteIdx at the first node with row >= targetRow.
// Call whenever seqRow or seqPattern changes discontinuously.
static void seqReposition(uint8_t targetRow) {
    const Song*    s    = seqSong();
    uint16_t       head = s->patterns[seqPattern].noteHead;
    if (head == NOTE_NULL) { seqNextNoteIdx = NOTE_NULL; return; }
    uint16_t idx   = head;
    uint16_t steps = 0;
    do {
        // Hardening: a link damaged by a concurrent song write must degrade
        // to "no notes", never an out-of-pool read or an unbounded walk.
        if (idx >= MAX_SONG_NOTES || ++steps > MAX_SONG_NOTES) {
            seqNextNoteIdx = NOTE_NULL;
            return;
        }
        if (s->notePool[idx].row >= targetRow) {
            seqNextNoteIdx = idx;
            return;
        }
        idx = s->notePool[idx].next;
    } while (idx != head);
    seqNextNoteIdx = NOTE_NULL;   // all notes are before targetRow
}

static void seqPlayRow() {
    const Song*    s   = seqSong();
    const uint16_t head = s->patterns[seqPattern].noteHead;

    seqA0RowOverride = false;   // fresh per row walk

    // Walk nodes at seqRow in list order (col 0 first, then col 1+).
    // seqNextNoteIdx is already positioned at the first node for this row.
    uint16_t steps = 0;
    while (seqNextNoteIdx != NOTE_NULL) {
        // Same hardening as seqReposition: bail on out-of-pool or cyclic links.
        if (seqNextNoteIdx >= MAX_SONG_NOTES || ++steps > MAX_SONG_NOTES) {
            seqNextNoteIdx = NOTE_NULL;
            break;
        }
        const NoteNode& nn = s->notePool[seqNextNoteIdx];
        if (nn.row != seqRow) break;                    // past this row — stop

        uint16_t nxt   = nn.next;
        seqNextNoteIdx = (nxt == head) ? NOTE_NULL : nxt;

        // Universal output-column effect 0xFF param 0xFF = "STOP".  Halts
        // playback and sends note-off on every active column.  Excludes
        // SFX/pixel-post columns (pixel-post uses effect/param for MOVE x/y).
        if (nn.col != INPUT_COLUMN && nn.effect == 0xFF && nn.param == 0xFF) {
            const ColumnSettings& csF = s->columns[nn.col];
            if (csF.midiChannel >= 1 && csF.midiChannel <= 16) {
                debugPrintf("[SEQ] FFFF stop pat=%u row=%u col=%u\n",
                            seqPattern, seqRow, nn.col);
                sequencerStop();
                return;
            }
        }

        // Universal output-column effect 0xA0 = "set tempo".  Param byte is
        // the BPM (clamped to song's min/maxBPM).  Excludes SFX/pixel-post
        // columns since they use effect/param for their own purposes.
        // Holds until the next performer snap (WAIT/SYNC) re-derives BPM.
        if (nn.col != INPUT_COLUMN && nn.effect == 0xA0) {
            const ColumnSettings& csA = s->columns[nn.col];
            if (csA.midiChannel >= 1 && csA.midiChannel <= 16 && nn.param > 0) {
                uint16_t newBpm = nn.param;
                uint16_t minBPM = s->minBPM > 0 ? s->minBPM : 20;
                uint16_t maxBPM = s->maxBPM > 0 ? s->maxBPM : 400;
                if (newBpm < minBPM) newBpm = minBPM;
                if (newBpm > maxBPM) newBpm = maxBPM;
                seqCurrentBPM    = newBpm;
                seqA0RowOverride = true;
                seqA0RowBpm      = newBpm;
                debugPrintf("[SEQ] A0 set BPM pat=%u row=%u col=%u -> bpm=%u\n",
                            seqPattern, seqRow, nn.col, seqCurrentBPM);
            }
        }

        if (nn.col == INPUT_COLUMN) {
            // WAIT/SYNC effects are handled by tick/performer logic, not here.
            // AVRG: when the playhead reaches this row, override seqCurrentBPM
            // with the running mean of the last 4 performer-derived BPMs.
            // Lets a new block enter at the tempo the performer was already at.
            if (nn.effect == EFFECT_AVRG) {
                uint16_t avg = seqAverageBpmHistory();
                if (avg > 0) {
                    const Song* sg = seqSong();
                    uint16_t minBPM = sg->minBPM > 0 ? sg->minBPM : 20;
                    uint16_t maxBPM = sg->maxBPM > 0 ? sg->maxBPM : 400;
                    if (avg < minBPM) avg = minBPM;
                    if (avg > maxBPM) avg = maxBPM;
                    seqCurrentBPM = avg;
                    debugPrintf("[SEQ] AVRG pat=%u row=%u -> bpm=%u\n",
                                seqPattern, seqRow, seqCurrentBPM);
                }
            }
        } else if (nn.note == NOTE_OFF) {
            const ColumnSettings& cs = s->columns[nn.col];

            // PIXELPOST: "lift finger".  Send MOVE + SLIDER with pressed=0
            // using whatever values are on the OFF row (typically zeros, but
            // composer can also set a final position to release at).
            if (cs.midiChannel == PIXELPOST_CHANNEL) {
                extern void pixelpostSetSlider(uint8_t value);
                extern void pixelpostSetTouchpad(uint8_t x, uint8_t y, bool touched);
                seqPpPressed[nn.col] = false;
                uint8_t vel = (nn.velocity & 0x80) ? 0 : nn.velocity;
                uint8_t slider = (vel <= 127) ? (uint8_t)(vel * 2) : 255;
                pixelpostSetSlider(slider);
                pixelpostSetTouchpad(nn.effect, nn.param, /*touched=*/false);
                continue;
            }

            seqNoteOff(nn.col);

            // Patch-change point — when the composer places an OFF, also
            // flush any pending bank/program/volume change for that column.
            // This gives the synth time to load the new patch before the
            // next note-on (which the composer placed at the desired offset
            // after the OFF).  Note-ons themselves never send patch changes.
            // SFX columns don't use MIDI patch changes; the OFF already
            // stopped the sample above.
            if (cs.midiChannel != 0 && cs.midiChannel != SFX_CHANNEL &&
                cs.midiChannel != PIXELPOST_CHANNEL &&
                cs.midiChannel != ORGAN_CHANNEL && !cs.mute) {
                uint8_t midiCh = (uint8_t)(cs.midiChannel - 1);
                uint8_t vol    = cs.volume > 0 ? cs.volume : 100;
                if (seqChanBank[midiCh] != cs.bankMSB || seqChanProg[midiCh] != cs.program) {
                    seqWriteStatus((uint8_t)(0xB0 | midiCh));  // CC 32 — Bank Select LSB
                    midiTx((uint8_t)0x20);
                    midiTx(cs.bankMSB);
                    seqWriteStatus((uint8_t)(0xC0 | midiCh));  // Program Change
                    midiTx(cs.program);
                    seqChanBank[midiCh] = cs.bankMSB;
                    seqChanProg[midiCh] = cs.program;
                }
                if (seqChanVol[midiCh] != vol) {
                    seqWriteStatus((uint8_t)(0xB0 | midiCh));  // CC 7  — Volume
                    midiTx((uint8_t)0x07);
                    midiTx(vol);
                    seqChanVol[midiCh] = vol;
                }
            }
        } else if (nn.note != NOTE_EMPTY) {
            int     col    = nn.col;
            const ColumnSettings& cs = s->columns[col];

            // Skip muted columns or columns with no MIDI channel
            if (cs.midiChannel == 0 || cs.mute) continue;

            seqNoteOff(col);

            if (cs.midiChannel == SFX_CHANNEL) {
                if (organActive()) continue;   // organ owns the codec — SFX silent
                const char* fname = sampleManifestNameFor(cs.program);
                if (fname) {
                    char path[64];
                    snprintf(path, sizeof(path), "/samples/%s", fname);
                    samplePlayerPlay(path, sfxPitchFor(cs, nn.note), cs.volume);
                    seqActiveNote[col] = 1;               // any non-zero marks "active"
                    seqActiveChan[col] = SEQ_CHAN_SFX;    // route stop to SamplePlayer
                }
                continue;
            }

            if (cs.midiChannel == ORGAN_CHANNEL) {
                if (!organActive()) continue;   // codec not ours — stay silent
                if (nn.note < 1 || nn.note > NOTE_MAX) continue;
                // Same note transform as MIDI columns; performer transpose
                // always applies (the organ is melodic, never drums).
                int raw = (int)nn.note + TRACKER_TO_MIDI_OFFSET + seqTranspose
                        + (int)cs.transpose;
                if (raw < 0)   raw = 0;
                if (raw > 127) raw = 127;
                uint8_t vel = (nn.velocity & 0x80) ? 100 : nn.velocity;
                organNoteOn((uint8_t)raw, vel ? vel : 100);
                seqActiveNote[col] = (uint8_t)raw;
                seqActiveChan[col] = SEQ_CHAN_ORGAN;   // route stop to the organ
                continue;
            }

            if (cs.midiChannel == PIXELPOST_CHANNEL) {
                extern bool pixelpostManualActive();
                if (pixelpostManualActive()) continue;   // perf-page override owns the lights
                // PIXELPOST: a real note means "finger down at this XY with
                // this slider value, on this effect".  Effect index = note - 1
                // (so C-0 = 0, C#0 = 1, ...).  SELECT_EFFECT is always sent
                // (no per-column dedup) — after a song load the posts must
                // pick up the new song's effect even if it matches the last
                // one sent from the previous song.
                // Skip non-pitched notes (NOTE_ANY etc.); they have no
                // sensible effect mapping.
                if (nn.note < 1 || nn.note > NOTE_MAX) continue;
                extern void pixelpostSetEffect(uint8_t idx);
                extern void pixelpostSetSlider(uint8_t value);
                extern void pixelpostSetTouchpad(uint8_t x, uint8_t y, bool touched);
                seqPpPressed[col] = true;
                uint8_t effectIdx = (uint8_t)(nn.note - 1);
                pixelpostSetEffect(effectIdx);
                seqPpLastEffect[col] = effectIdx;
                uint8_t vel = (nn.velocity & 0x80) ? 100 : nn.velocity;
                uint8_t slider = (vel <= 127) ? (uint8_t)(vel * 2) : 255;
                pixelpostSetSlider(slider);
                pixelpostSetTouchpad(nn.effect, nn.param, /*touched=*/true);
                continue;
            }

            uint8_t midiCh = (uint8_t)(cs.midiChannel - 1);
            int8_t  colXp  = cs.transpose;

            // Performer-driven transpose only applies to channels enabled in
            // song->transposeChMask (default: all except ch 10 / drums).
            // Authored per-column transpose (cs.transpose) always applies.
            int8_t  xp     = ((s->transposeChMask >> midiCh) & 1) ? seqTranspose : 0;
            int raw = (int)nn.note + TRACKER_TO_MIDI_OFFSET + xp + (int)colXp;
            if (raw < 0)   raw = 0;
            if (raw > 127) raw = 127;
            uint8_t midiNote = (uint8_t)raw;

            uint8_t noteVel = (nn.velocity & 0x80) ? 100 : nn.velocity;
            seqWriteStatus((uint8_t)(0x90 | midiCh));
            midiTx(midiNote);
            midiTx(noteVel);
            seqActiveNote[col] = midiNote;
            seqActiveChan[col] = midiCh;

            // Retrigger: subdivide the row into `hits` evenly spaced
            // re-articulations.  Interval is anchored to the current row
            // duration at trigger time; later tempo changes don't reshape an
            // in-flight roll.  seqNoteOff (next note/OFF) cancels it.
            if (nn.effect == EFFECT_RTRG) {
                uint8_t  hits  = retrigHits(nn.param);
                uint8_t  speed = s->speed > 0 ? s->speed : 6;
                uint16_t bpm   = seqCurrentBPM > 0 ? seqCurrentBPM : 100;
                uint32_t rowMs = (uint32_t)speed * 2500UL / bpm;
                uint32_t interval = rowMs / hits;
                if (interval < 5) interval = 5;   // floor: never machine-gun
                seqRetrigLeft[col]       = (uint8_t)(hits - 1);
                seqRetrigIntervalMs[col] = interval;
                seqRetrigNextMs[col]     = millis() + interval;
                seqRetrigNote[col]       = midiNote;
                seqRetrigVel[col]        = noteVel;
                seqRetrigCh[col]         = midiCh;
            }
        } else {
            // NOTE_EMPTY — only meaningful on PIXELPOST columns: velocity-only
            // rows fire MSG_SLIDER, attribute-only rows fire MSG_MOVE.  Both
            // ride the column's current "pressed" state (set by the last NOTE
            // or NOTE_OFF row).
            const ColumnSettings& cs = s->columns[nn.col];
            extern bool pixelpostManualActive();
            if (cs.midiChannel == PIXELPOST_CHANNEL && !pixelpostManualActive()) {
                extern void pixelpostSetSlider(uint8_t value);
                extern void pixelpostSetTouchpad(uint8_t x, uint8_t y, bool touched);
                bool pressed = seqPpPressed[nn.col];
                if (!(nn.velocity & 0x80)) {
                    uint8_t slider = (nn.velocity <= 127)
                                     ? (uint8_t)(nn.velocity * 2) : 255;
                    pixelpostSetSlider(slider);
                }
                if (nn.effect != 0 || nn.param != 0) {
                    pixelpostSetTouchpad(nn.effect, nn.param, pressed);
                }
            }
        }
    }
}

// ── BPM derivation ────────────────────────────────────────────────────────────
static void seqRecordSnap(uint32_t now) {
    if (seqHaveLastSnap) {
        uint32_t deltaMs   = now - seqLastSnapMs;
        uint32_t deltaRows = seqAbsRow - seqLastSnapAbsRow;

        if (deltaRows > 0 && deltaMs > 50) {  // ignore jitter / double-hits
            const Song* s = seqSong();
            uint32_t derived = ((uint32_t)s->speed * 2500UL * deltaRows) / deltaMs;

            // Clamp using song's fumble-protection limits (fall back if unset)
            uint16_t minBPM = s->minBPM > 0 ? s->minBPM : 20;
            uint16_t maxBPM = s->maxBPM > 0 ? s->maxBPM : 400;
            if (derived < minBPM) derived = minBPM;
            if (derived > maxBPM) derived = maxBPM;

            seqCurrentBPM = (uint16_t)derived;
            seqPushBpmHistory(seqCurrentBPM);
        }
    }

    seqLastSnapMs      = now;
    seqLastSnapAbsRow  = seqAbsRow;
    seqHaveLastSnap    = true;
}

// ── Performer note handler ────────────────────────────────────────────────────
static void seqOnPerformerNote(uint8_t midiNote) {
    if (seqPreview) return;
    if (!srvHasActive || !seqRunning) return;
    uint32_t now       = millis();
    uint8_t pitchClass = midiNote % 12;
    const Song* s      = seqSong();
    if (seqPattern >= s->numPatterns) return;   // stale index from prior song
    const Pattern& pat = s->patterns[seqPattern];

    // Always apply InputNoteEntry actions — transpose and block switch happen for
    // every performed note, regardless of whether a cue row is found.
    const InputNoteEntry& entry = pat.inputNotes[pitchClass];
    if (entry.transposeAction == TransposeAction::NOTE) {
        seqTranspose = (int8_t)pitchClass;
    } else if (entry.transposeAction == TransposeAction::CUSTOM) {
        seqTranspose = entry.transposeValue;
    }
    if (entry.switchMode != BlockSwitch::STAY) {
        uint8_t target = entry.switchTarget;
        if (target < s->numPatterns) {
            if (target != seqPattern) seqReturnFrom = seqPattern;
            seqPattern = target;
            if (entry.switchMode == BlockSwitch::TOP) seqRow = 0;
            seqWaiting = false;
            seqSendColumnSetup(seqPattern);
            seqReposition(seqRow);
            // Performer-driven jump: don't inherit the old block's row-schedule
            // phase.  Without this, if the new block has no same-row WAIT to
            // snap on, row 0 won't fire until the previous row's scheduled
            // time elapses — felt as "row 0 is slow to start".
            seqNextRowMs = millis();

            // Re-apply transpose from the TARGET pattern's inputNotes — the
            // source pattern's transpose may differ from what the target expects.
            const InputNoteEntry& tgtEntry = s->patterns[seqPattern].inputNotes[pitchClass];
            if (tgtEntry.transposeAction == TransposeAction::NOTE) {
                seqTranspose = (int8_t)pitchClass;
            } else if (tgtEntry.transposeAction == TransposeAction::CUSTOM) {
                seqTranspose = tgtEntry.transposeValue;
            }
        }
    }

    // ── Key-change detection ──────────────────────────────────────────────────
    // Fires when pitch class changes (octave-independent).  If keyChangeMode==TOP,
    // scan remaining rows of the (possibly block-switched) pattern for BLK+/BLK-
    // nav flags, follow navigation, then jump to row 0 of the resulting block.
    bool keyChanged = (seqLastPitchClass != 0xFF) && (pitchClass != seqLastPitchClass);
    seqLastPitchClass = pitchClass;

    if (keyChanged) {
        const Pattern& kPat = s->patterns[seqPattern];
        if (kPat.keyChangeMode == (uint8_t)KeyChangeMode::TOP) {
            // Apply block-end navigation from pattern property
            uint8_t nav = kPat.blockEndNav;
            uint8_t mode = nav & NAV_MODE_MASK;
            uint8_t target = nav & NAV_TARGET_MASK;
            uint8_t fromPattern = seqPattern;
            if (nav == NAV_RNT) {
                if (seqReturnFrom != 0xFF && seqReturnFrom < s->numPatterns)
                    seqPattern = seqReturnFrom;
            } else if (mode == NAV_FWD && target > 0) {
                seqPattern += target;
                if (seqPattern >= s->numPatterns) seqPattern = 0;
            } else if (mode == NAV_BACK && target > 0) {
                if (seqPattern >= target) seqPattern -= target;
                else seqPattern = s->numPatterns - 1;
            } else if (mode == NAV_ABS) {
                seqPattern = (target < s->numPatterns) ? target : 0;
            }
            if (seqPattern != fromPattern) seqReturnFrom = fromPattern;
            seqRow               = 0;
            seqEndNav            = 0;
            seqWaiting           = false;
            seqSnapCooldownUntil = 0;
            seqSendColumnSetup(seqPattern);
            seqReposition(0);
            // Same rationale as the immediate-switch path above: reset the
            // row-schedule phase so row 0 fires on the next tick rather than
            // waiting out the previous block's leftover row time.
            seqNextRowMs = millis();
            if (seqRowCallback) seqRowCallback(seqPattern, seqRow);
        }
    }

    // Reset consumed-noise / section-anchor state if the playhead has wrapped,
    // jumped backwards, or the pattern has changed since the last performer
    // note.  The noise high-water mark and section anchor are only meaningful
    // within a single forward pass.
    if (seqPattern != seqLastSeenPat ||
        (seqLastSeenRow != 0xFF && seqRow < seqLastSeenRow)) {
        seqNoiseConsumedRow = 0xFF;
        seqLastSnapRow      = 0xFF;
        // Snap baseline is meaningless across a discontinuity: deltaMs would
        // span time from the old pattern but deltaRows only counts the few
        // rows played in the new one, producing an artificially low BPM that
        // makes row 0 of the new block crawl until the next genuine snap.
        // Treat the next snap as a fresh baseline (no derivation).
        seqHaveLastSnap = false;
    }
    seqLastSeenPat = seqPattern;
    seqLastSeenRow = seqRow;

    // Find the cue row to snap to (using updated seqPattern/seqTranspose).
    // Walk runs even during snap cooldown so noise markers still get consumed;
    // cooldown only suppresses the snap itself.
    uint8_t cueRow = 0xFF;
    bool    skipBpmUpdate = false;   // set for SYNC snaps (tempo is performer-locked elsewhere)
    if (seqWaiting) {
        // Halted — seqNextNoteIdx is parked at the WAIT node (tick peeked but didn't consume).
        // Resolve if pitch class matches, or if NOTE_ANY (match any input).
        if (seqNextNoteIdx != NOTE_NULL) {
            const NoteNode& nn = s->notePool[seqNextNoteIdx];
            if (nn.row == seqRow && nn.col == INPUT_COLUMN && nn.note != NOTE_EMPTY) {
                if (nn.note == NOTE_ANY) {
                    cueRow = seqRow;
                } else {
                    int semi = ((int)(nn.note - 1) % 12) + seqTranspose;
                    uint8_t pc = (uint8_t)((semi % 12 + 12) % 12);
                    if (pc == pitchClass) cueRow = seqRow;
                }
            }
        }
    } else {
        // Running — walk forward through col-0 nodes from the SECTION START
        // (the row after the last WAIT/SYNC snap) to find the next cue.
        // WAIT/SYNC stops the search (cue row).  A PASS marker (no-effect col-0)
        // swallows a matching pitch so it doesn't snap to the next WAIT — but
        // only while the playhead is at or before the PASS row.  Once the tick
        // has advanced past a PASS it has "gone by" and must not absorb later
        // notes meant for the WAIT (so skipping a PASS still reaches the cue).
        // Already-consumed PASS markers are skipped so a subsequent same-pitch
        // note can still reach the WAIT.
        const Pattern& curPat = s->patterns[seqPattern];
        uint16_t head     = curPat.noteHead;
        uint16_t startIdx = NOTE_NULL;
        if (head != NOTE_NULL) {
            uint16_t idx = head;
            do {
                if (seqLastSnapRow == 0xFF ||
                    s->notePool[idx].row > seqLastSnapRow) {
                    startIdx = idx;
                    break;
                }
                idx = s->notePool[idx].next;
            } while (idx != head);
            if (startIdx == NOTE_NULL) startIdx = head;  // wrap if all behind
        }
        if (startIdx != NOTE_NULL) {
            uint16_t idx = startIdx;
            do {
                const NoteNode& nn = s->notePool[idx];
                if (nn.col == INPUT_COLUMN && nn.note != NOTE_EMPTY) {
                    bool pitchMatches;
                    if (nn.note == NOTE_ANY) {
                        pitchMatches = true;
                    } else {
                        int semi = ((int)(nn.note - 1) % 12) + seqTranspose;
                        uint8_t pc = (uint8_t)((semi % 12 + 12) % 12);
                        pitchMatches = (pc == pitchClass);
                    }
                    if (IS_WAIT_EFFECT(nn.effect)) {
                        // WAIT — snap forward to it (or stop the walk).  WAIT
                        // halts the tick so it should never actually be passed
                        // naturally; sequencerGoto can land past one though,
                        // hence the row guard.
                        if (nn.row >= seqRow) {
                            if (pitchMatches) cueRow = nn.row;
                            break;
                        }
                        // passed — keep walking
                    } else if (nn.effect == EFFECT_SYNC) {
                        // SYNC — proximity-based.  Never updates BPM (Option C).
                        //   Rule 1 (on-row, late hit): SYNC was the just-played row
                        //     (nn.row + 1 == seqRow).  Reset the row timer so the
                        //     current row is extended by rowMs from the performer's
                        //     note — soft tempo absorption with no BPM drift.
                        //   Rule 2 (one row before): SYNC is the next-to-play row
                        //     (nn.row == seqRow).  Snap forward; skip BPM update.
                        //   Outside that ±1-row window, the SYNC is ignored and
                        //   the walk continues so a later WAIT/PASS can still match.
                        if (pitchMatches) {
                            if ((uint16_t)(nn.row + 1) == seqRow) {
                                uint32_t rowMs = (uint32_t)s->speed * 2500UL / seqCurrentBPM;
                                seqNextRowMs        = now + rowMs;
                                seqSnapCooldownUntil = now + rowMs/2;
                                return;
                            }
                            if (nn.row == seqRow) {
                                cueRow        = nn.row;
                                skipBpmUpdate = true;
                                break;
                            }
                        }
                        // out of proximity (or pitch mismatch) — keep walking
                    }
                    if (nn.effect == EFFECT_PASA) {
                        // PASA — Pass-All.  Pitch rule is the same as plain
                        // PASS (taken from the marker's note: NOTE_ANY =
                        // absorb any pitch, specific pitch = absorb only
                        // matching).  What's different is persistence: PASA
                        // is "always armed" — no consumed-row bookkeeping,
                        // so it absorbs every matching note until the tick
                        // advances past this row.
                        if (nn.row >= seqRow && pitchMatches) {
                            return;
                        }
                    } else {
                        // PASS — single-use.  consumedAlready bookkeeping
                        // ensures only ONE note is absorbed per marker; once
                        // used, the next same-pitch note falls through to the
                        // next WAIT/SYNC.
                        bool consumedAlready = (seqNoiseConsumedRow != 0xFF) &&
                                               (nn.row <= seqNoiseConsumedRow);
                        if (nn.row >= seqRow && !consumedAlready && pitchMatches) {
                            seqNoiseConsumedRow = nn.row;
                            return;
                        }
                    }
                    // else: passed / consumed / non-matching marker — keep walking
                }
                idx = s->notePool[idx].next;
            } while (idx != startIdx);
        }
    }

    if (cueRow == 0xFF) return;

    // Snap cooldown — suppress snap itself while still letting noise markers
    // be consumed by the walk above.
    if (now < seqSnapCooldownUntil) return;

    // Play MIDI first — zero overhead between snap detection and note output
    seqRow               = cueRow;
    seqReposition(cueRow);
    seqPlayRow();
    seqNoiseConsumedRow  = 0xFF;   // new section starts after a WAIT/SYNC snap
    seqLastSnapRow       = seqRow; // anchor next walk to the post-snap row
    seqLastSeenRow       = seqRow; // keep lastSeen in sync with the post-snap row
    uint32_t afterPlay   = millis();

    // BPM derivation after MIDI is out (uses pre-increment absRow).
    // SYNC-driven snaps skip this — tempo is set by WAITs only (Option C).
    if (!skipBpmUpdate) seqRecordSnap(now);
    // If the just-played row also carried an A0xx tempo-set attribute,
    // it overrides the WAIT-derived BPM (matches walk-order intuition:
    // output-col attribute fires after col-0 WAIT).
    if (seqA0RowOverride) seqCurrentBPM = seqA0RowBpm;
    uint32_t rowMs = (uint32_t)s->speed * 2500UL / seqCurrentBPM;

    // Schedule and log AFTER MIDI is out
    seqAbsRow++;
    seqRow               = (cueRow + 1) % s->patterns[seqPattern].length;
    seqWaiting           = false;
    seqNextRowMs         = afterPlay + rowMs;
    seqSnapCooldownUntil = afterPlay + rowMs/2;
    if (seqRowCallback) seqRowCallback(seqPattern, seqRow);
}

// ── MIDI input polling ────────────────────────────────────────────────────────
static void sequencerPollMidiInBody() {
    // Test mode — inject perfectly timed C-4 notes
    if (seqTestMode && seqRunning) {
        uint32_t now = millis();
        if (now >= seqTestNextMs) {
            seqTestNextMs = now + seqTestIntervalMs;
            debugPrintf("[T] TEST-IN  t=%lu\n", now);
            seqOnPerformerNote(60);  // C-4 = MIDI note 60
        }
    }

    static uint8_t midiStatus = 0;
    static uint8_t midiByte1  = 0;
    static uint8_t midiCount  = 0;

    while (uart_ll_get_rxfifo_len(midiHw) > 0) {
        uint8_t b;
        uart_ll_read_rxfifo(midiHw, &b, 1);

        if (b & 0x80) {
            // System real-time messages (0xF8–0xFF: clock, active sensing, reset…)
            // are single-byte and must not update running status — skip them.
            if (b >= 0xF8) continue;
            midiStatus = b;
            midiCount  = 0;
            continue;
        }

        if ((midiStatus & 0xF0) == 0x90) {
            if (midiCount == 0) {
                midiByte1 = b;
                midiCount = 1;
            } else {
                uint8_t velocity = b;
                midiCount = 0;
                // Raw monitor first — sees every note on any channel (organ).
                if (seqRawNoteCallback) seqRawNoteCallback(velocity > 0, midiByte1, velocity);
                if (velocity == 0) seqSfxAudNoteOff(midiByte1);   // running-status note-off
                if (velocity >= 20) {
                    const Song* s = seqSong();
                    uint8_t ch = midiStatus & 0x0F;  // 0-based channel
                    // Channel filter applies in both play and step-input.
                    bool chanOk = (s->midiInChannel == 0) || (ch == s->midiInChannel - 1);
                    if (chanOk && !seqPreview) {
                        if (seqRunning) {
                            // Performer-sync also windows by note range so
                            // stray keys outside the cue band don't snap.
                            bool noteOk = (midiByte1 >= s->midiInNoteMin) && (midiByte1 <= s->midiInNoteMax);
                            if (noteOk) seqOnPerformerNote(midiByte1);
                        } else if (seqMidiNoteInCallback) {
                            // Step-input while stopped — no note-range filter;
                            // any note on the MIDI-in channel enters the column.
                            seqMidiNoteInCallback(midiByte1, velocity);
                        }
                    }
                }
            }
        } else if ((midiStatus & 0xF0) == 0x80) {
            if (midiCount == 0) {
                midiByte1 = b;       // note number
                midiCount = 1;
            } else {
                midiCount = 0;       // velocity byte — message complete
                if (seqRawNoteCallback) seqRawNoteCallback(false, midiByte1, 0);
                seqSfxAudNoteOff(midiByte1);
            }
        } else if ((midiStatus & 0xF0) == 0xB0) {
            if (midiCount == 0) {
                midiByte1 = b;       // controller number
                midiCount = 1;
            } else {
                midiCount = 0;       // controller value — message complete
                // CC64 damper pedal → organ sustain (any channel; the synth
                // ignores it unless it's active, so no gating needed here).
                if (midiByte1 == 64) organSetSustain(b >= 64);
            }
        }
    }
}

// ── Column setup — send bank/program for each column entering a block ────────
// Called at sequencer start and every block transition.  Skips channels that
// are already configured with the correct bank/program, and skips channels
// already touched this pass (first column wins when two columns share a channel
// but have different bank/program — the per-note check in seqPlayRow handles
// switching mid-block).
static void seqSendColumnSetup(uint8_t pattern, bool force) {
    const Song* s = seqSong();
    if (pattern >= s->numPatterns) return;

    bool chanDone[16] = {};  // true = already configured this pass

    for (int col = 1; col < MAX_COLUMNS; col++) {
        const ColumnSettings& cs = s->columns[col];
        if (cs.midiChannel == 0 || cs.mute) continue;        // unset or muted — skip
        if (cs.midiChannel == SFX_CHANNEL) continue;        // SFX has no MIDI patch
        if (cs.midiChannel == PIXELPOST_CHANNEL) continue;  // pixel_post has no MIDI patch
        if (cs.midiChannel == ORGAN_CHANNEL) {   // organ: no MIDI — apply the
            seqOrganApplySongPatch();            // song's patch (deduped, cheap)
            continue;
        }
        uint8_t midiCh = (uint8_t)(cs.midiChannel - 1);
        if (chanDone[midiCh]) continue;                     // already set this pass
        chanDone[midiCh] = true;

        uint8_t vol = cs.volume > 0 ? cs.volume : 100;

        // Skip if channel is already configured with this bank/program/volume.
        // When force==true, bypass all caching — needed on PLAY in case the
        // user manually changed bank/program on the synth.
        if (!force &&
            seqChanBank[midiCh] == cs.bankMSB &&
            seqChanProg[midiCh] == cs.program &&
            seqChanVol[midiCh]  == vol) continue;

        if (force ||
            seqChanBank[midiCh] != cs.bankMSB ||
            seqChanProg[midiCh] != cs.program) {
            midiTx((uint8_t)(0xB0 | midiCh));  // CC 32 — Bank Select LSB
            midiTx((uint8_t)0x20);
            midiTx(cs.bankMSB);
            midiTx((uint8_t)(0xC0 | midiCh));  // Program Change
            midiTx(cs.program);
            seqChanBank[midiCh] = cs.bankMSB;
            seqChanProg[midiCh] = cs.program;
        }
        if (force || seqChanVol[midiCh] != vol) {
            midiTx((uint8_t)(0xB0 | midiCh));  // CC 7  — Volume
            midiTx((uint8_t)0x07);
            midiTx(vol);
            seqChanVol[midiCh] = vol;
        }
        seqOutStatus = 0;  // force full status on next message
        debugPrintf("[SEQ] col-setup col%d ch%d: bank=%u prog=%u vol=%u\n",
                      col, cs.midiChannel, cs.bankMSB, cs.program, vol);
    }
}

// ── Preview helpers ───────────────────────────────────────────────────────────
// Walk the preview pattern's note list looking for the cell at
// (seqPreviewRow, seqPreviewCol).  Plays at most one note (NOTE_OFF, pitched,
// or no-op for empty / NOTE_ANY).
static void seqPlayPreviewRow() {
    const Song* s = seqSong();
    if (seqPreviewPat >= s->numPatterns) return;
    const ColumnSettings& cs = s->columns[seqPreviewCol];
    if (cs.midiChannel == 0 || cs.mute) return;

    uint16_t head = s->patterns[seqPreviewPat].noteHead;
    if (head == NOTE_NULL) return;

    uint16_t idx = head;
    do {
        const NoteNode& nn = s->notePool[idx];
        if (nn.row == seqPreviewRow && nn.col == seqPreviewCol) {
            if (nn.note == NOTE_OFF) {
                seqNoteOff(seqPreviewCol);
            } else if (nn.note != NOTE_EMPTY && nn.note != NOTE_ANY) {
                seqNoteOff(seqPreviewCol);
                if (cs.midiChannel == SFX_CHANNEL) {
                    if (organActive()) return;   // organ owns the codec — SFX silent
                    const char* fname = sampleManifestNameFor(cs.program);
                    if (fname) {
                        char path[64];
                        snprintf(path, sizeof(path), "/samples/%s", fname);
                        samplePlayerPlay(path, sfxPitchFor(cs, nn.note), cs.volume);
                        seqActiveNote[seqPreviewCol] = 1;
                        seqActiveChan[seqPreviewCol] = SEQ_CHAN_SFX;
                    }
                    return;
                }
                if (cs.midiChannel == ORGAN_CHANNEL) {
                    if (!organActive()) return;
                    if (nn.note < 1 || nn.note > NOTE_MAX) return;
                    int raw = (int)nn.note + TRACKER_TO_MIDI_OFFSET + (int)cs.transpose;
                    if (raw < 0)   raw = 0;
                    if (raw > 127) raw = 127;
                    uint8_t vel = (nn.velocity & 0x80) ? 100 : nn.velocity;
                    organNoteOn((uint8_t)raw, vel ? vel : 100);
                    seqActiveNote[seqPreviewCol] = (uint8_t)raw;
                    seqActiveChan[seqPreviewCol] = SEQ_CHAN_ORGAN;
                    return;
                }
                if (cs.midiChannel == PIXELPOST_CHANNEL) {
                    extern bool pixelpostManualActive();
                    if (pixelpostManualActive()) return;   // perf-page override owns the lights
                    if (nn.note < 1 || nn.note > NOTE_MAX) return;
                    extern void pixelpostSetEffect(uint8_t idx);
                    extern void pixelpostSetSlider(uint8_t value);
                    extern void pixelpostSetTouchpad(uint8_t x, uint8_t y, bool touched);
                    uint8_t effectIdx = (uint8_t)(nn.note - 1);
                    pixelpostSetEffect(effectIdx);
                    seqPpLastEffect[seqPreviewCol] = effectIdx;
                    uint8_t vel = (nn.velocity & 0x80) ? 100 : nn.velocity;
                    uint8_t slider = (vel <= 127) ? (uint8_t)(vel * 2) : 255;
                    pixelpostSetSlider(slider);
                    pixelpostSetTouchpad(nn.effect, nn.param, /*touched=*/true);
                    return;
                }
                uint8_t midiCh = (uint8_t)(cs.midiChannel - 1);
                int raw = (int)nn.note + TRACKER_TO_MIDI_OFFSET + (int)cs.transpose;
                if (raw < 0)   raw = 0;
                if (raw > 127) raw = 127;
                uint8_t midiNote = (uint8_t)raw;
                uint8_t noteVel  = (nn.velocity & 0x80) ? 100 : nn.velocity;
                seqWriteStatus((uint8_t)(0x90 | midiCh));
                midiTx(midiNote);
                midiTx(noteVel);
                seqActiveNote[seqPreviewCol] = midiNote;
                seqActiveChan[seqPreviewCol] = midiCh;
            }
            return;
        }
        idx = nn.next;
    } while (idx != head);
}

// ── Public API ────────────────────────────────────────────────────────────────
void sequencerStart() {
    if (!srvHasActive) return;
    if (seqPreview) sequencerStopPreview();

    for (int c = 0; c < MAX_COLUMNS; c++) seqNoteOff(c);
    memset(seqActiveChan,   0,    sizeof(seqActiveChan));
    memset(seqChanBank,     0xFF, sizeof(seqChanBank));  // 0xFF = force first send
    memset(seqChanProg,     0xFF, sizeof(seqChanProg));
    memset(seqChanVol,      0xFF, sizeof(seqChanVol));
    memset(seqPpPressed,    0,    sizeof(seqPpPressed));
    memset(seqPpLastEffect, 0xFF, sizeof(seqPpLastEffect));  // 0xFF = force first SELECT_EFFECT

    const Song* s = seqSong();
    uint8_t startPat = s->startPattern < s->numPatterns ? s->startPattern : 0;

    // Organ codec ownership follows the song (~60 ms DMA swap, so at start
    // only): grab if any unmuted organ column, release ours otherwise.
    if (seqSongUsesOrgan()) seqOrganGrab();
    else                    seqOrganRelease();

    seqSendColumnSetup(startPat, /*force=*/true);

    seqSendSlotEnable();

    seqPattern       = 0;
    seqRow           = 0;
    seqWaiting       = false;
    seqEndNav        = 0;
    seqQueuedPattern = -1;
    seqReturnFrom    = 0xFF;
    seqTranspose     = 0;
    seqCurrentBPM   = seqSong()->bpm > 0 ? seqSong()->bpm : 100;
    seqAbsRow            = 0;
    seqHaveLastSnap      = false;
    seqBpmHistoryHead    = 0;
    seqBpmHistoryCount   = 0;
    seqSnapCooldownUntil = 0;
    seqLastPitchClass    = 0xFF;
    seqRunning           = true;
    seqNextRowMs         = millis();
    seqReposition(0);

    debugPrintf("[SEQ] start t=%lu bpm=%u speed=%u\n",
                  millis(), seqCurrentBPM, seqSong()->speed);
    if (seqStateCallback) seqStateCallback(true);
}

void sequencerResume() {
    if (!srvHasActive) return;
    if (seqPreview) sequencerStopPreview();
    // Clamp before any patterns[seqPattern] deref — see sequencerReset() for
    // the rationale; a bare MSG_PLAY (without a preceding seek) can land here
    // with seqPattern left over from a different song.
    const Song* s0 = seqSong();
    if (s0 && seqPattern >= s0->numPatterns) {
        seqPattern = 0;
        seqRow     = 0;
        seqReposition(0);
    }
    seqPaused    = false;
    seqWaiting   = false;
    seqRunning   = true;
    seqTranspose = 0;            // PLAY always starts at concert pitch
    seqCurrentBPM = seqSong()->bpm > 0 ? seqSong()->bpm : 100;
    seqHaveLastSnap = false;
    seqNextRowMs = millis();
    if (seqSongUsesOrgan()) seqOrganGrab();
    else                    seqOrganRelease();
    // Force-resend bank/program/volume on PLAY in case the user manually
    // changed the synth while stopped.
    seqSendColumnSetup(seqPattern, /*force=*/true);
    seqSendSlotEnable();
    debugPrintf("[SEQ] resume t=%lu pat=%u row=%u bpm=%u\n", millis(), seqPattern, seqRow, seqCurrentBPM);
    if (seqStateCallback) seqStateCallback(true);
}

void sequencerStop() {
    seqRunning        = false;
    seqPaused         = false;
    seqWaiting        = false;
    seqQueuedPattern  = -1;    // clear any performance mode queue
    seqLastPitchClass = 0xFF;
    for (int c = 0; c < MAX_COLUMNS; c++) seqNoteOff(c);
    seqOrganRelease();
    seqOutStatus = 0;  // force full status byte on next start
    debugPrintf("[SEQ] stop t=%lu\n", millis());
    if (seqStateCallback) seqStateCallback(false);
}

void sequencerStartPreview(uint8_t pat, uint8_t col) {
    if (!srvHasActive) return;
    const Song* s = seqSong();
    if (pat >= s->numPatterns)  return;
    if (col >= MAX_COLUMNS) return;               // col 0 is silent but still animates the playhead

    if (seqRunning) sequencerStop();              // mutually exclusive
    if (seqPreview) seqNoteOff(seqPreviewCol);    // restart on same/different col — avoid stuck notes

    seqPreview       = true;
    seqPreviewPat    = pat;
    seqPreviewCol    = col;
    seqPreviewRow    = 0;
    seqPreviewNextMs = millis();
    if (s->columns[col].midiChannel == ORGAN_CHANNEL) seqOrganGrab();
    memset(seqChanBank, 0xFF, sizeof(seqChanBank));
    memset(seqChanProg, 0xFF, sizeof(seqChanProg));
    memset(seqChanVol,  0xFF, sizeof(seqChanVol));
    seqSendColumnSetup(seqPreviewPat, /*force=*/true);
    debugPrintf("[SEQ] preview start pat=%u col=%u\n", pat, col);
}

void sequencerStopPreview() {
    if (!seqPreview) return;
    seqPreview = false;
    seqNoteOff(seqPreviewCol);
    seqOrganRelease();
    seqOutStatus = 0;
    debugPrintf("[SEQ] preview stop\n");
}

bool sequencerPreviewActive() {
    return seqPreview;
}

void sequencerAuditionNote(uint8_t pat, uint8_t row, uint8_t col) {
    if (!srvHasActive)              return;
    if (seqRunning)                 return;   // song would mask anyway, skip
    if (seqPreview)                 return;   // preview is already auditioning
    if (col == 0 || col >= MAX_COLUMNS) return;

    const Song* s = seqSong();
    if (pat >= s->numPatterns)      return;

    const ColumnSettings& cs = s->columns[col];
    if (cs.midiChannel == 0 || cs.mute) return;

    // Walk the pattern's note list looking for the cell at (row, col).
    uint16_t head = s->patterns[pat].noteHead;
    if (head == NOTE_NULL)          return;
    Note nn = { NOTE_EMPTY, 0, 0, 0 };
    {
        uint16_t idx = head;
        do {
            const NoteNode& n = s->notePool[idx];
            if (n.row == row && n.col == col) {
                nn.note = n.note; nn.velocity = n.velocity;
                nn.effect = n.effect; nn.param = n.param;
                break;
            }
            idx = n.next;
        } while (idx != head);
    }
    if (nn.note == NOTE_EMPTY || nn.note == NOTE_OFF || nn.note == NOTE_ANY) return;

    // Cancel any prior audition (possibly on a different column).
    if (seqAuditionCol != 0xFF) seqNoteOff(seqAuditionCol);

    // Make sure bank/program/volume are flushed before the note-on.  No-op
    // for columns whose cache is already current.
    seqSendColumnSetup(pat, /*force=*/false);

    seqNoteOff(col);   // silence anything stale on this column

    if (cs.midiChannel == SFX_CHANNEL) {
        // Editing convenience: an SFX audition takes the codec back from a
        // sequencer-owned organ (a screen-owned organ keeps it — stay silent).
        seqOrganRelease();
        if (organActive()) return;
        const char* fname = sampleManifestNameFor(cs.program);
        if (fname) {
            char path[64];
            snprintf(path, sizeof(path), "/samples/%s", fname);
            samplePlayerPlay(path, sfxPitchFor(cs, nn.note), cs.volume);
            seqActiveNote[col] = 1;
            seqActiveChan[col] = SEQ_CHAN_SFX;
            seqAuditionCol   = col;
            seqAuditionEndMs = millis() + 500;
            debugPrintf("[SEQ] audition col=%u SFX=%s\n", col, fname);
        }
        return;
    }

    if (cs.midiChannel == ORGAN_CHANNEL) {
        seqOrganGrab();
        int raw = (int)nn.note + TRACKER_TO_MIDI_OFFSET + (int)cs.transpose;
        if (raw < 0)   raw = 0;
        if (raw > 127) raw = 127;
        uint8_t vel = (nn.velocity & 0x80) ? 100 : nn.velocity;
        organNoteOn((uint8_t)raw, vel ? vel : 100);
        seqActiveNote[col] = (uint8_t)raw;
        seqActiveChan[col] = SEQ_CHAN_ORGAN;
        seqAuditionCol   = col;
        seqAuditionEndMs = millis() + 500;
        debugPrintf("[SEQ] audition col=%u ORGAN note=%d\n", col, raw);
        return;
    }

    if (cs.midiChannel == PIXELPOST_CHANNEL) return;   // nothing sensible to audition

    uint8_t midiCh = (uint8_t)(cs.midiChannel - 1);
    int raw = (int)nn.note + TRACKER_TO_MIDI_OFFSET + (int)cs.transpose;
    if (raw < 0)   raw = 0;
    if (raw > 127) raw = 127;
    uint8_t midiNote = (uint8_t)raw;
    uint8_t noteVel  = (nn.velocity & 0x80) ? 100 : nn.velocity;
    seqWriteStatus((uint8_t)(0x90 | midiCh));
    midiTx(midiNote);
    midiTx(noteVel);
    seqActiveNote[col] = midiNote;
    seqActiveChan[col] = midiCh;

    seqAuditionCol   = col;
    seqAuditionEndMs = millis() + 500;
    debugPrintf("[SEQ] audition col=%u note=%u\n", col, midiNote);
}

// 50 ms quantise window absorbs WiFi delivery jitter inside one drum-step
// burst so all notes of a chord land together on the UART.  Step-to-step
// timing is no worse than the first packet's jitter alone — small.
static const int      SEQ_RAW_AUD_Q_MAX   = 16;
static const uint32_t SEQ_RAW_AUD_HOLD_MS = 50;
struct SeqRawAudEntry { uint8_t chan, note, vel; };
static SeqRawAudEntry seqRawAudQ[SEQ_RAW_AUD_Q_MAX];
static uint8_t        seqRawAudCount  = 0;
static bool           seqRawAudArmed  = false;
static uint32_t       seqRawAudFireMs = 0;

// Pending audition program change (e.g. drum-kit select on ch10).  Set by the
// command task, emitted on the MIDI task so it shares seqOutStatus.
static volatile bool    seqAudProgPending = false;
static volatile uint8_t seqAudProgChan    = 0;
static volatile uint8_t seqAudProgram     = 0;

void sequencerAuditionRawNote(uint8_t channel, uint8_t note, uint8_t velocity,
                              uint8_t col) {
    // SFX: play column `col`'s sample at the raw MIDI pitch (60 = native +
    // TUNE) — the client's tune-by-ear path while the sequencer is stopped.
    // Fire-and-forget straight to the SamplePlayer queue (no SD, no MIDI
    // running-status involvement), so no need for the raw-audition hold queue.
    if (channel == SFX_CHANNEL) {
        if (col >= MAX_COLUMNS || note > 127) return;
        if (organActive()) return;             // organ owns the codec
        const ColumnSettings& cs = seqSong()->columns[col];
        const char* fname = sampleManifestNameFor(cs.program);
        if (!fname) return;
        char path[64];
        snprintf(path, sizeof(path), "/samples/%s", fname);
        samplePlayerPlay(path, (float)((int)note - 60 + (int)cs.transpose)
                             + (float)cs.sfxTuneCents * 0.01f,
                         cs.volume);
        seqSfxAudNote = note;   // the local MIDI parser stops it on note-off
        return;
    }
    if (channel < 1 || channel > 16) return;
    if (note > 127 || velocity == 0 || velocity > 127) return;
    if (seqRawAudCount >= SEQ_RAW_AUD_Q_MAX) return;
    if (!seqRawAudArmed) {
        seqRawAudArmed  = true;
        seqRawAudFireMs = millis() + SEQ_RAW_AUD_HOLD_MS;
    }
    SeqRawAudEntry& e = seqRawAudQ[seqRawAudCount++];
    e.chan = channel; e.note = note; e.vel = velocity;
}

void sequencerAuditionProgram(uint8_t channel, uint8_t program) {
    if (channel < 1 || channel > 16 || program > 127) return;
    seqAudProgChan    = channel;
    seqAudProgram     = program;
    seqAudProgPending = true;
}

void sequencerRawAuditionTick() {
    // Emit any pending program change first so the kit is selected before the
    // next note.  A program change leaves the synth in PC running status, so
    // reset seqOutStatus to force a fresh status byte on the following note —
    // without this every later note-on is sent statusless and stays silent.
    if (seqAudProgPending) {
        seqAudProgPending = false;
        uint8_t ch = (uint8_t)(seqAudProgChan - 1);
        seqWriteStatus((uint8_t)(0xC0 | ch));   // Program Change
        midiTx(seqAudProgram);
        seqOutStatus = 0;
    }

    if (!seqRawAudArmed) return;
    if ((int32_t)(millis() - seqRawAudFireMs) < 0) return;
    for (uint8_t i = 0; i < seqRawAudCount; i++) {
        const SeqRawAudEntry& e = seqRawAudQ[i];
        uint8_t ch = (uint8_t)(e.chan - 1);
        seqWriteStatus((uint8_t)(0x90 | ch));
        midiTx(e.note);
        midiTx(e.vel);
    }
    seqRawAudCount = 0;
    seqRawAudArmed = false;
}

void sequencerPanic() {
    for (uint8_t ch = 0; ch < 16; ch++) {
        midiTx((uint8_t)(0xB0 | ch));   // CC on channel
        midiTx((uint8_t)123);           // All Notes Off
        midiTx((uint8_t)0);
    }
    seqOutStatus = 0;  // force full status byte on next note
    debugPrintf("[SEQ] panic — all notes off\n");
}

void sequencerQueueBlock(uint8_t pattern) {
    // If waiting on row 0, the current block hasn't logically started yet —
    // switch immediately instead of queuing for block-end.
    if (seqWaiting && seqRow == 0) {
        const Song* s = seqSong();
        if (pattern < s->numPatterns) {
            for (int c = 0; c < MAX_COLUMNS; c++) seqNoteOff(c);
            if (pattern != seqPattern) seqReturnFrom = seqPattern;
            seqPattern = pattern;
            seqRow     = 0;
            seqWaiting = false;
            seqSendColumnSetup(seqPattern);
            seqReposition(0);
            seqNextRowMs = millis();
            debugPrintf("[SEQ] immediate switch (WAIT row 0) -> pat=%u\n", pattern);
            if (seqRowCallback) seqRowCallback(seqPattern, seqRow);
        }
        return;
    }
    seqQueuedPattern = (int8_t)pattern;
    debugPrintf("[SEQ] queue block %u\n", pattern);
}

void sequencerCancelQueue() {
    seqQueuedPattern = -1;
    debugPrintf("[SEQ] cancel queued block\n");
}

int8_t sequencerQueuedBlock() {
    return seqQueuedPattern;
}

void seqSendSlotEnable() {
    const Song* s = seqSong();
    midiTx((uint8_t)0xBF);                          // CC on MIDI channel 16
    midiTx((uint8_t)115);
    midiTx((uint8_t)((s->performerMask & 0x0F) << 3));
    debugPrintf("[SEQ] slotEnable mask=0x%02X val=0x%02X\n",
                  s->performerMask & 0x0F, (s->performerMask & 0x0F) << 3);
}

void sequencerReset() {
    // Clamp seqPattern against the *current* song.  Every song-replacement
    // path (load from SD, client push, new song) lands here while seqPattern
    // still reflects the previous song; if that song had more blocks, the
    // stale index reads a Pattern slot that's logically deleted, and its
    // noteHead can be a garbage value that walks the linked list off the end
    // of the note pool.  Reset to block 0 if out of range.
    if (srvHasActive) {
        const Song* s = seqSong();
        if (s && seqPattern >= s->numPatterns) seqPattern = 0;
    }
    seqRow            = 0;
    seqWaiting        = false;
    seqEndNav         = 0;
    seqQueuedPattern  = -1;
    seqReposition(0);
    debugPrintf("[SEQ] reset t=%lu pat=%u row=0\n", millis(), seqPattern);
    if (seqRowCallback) seqRowCallback(seqPattern, seqRow);
}

void sequencerSeek(uint8_t pattern, uint8_t row) {
    if (!srvHasActive) return;
    const Song* s = seqSong();
    if (pattern >= s->numPatterns) return;
    if (row >= s->patterns[pattern].length) return;

    bool patChanged = (pattern != seqPattern);
    if (patChanged) {
        for (int c = 0; c < MAX_COLUMNS; c++) seqNoteOff(c);
        seqSendColumnSetup(pattern);
        seqReturnFrom = seqPattern;
    }

    seqPattern = pattern;
    seqRow     = row;
    seqWaiting = false;
    seqReposition(row);

    // Restart the row-schedule timer at "now" — the previous block's pending
    // seqNextRowMs is what was making row 0 of the new block last longer than
    // rowMs.  Letting the next tick fire row 0 (instead of playing it inline
    // here) keeps the schedule chain anchored to this instant.
    if (seqRunning) seqNextRowMs = millis();
    seqHaveLastSnap = false;   // snap baseline meaningless across the jump

    debugPrintf("[SEQ] seek t=%lu pat=%u row=%u\n", millis(), seqPattern, seqRow);
    if (seqRowCallback) seqRowCallback(seqPattern, seqRow);
}

void sequencerGoto(uint8_t pat, uint8_t row) {
    if (!srvHasActive) return;
    const Song* s = seqSong();
    if (pat >= s->numPatterns) return;
    if (row >= s->patterns[pat].length) return;

    bool patChanged = (pat != seqPattern);

    if (patChanged) {
        for (int c = 0; c < MAX_COLUMNS; c++) seqNoteOff(c);
        seqSendColumnSetup(pat);
        seqReturnFrom = seqPattern;
    }

    seqPattern = pat;
    seqRow     = row;
    seqWaiting = false;
    seqReposition(row);

    // When running, kick the tick to fire immediately — WAIT detection and
    // row playback live in sequencerTick, so we don't duplicate them here.
    // When stopped, no tick runs, no audio.
    if (seqRunning) seqNextRowMs = millis();

    debugPrintf("[SEQ] goto t=%lu pat=%u row=%u running=%d\n",
                  millis(), seqPattern, seqRow, (int)seqRunning);
    if (seqRowCallback) seqRowCallback(seqPattern, seqRow);
}

bool sequencerIsRunning() {
    return seqRunning;
}

uint16_t sequencerCurrentBPM() {
    return seqCurrentBPM;
}

void sequencerSetBPM(uint16_t bpm) {
    if (bpm == 0) return;
    seqCurrentBPM = bpm;
    debugPrintf("[SEQ] setBPM %u\n", bpm);
}

void sequencerPause() {
    seqPaused = true;
    for (int c = 0; c < MAX_COLUMNS; c++) seqNoteOff(c);
    debugPrintf("[SEQ] pause t=%lu\n", millis());
}

void sequencerUnpause() {
    if (!seqPaused) return;
    seqPaused    = false;
    seqNextRowMs = millis();  // don't play a burst of missed rows on resume
    debugPrintf("[SEQ] unpause t=%lu\n", millis());
}

static void sequencerTickBody() {
    // Audition timeout — fires regardless of play / preview state, so a
    // stranded audition note never sits on indefinitely.
    if (seqAuditionCol != 0xFF && (int32_t)(millis() - seqAuditionEndMs) >= 0) {
        seqNoteOff(seqAuditionCol);
        seqAuditionCol = 0xFF;
        seqOutStatus = 0;
    }

    // Retrigger rolls run off the row clock — service them every loop while the
    // sequencer is live so a roll finishes even between row boundaries.
    if (seqRunning && !seqPaused && srvHasActive) seqProcessRetrigs(millis());

    if (seqPreview) {
        if (!srvHasActive) return;
        uint32_t now = millis();
        if (now < seqPreviewNextMs) return;

        seqPlayPreviewRow();

        const Song* s = seqSong();
        uint16_t bpm   = s->bpm   > 0 ? s->bpm   : 100;
        uint8_t  speed = s->speed > 0 ? s->speed : 6;
        uint32_t rowMs = (uint32_t)speed * 2500UL / bpm;
        uint32_t after = millis();

        if (seqPreviewRowCallback) seqPreviewRowCallback(seqPreviewRow);

        uint8_t plen = s->patterns[seqPreviewPat].length;
        if (plen == 0) plen = 1;
        seqPreviewRow = (seqPreviewRow + 1) % plen;
        seqPreviewNextMs = after + rowMs;
        return;
    }

    if (!seqRunning || seqPaused || !srvHasActive) return;
    uint32_t now = millis();

    if (seqWaiting) {
        uint32_t elapsed = now - seqWaitStartMs;
        if (elapsed > seqWaitTimeoutMs) {
            // Timeout — performer stopped.
            // If a block is queued, switch to it (the current block never
            // logically started if WAIT was on row 0).  Otherwise restart
            // the current block from the top.
            if (seqQueuedPattern >= 0) {
                const Song* s2 = seqSong();
                seqPattern = (uint8_t)seqQueuedPattern;
                if (seqPattern >= s2->numPatterns) seqPattern = 0;
                seqQueuedPattern = -1;
                debugPrintf("[TICK] t=%lu WAIT timeout + queued -> pat=%u\n", now, seqPattern);
            } else {
                debugPrintf("[TICK] t=%lu WAIT timeout pat=%u -> row=0\n", now, seqPattern);
            }
            // Reset bank/program/volume on every WAIT timeout so the block
            // restarts with all columns in their declared state — note-ons
            // never send patch changes, and the next OFF may be many rows
            // away (or absent entirely).
            seqSendColumnSetup(seqPattern);
            seqRow          = 0;
            seqWaiting      = false;
            seqEndNav       = 0;
            seqNextRowMs    = now;
            seqHaveLastSnap = false;
            seqReposition(0);
            if (seqRowCallback) seqRowCallback(seqPattern, seqRow);
        }
        // else: silently waiting — no spam
        return;
    }

    if (now < seqNextRowMs) return;

    // Check WAIT before playing — peek at the next node; if it is a col-0 WAIT
    // at the current row, halt until the performer resolves it.
    const Song*    s    = seqSong();
    const Pattern& pat  = s->patterns[seqPattern];
    if (seqNextNoteIdx != NOTE_NULL) {
        const NoteNode& peek = s->notePool[seqNextNoteIdx];
        if (peek.row == seqRow && peek.col == INPUT_COLUMN &&
            peek.note != NOTE_EMPTY && IS_WAIT_EFFECT(peek.effect)) {
            seqWaiting     = true;
            seqWaitStartMs = now;
            seqWaitTimeoutMs = (peek.effect == EFFECT_WAT1) ? 1000
                             : (peek.effect == EFFECT_WAT2) ? 2000
                                                            : 500;
            debugPrintf("[T] WAIT      t=%lu pat=%u row=%u eff=%02X to=%ums\n",
                        now, seqPattern, seqRow, peek.effect, (unsigned)seqWaitTimeoutMs);
            return;
        }
    }

    seqPlayRow();
    seqAbsRow++;

    seqRow++;
    if (seqRow >= pat.length) {
        uint8_t fromPattern = seqPattern;
        if (seqQueuedPattern >= 0) {
            // Performance mode override — use queued block
            seqPattern = (uint8_t)seqQueuedPattern;
            if (seqPattern >= s->numPatterns) seqPattern = 0;
            seqQueuedPattern = -1;  // one-shot
        } else {
            uint8_t nav = pat.blockEndNav;
            uint8_t mode = nav & NAV_MODE_MASK;
            uint8_t target = nav & NAV_TARGET_MASK;
            if (nav == NAV_RNT) {
                if (seqReturnFrom != 0xFF && seqReturnFrom < s->numPatterns)
                    seqPattern = seqReturnFrom;
                // else: behaves like LOOP (stay on this block)
            } else if (mode == NAV_FWD && target > 0) {
                seqPattern += target;
                if (seqPattern >= s->numPatterns) seqPattern = 0;
            } else if (mode == NAV_BACK && target > 0) {
                if (seqPattern >= target) seqPattern -= target;
                else seqPattern = s->numPatterns - 1;
            } else if (mode == NAV_ABS) {
                seqPattern = (target < s->numPatterns) ? target : 0;
            }
        }
        if (seqPattern != fromPattern) seqReturnFrom = fromPattern;
        seqRow = 0;
        seqSendColumnSetup(seqPattern);
        seqReposition(0);
    }

    if (seqRowCallback) seqRowCallback(seqPattern, seqRow);

    uint32_t rowMs = (uint32_t)s->speed * 2500UL / seqCurrentBPM;
    // Advance from this row's *scheduled* time, not from when seqPlayRow()
    // returned.  A dense row can back-pressure the MIDI FIFO and block the task
    // for a few ms; absorbing that into the interval keeps the row grid locked
    // to its tempo boundaries instead of drifting a little later every row.
    seqNextRowMs += rowMs;
    // Guard: if the task ever stalls for more than a whole row (pathological —
    // not the normal few-ms FIFO wait), resync rather than machine-gunning rows
    // back-to-back to catch up.
    uint32_t after = millis();
    if ((int32_t)(after - seqNextRowMs) > (int32_t)rowMs) seqNextRowMs = after;
}

// Both loop-task song walkers run under a busy flag.  The srvHasActive /
// seqRunning guards only protect walks that haven't started; a walk already
// in flight can be parked in a midiTx FIFO wait for several ms.  Cross-task
// writers (the MagiLink song-push receive) must wait for this flag to clear
// after dropping the guards, BEFORE overwriting srvActiveBuf — see
// onMagiLinkSaveHeader in commands_server.ino.
static volatile bool seqLoopWalkerBusy = false;

void sequencerTick() {
    seqLoopWalkerBusy = true;
    sequencerTickBody();
    seqLoopWalkerBusy = false;
}

void sequencerPollMidiIn() {
    seqLoopWalkerBusy = true;
    sequencerPollMidiInBody();
    seqLoopWalkerBusy = false;
}

bool sequencerSongReaderBusy() {
    return seqLoopWalkerBusy;
}

void seqSetRowCallback(void (*cb)(uint8_t pattern, uint8_t row)) {
    seqRowCallback = cb;
}

void seqSetStateCallback(void (*cb)(bool playing)) {
    seqStateCallback = cb;
}

void seqSetMidiNoteInCallback(void (*cb)(uint8_t midiNote, uint8_t velocity)) {
    seqMidiNoteInCallback = cb;
}

void seqSetMidiRawNoteCallback(void (*cb)(bool on, uint8_t midiNote, uint8_t velocity)) {
    seqRawNoteCallback = cb;
}

void seqSetPreviewRowCallback(void (*cb)(uint8_t row)) {
    seqPreviewRowCallback = cb;
}

void sequencerSetTestMode(uint32_t intervalMs) {
    if (intervalMs == 0) {
        seqTestMode = false;
        debugPrintf("[TEST] performer test mode OFF\n");
    } else {
        seqTestIntervalMs = intervalMs;
        seqTestNextMs     = millis() + intervalMs;
        seqTestMode       = true;
        debugPrintf("[TEST] performer test mode ON — C-4 every %lu ms\n", intervalMs);
    }
}

#ifdef UNIT_TEST
void     _test_onPerformerNote(uint8_t n) { seqOnPerformerNote(n); }
uint8_t  _test_getRow()                   { return seqRow; }
uint8_t  _test_getPattern()               { return seqPattern; }
bool     _test_isWaiting()                { return seqWaiting; }
bool     _test_isRunning()                { return seqRunning; }
uint32_t _test_getAbsRow()                { return seqAbsRow; }
uint8_t  _test_getLastSnapRow()           { return seqLastSnapRow; }
uint32_t _test_getNextRowMs()             { return seqNextRowMs; }
uint16_t _test_getBPM()                   { return seqCurrentBPM; }
void     _test_pushBpmHistory(uint16_t b) { seqPushBpmHistory(b); }
#endif
