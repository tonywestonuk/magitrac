// midi_player.cpp — sequencer + performer sync for M5Stack server

#include <Arduino.h>
#include "TrackerData.h"
#include "NoteGrid.h"
#include "MagiMsg.h"
#include "sample_record.h"
#include "midi_player.h"
#include "debug_log.h"
#include "hal/uart_ll.h"
#ifdef ARDUINO
#include "driver/uart.h"   // IDF UART driver — used only to configure the peripheral
#endif
#include "SamplePlayer.h"
#include "SampleManifest.h"
#include "SampleDebug.h"   // [SMPDBG] lines (off unless SAMPLE_DEBUG is 1)
#include "drawbar_organ.h"
#include "proc_sounds.h"   // PROC_SOUND_COUNT — organ-column program → voice map

// Direct UART1 hardware FIFO access — bypasses Arduino's software ring buffer
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

// Fumble window — for a short time after a performer key has taken effect, a
// further key is treated as part of the same fumble (a stray key caught
// alongside the intended one).  The rule is THE LAST KEY COUNTS: see
// seqOnPerformerNote.  The deadline is a millis() value compared wrap-safely.
static bool     seqFumbleOn    = false;
static uint32_t seqFumbleUntil = 0;
static uint8_t  seqFumblePc    = 0xFF;   // pitch class of the key currently standing

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

// Note sounding per output column.  seqActiveOn is the "something is sounding"
// flag — the note value itself can be 0 (a pitch clamped to MIDI note 0).
static bool     seqActiveOn[MAX_COLUMNS]   = {};
static uint8_t  seqActiveNote[MAX_COLUMNS] = {};
// Bumped every time a column's sounding note starts or stops — lets the fumble
// logic tell "the note this column had then" from "a note it has been given
// since", even when the pitch is the same.
static uint16_t seqActiveGen[MAX_COLUMNS]  = {};
// Per-column pixel_post "pressed" state. A NOTE row sets it on, NOTE_OFF
// turns it off, in-between rows (velocity-only / attr-only) ride the current
// state when emitting MSG_SLIDER / MSG_MOVE.
static bool     seqPpPressed[MAX_COLUMNS] = {};
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

static inline void seqSetActive(int col, uint8_t note, uint8_t chan) {
    seqActiveGen[col]++;
    seqActiveOn[col]   = true;
    seqActiveNote[col] = note;
    seqActiveChan[col] = chan;
}
// Per-channel bank/program/volume state — 0xFF = not set (force send on next note)
static uint8_t  seqChanBank[16];
static uint8_t  seqChanProg[16];
static uint8_t  seqChanVol[16];
// SFX columns: RAM voice handle when the column's sample plays from the PSRAM
// cache (polyphonic), -1 when it streams from SD (mono) or is silent.
static int seqSfxVoice[MAX_COLUMNS];
static bool _seqSfxVoiceInit = [](){
    for (int i = 0; i < MAX_COLUMNS; i++) seqSfxVoice[i] = -1;
    return true;
}();

// Forward declarations — defined below sequencerStart()
static void seqSendColumnSetup(uint8_t pattern, bool force = false);
static void seqStopImpl(bool deferOrganRelease = false);

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
extern uint8_t*       srvActiveBuf;    // PSRAM (commands_server allocates)
extern bool           srvHasActive;

// Last MIDI status byte written — enables running status on output.  Reset
// to 0 on sequencer stop (and after the SAM2695 SysEx burst / raw forwards)
// so the first message after the reset always sends its status.  Defined up
// here so the raw-forward + SysEx helpers below can clear it; original site
// was further down with the rest of the sequencer-state statics.
static uint8_t  seqOutStatus = 0;

// The MIDI-task song walkers (tick, MIDI-in poll, marshalled requests, the
// song-load patch flush) run under a busy flag.  The srvHasActive / seqRunning
// guards only protect walks that haven't started; a walk already in flight can
// be parked in a midiTx FIFO wait for several ms.  Cross-task writers (song
// load / push on the loop and MagiLink tasks) must wait for this flag to clear
// after dropping the guards, BEFORE overwriting srvActiveBuf — see
// quiesceSongReaders() and onMagiLinkSaveHeader() in commands_server.ino.
static volatile bool seqSongReaderBusy = false;

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
static void seqRawBytesImpl(const uint8_t* bytes, int n) {
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

// ── Untrusted-song guards ─────────────────────────────────────────────────────
// Song bytes come from SD files and raw client patches, and nothing validates
// them on the way in — so every field used as an index or a divisor is checked
// where it is used.  For a well-formed song none of these change anything.
static inline bool seqIsMidiChan(uint8_t c) { return c >= 1 && c <= 16; }

static inline uint8_t seqNumPat(const Song* s) {
    return s->numPatterns <= MAX_PATTERNS ? s->numPatterns : MAX_PATTERNS;
}

// One step of a note-list walk: false once idx is outside the pool or the walk
// has visited more nodes than the pool holds (a damaged link made it cyclic).
static inline bool seqIdxOk(uint16_t idx, uint16_t& steps) {
    return idx < MAX_SONG_NOTES && ++steps <= MAX_SONG_NOTES;
}

// ── Pan (EFFECT_PAN → MIDI CC 10) ─────────────────────────────────────────────
// Sent only where a PAN effect is written — the sequencer never sends a pan of
// its own accord (nothing on song load, start or stop).  What the pan is
// before the first PAN row is up to the synth: the preset selected by the
// column's program change, or whatever the channel was last left at.
static void seqSendPan(uint8_t ch0, uint8_t value) {
    if (value > 127) value = 127;
    seqWriteStatus((uint8_t)(0xB0 | ch0));   // CC 10 — Pan
    midiTx((uint8_t)10);
    midiTx(value);
}

static inline uint32_t seqRowMs(const Song* s) {   // speed 0 would be a row every ms
    uint8_t speed = s->speed > 0 ? s->speed : 6;
    return (uint32_t)speed * 2500UL / seqCurrentBPM;
}

// Clamp a tempo to the song's fumble-protection limits (20 / 400 if unset).
static inline uint32_t seqClampBpm(const Song* s, uint32_t bpm) {
    uint16_t minBPM = s->minBPM > 0 ? s->minBPM : 20;
    uint16_t maxBPM = s->maxBPM > 0 ? s->maxBPM : 400;
    if (bpm < minBPM) bpm = minBPM;
    if (bpm > maxBPM) bpm = maxBPM;
    return bpm;
}

// Cell velocity (0..127) → pixel_post slider (0..254).
static inline uint8_t seqPpSlider(uint8_t vel) {
    return (vel <= 127) ? (uint8_t)(vel * 2) : 255;
}

// ── MIDI clock out (24 PPQN) ──────────────────────────────────────────────────
// Emits 0xF8 timing clock at the live tempo while a clock-enabled song plays,
// bracketed by 0xFA (Start) / 0xFC (Stop) so external gear follows the
// performer.  Transport also tracks WAITs: a WAIT that TIMES OUT halts the
// clock (0xFC), and the next row to play "starts the block again" and
// re-Starts it (0xFA) — so a slaved drum machine drops out when the performer
// misses a cue and re-locks to their downbeat when they return.  A WAIT the
// performer resolves in time is seamless (clock never stops).
// micros()-based so the average rate is drift-free (integer 2500/bpm ms would
// run ~4% fast).  Real-time bytes don't affect running status → call midiTx
// directly and never clear seqOutStatus.  All on the MIDI task (Core 1) — the
// single midiTx producer, now that every cross-task entry point is marshalled
// onto it (see "Cross-task entry" at the end of this file).
static bool     seqClockActive = false;   // 0xFA sent, pulses running
static bool     seqClockHalted = false;   // stopped by a WAIT timeout; a row re-Starts
static uint32_t seqClockNextUs = 0;

static inline bool seqClockEnabled() {
    return srvHasActive && (seqSong()->flags & SONG_FLAG_MIDI_CLOCK);
}

// WAIT timed out (performer missed the cue): stop the external gear and arm a
// re-Start for whenever the block plays again.
static void seqClockOnWaitTimeout() {
    if (seqClockActive) { midiTx(0xFC); seqClockActive = false; }
    if (seqClockEnabled()) seqClockHalted = true;
}

// A row fired.  If a timeout had halted us, the block has started again →
// re-Start (0xFA).  Normal rows (not halted) are a cheap no-op.
static void seqClockOnRowPlayed() {
    if (seqClockHalted && seqClockEnabled()) {
        midiTx(0xFA);
        seqClockActive = true;
        seqClockHalted = false;
        seqClockNextUs = micros();
    }
}

void sequencerMidiClockTick() {
    if (!seqRunning || !seqClockEnabled()) {   // stopped / disabled → ensure Stop
        if (seqClockActive) { midiTx(0xFC); seqClockActive = false; }
        seqClockHalted = false;
        return;
    }
    if (!seqClockActive) {
        if (seqClockHalted) return;            // timed-out WAIT — await a row-play
        midiTx(0xFA);                          // song start, or enabled mid-play
        seqClockActive = true;
        seqClockNextUs = micros();
    }
    uint16_t bpm = seqCurrentBPM ? seqCurrentBPM : 120;
    uint32_t periodUs = 2500000UL / bpm;       // 60e6 / (bpm * 24) = one 1/24 note
    uint32_t nowUs = micros();
    if ((int32_t)(nowUs - seqClockNextUs) >= 0) {
        midiTx(0xF8);                          // Timing Clock
        seqClockNextUs += periodUs;
        // Fell more than a period behind (tempo jump / task stall)? resync to
        // now instead of bursting a run of catch-up pulses.
        if ((int32_t)(nowUs - seqClockNextUs) >= (int32_t)periodUs)
            seqClockNextUs = nowUs + periodUs;
    }
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
// newer note.  0xFF = none.  Written and read on the MIDI task (the audition
// request is marshalled there — see "Cross-task entry"); volatile covers the
// fallback where a stalled hand-off runs the request on the calling task.
static volatile uint8_t seqSfxAudNote = 0xFF;

// Preloaded samples make tune-by-ear POLYPHONIC: each held key owns a RAM
// voice, tracked here so its own note-off releases exactly that voice.
// (Streamed samples stay mono and use seqSfxAudNote above.)
#define SEQ_AUD_POLY 6
static struct { uint8_t note; int voice; } seqSfxAudPoly[SEQ_AUD_POLY] = {
    { 0, -1 }, { 0, -1 }, { 0, -1 }, { 0, -1 }, { 0, -1 }, { 0, -1 },
};

// Safe while a song runs: the poly table only ever holds voices a key started
// (and a voice handle can't stop a slot that has since been reused), and
// seqSfxAudNote is cleared the moment the song takes the SD stream for one of
// its own samples (sfxStreamPlay), so a late key-up can't cut song SFX.
static void seqSfxAudNoteOff(uint8_t note) {
    for (int i = 0; i < SEQ_AUD_POLY; i++) {
        if (seqSfxAudPoly[i].voice >= 0 && seqSfxAudPoly[i].note == note) {
            sampleStopVoice(seqSfxAudPoly[i].voice);
            seqSfxAudPoly[i].voice = -1;
            return;
        }
    }
    if (note == seqSfxAudNote) {
        seqSfxAudNote = 0xFF;
        samplePlayerStopStream();   // fades out, no click
    }
}

// Pitch (fractional semitones) for an SFX cell note: semitone tracking around
// C-4, plus the column's TRANSPOSE (whole semitones, same field as MIDI
// columns) and TUNE offset (cents from native — brings an arbitrarily-pitched
// recording in tune with the band).  All zero = native, the pre-existing
// behaviour.
// The sample's own TUNE (set in the sample editor, stored with its trim) is
// added on top, so a sample tuned once is in tune in every song that uses it.
static float sfxPitchFor(const ColumnSettings& cs, uint8_t note) {
    return (float)((int)note - SAMPLE_ROOT_NOTE + (int)cs.transpose)
         + (float)((int)cs.sfxTuneCents + (int)sampleTuneCentsFor(cs.program)) * 0.01f;
}

// Queue a sample on the (mono) SD stream at `semis` from native, with its
// saved trim/loop.  Returns false if the manifest has no such sample.
static bool sfxStreamPlayId(uint8_t sampleId, float semis, uint8_t volume) {
    const char* fname = sampleManifestNameFor(sampleId);
    if (!fname) return false;
    char path[64];
    snprintf(path, sizeof(path), "/samples/%s", fname);
    const SampleTrim* tr = sampleTrimFor(sampleId);
    samplePlayerPlay(path, semis, volume,
                     tr ? tr->startFrame : 0, tr ? tr->endFrame : 0,
                     tr && tr->loop);
    return true;
}

// Sounds started from the client's samples page — a square tapped (unkeyed),
// or a MIDI key playing the page's keyed sample.  They may play while a song
// runs, so they are tracked apart from the song's own: the page's STOP must
// silence only these, and a song column's note-off must not cut one that has
// since taken over the (single) SD stream.
#define SEQ_PAD_POLY 8
static int           seqPadVoice[SEQ_PAD_POLY] = { -1, -1, -1, -1, -1, -1, -1, -1 };   // squares' RAM voices
static uint8_t       seqPadNext       = 0;        // round-robin slot
static volatile bool seqPadOwnsStream = false;    // the SD stream is the page's, not the song's

static bool sfxStreamPlay(const ColumnSettings& cs, float semis) {
    if (!sfxStreamPlayId(cs.program, semis, cs.volume)) return false;
    seqPadOwnsStream = false;      // the song has the stream now…
    seqSfxAudNote    = 0xFF;       // …so no key's release may stop it
    return true;
}

// Start a sample outside the song (tune-by-ear, the client's sample page):
// preloaded → a RAM voice, else the SD stream.  `keyed` pairs the sound with
// MIDI key `note`, so that key's note-off (seqSfxAudNoteOff) stops it; an
// unkeyed sound (a pad tapped on the client) has no note-off and just plays.
static void sfxAudStart(uint8_t sampleId, float semis, uint8_t volume,
                        uint8_t note, bool keyed) {
    int h = samplePlayRam(sampleId, semis, volume);
#if SAMPLE_DEBUG
    debugPrintf("[SMPDBG] t=%lu play id=%u note=%u %s -> %s\n", (unsigned long)millis(),
                (unsigned)sampleId, (unsigned)note, keyed ? "key" : "pad",
                h >= 0 ? "RAM voice" : "SD stream");
#endif
    if (h >= 0) {
        if (!keyed) {              // remember it so the pads' STOP can find it
            seqPadVoice[seqPadNext] = h;
            seqPadNext = (uint8_t)((seqPadNext + 1) % SEQ_PAD_POLY);
            return;
        }
        // Polyphonic live playing: pair the voice with its key so the
        // note-off releases exactly this one.  Table full → oldest slot.
        int slot = 0;
        for (int i = 0; i < SEQ_AUD_POLY; i++)
            if (seqSfxAudPoly[i].voice < 0) { slot = i; break; }
        if (seqSfxAudPoly[slot].voice >= 0)          // table full: evict
            sampleStopVoice(seqSfxAudPoly[slot].voice);
        seqSfxAudPoly[slot].note  = note;
        seqSfxAudPoly[slot].voice = h;
        return;
    }
    if (!sfxStreamPlayId(sampleId, semis, volume)) return;
    // The stream is mono: this sound replaced whatever held it before — a
    // key, or (while a song runs) one of the song's streamed samples.
    seqSfxAudNote    = keyed ? note : 0xFF;
    seqPadOwnsStream = true;
}

// A key's velocity → the player's volume: velocity 100 is the sample at its own
// level (127); harder than that is louder — up to 161 at velocity 127, which is
// +4 dB and will clip a sample that is already recorded hot — and softer is
// quieter, on the squared taper (velocity 50 = a quarter of the level).
// A sample whose velocity sensitivity is switched off (sample editor, VEL OFF)
// always plays at its own level.
static inline uint8_t seqKeyVolume(uint8_t sampleId, uint8_t velocity) {
    if (sampleVelOffFor(sampleId)) return 127;
    return (uint8_t)(((int)velocity * 127 + 50) / 100);
}

// ── Sample page (client) — a sample played with no song column ───────────────
// seqKeysSampleId != 0 while the client has a sample "on the keys" — its
// sample editor is open, or a square on its samples page was held during a
// song: live MIDI keys play that sample here on the MIDI task (no wire round
// trip, chords work, no song needed).  It is a lease the client keeps
// renewing — see SAMPLE_EDIT_KEYS in MagiMsg.h.
static volatile uint8_t  seqKeysSampleId = 0;
static volatile uint32_t seqKeysStampMs  = 0;

uint8_t sequencerSampleKeysId() {
    uint8_t id = seqKeysSampleId;
    if (id && (uint32_t)(millis() - seqKeysStampMs) > SAMPLE_KEYS_LEASE_MS) {
        seqKeysSampleId = 0;
        id = 0;
    }
    return id;
}

void sequencerSampleKeys(uint8_t sampleId) {
    seqKeysStampMs  = millis();
    seqKeysSampleId = sampleId;
}

// Pitch: MIDI note 60 = the sample's native pitch + its own TUNE.  Loudness:
// `volume` in the player's terms (127 = the sample at its own level, squared
// taper).  A square is always 127 — there is no column to take a volume from;
// a key passes its velocity through seqKeyVolume().
//
// Both squares and keys play over a running song (a key is then ALSO the
// performer's cue — see the MIDI parser).  A sample in the PSRAM cache is just
// another RAM voice; any other takes the single SD stream, cutting a song
// sample that was streaming.
static void seqSamplePlayImpl(uint8_t sampleId, uint8_t note, bool keyed, uint8_t volume = 127) {
    if (sampleId == 0 || note > 127) return;
    if (organActive()) return;     // organ owns the codec
    float semis = (float)((int)note - 60)
                + (float)sampleTuneCentsFor(sampleId) * 0.01f;
    sfxAudStart(sampleId, semis, volume, note, keyed);
}

// The samples page's STOP.  Stopped: silence the whole sample player (also
// ends a looping audition).  Song running: only what the page started — its
// squares and its keyed notes.
static void seqSamplePadsStopImpl() {
    if (!seqRunning) {
        samplePlayerStop();
    } else {
        for (int i = 0; i < SEQ_PAD_POLY; i++) sampleStopVoice(seqPadVoice[i]);
        for (int i = 0; i < SEQ_AUD_POLY; i++) sampleStopVoice(seqSfxAudPoly[i].voice);
        if (seqPadOwnsStream) samplePlayerStopStream();
    }
    for (int i = 0; i < SEQ_PAD_POLY; i++) seqPadVoice[i] = -1;
    for (int i = 0; i < SEQ_AUD_POLY; i++) seqSfxAudPoly[i].voice = -1;
    seqSfxAudNote    = 0xFF;
    seqPadOwnsStream = false;
}

// Trigger a column's sample: preloaded → polyphonic RAM voice; not cached →
// SD stream (mono, replaces any running stream).  Records the RAM handle so
// the column's note-off stops ITS voice, not everyone's.  Returns false if
// the sample doesn't exist at all.
static bool sfxTrigger(int col, const ColumnSettings& cs, uint8_t note) {
    int h = samplePlayRam(cs.program, sfxPitchFor(cs, note), cs.volume);
    if (h >= 0) {
        seqSfxVoice[col] = h;
        return true;
    }
    if (!sfxStreamPlay(cs, sfxPitchFor(cs, note))) return false;
    seqSfxVoice[col] = -1;
    return true;
}

// ── Organ codec ownership ─────────────────────────────────────────────────────
// The organ synth and SamplePlayer can't both write the codec (no mixer), so
// the sequencer grabs the organ only when a song/preview/audition needs it and
// releases only what it grabbed — if the organ screen (or the client's organ
// page) already owns the synth, seqOrganGrab is a no-op and release leaves it
// alone.  While the organ is active, SFX columns stay silent.
static bool seqOrganOwned = false;
// The FFFF row-stop fires inside the row walk, where the ~60 ms organ/codec
// release must not run.  It parks the release here; sequencerRawAuditionTick
// performs it on the same MIDI-task pass, outside the tick.  A grab or a
// direct release in between supersedes it.
static bool seqOrganReleasePending = false;

static void seqOrganGrab() {
    seqOrganReleasePending = false;    // wanted again before the parked release ran
    if (organActive()) return;         // screen/page already owns it
    samplePlayerStop();                // codec must be idle before the DMA swap
    organSetActive(true);
    seqOrganOwned = true;
}

// Screen navigation asks before deactivating the organ on leaving SCR_ORGAN:
// if the sequencer owns it (song playing an ORGAN column), it must stay up.
bool sequencerOrganOwned() { return seqOrganOwned; }

static void seqOrganRelease() {
    seqOrganReleasePending = false;
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
    if (!seqActiveOn[col]) return;
    if (seqActiveChan[col] == SEQ_CHAN_SFX) {
        if (seqSfxVoice[col] >= 0) {
            sampleStopVoice(seqSfxVoice[col]);   // this column's RAM voice only
            seqSfxVoice[col] = -1;
        } else if (!seqPadOwnsStream) {
            samplePlayerStopStream();            // it was the (single) SD stream;
        }                                        // other columns' RAM voices ring on
                                                 // (a pad that took the stream
                                                 // since is not this column's)
    } else if (seqActiveChan[col] == SEQ_CHAN_ORGAN) {
        organNoteOff(seqActiveNote[col]);
    } else {
        uint8_t note = seqActiveNote[col];
        seqWriteStatus((uint8_t)(0x80 | seqActiveChan[col]));
        midiTx(note);
        midiTx((uint8_t)0);
    }
    seqActiveGen[col]++;
    seqActiveOn[col]   = false;
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
        // Overdue by more than one interval means the MIDI task stalled past
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
        seqSetActive(col, note, ch);
        seqRetrigLeft[col]--;
        seqRetrigNextMs[col] += seqRetrigIntervalMs[col];
    }
}

// Scan the active pattern's circular list from its head and position
// seqNextNoteIdx at the first node with row >= targetRow.
// Call whenever seqRow or seqPattern changes discontinuously.
static void seqReposition(uint8_t targetRow) {
    // No song buffer at all (failed PSRAM alloc): sequencerReset() still
    // lands here from a client STOP, so don't dereference it.
    if (!srvActiveBuf) { seqNextNoteIdx = NOTE_NULL; return; }
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

    seqClockOnRowPlayed();      // re-Start MIDI clock if a WAIT timeout halted it
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
        if (nn.col >= MAX_COLUMNS) continue;            // corrupt cell — skip it

        // Universal output-column effect 0xFF param 0xFF = "STOP".  Halts
        // playback and sends note-off on every active column.  Excludes
        // SFX/pixel-post columns (pixel-post uses effect/param for MOVE x/y).
        if (nn.col != INPUT_COLUMN && nn.effect == 0xFF && nn.param == 0xFF) {
            const ColumnSettings& csF = s->columns[nn.col];
            if (csF.midiChannel >= 1 && csF.midiChannel <= 16) {
                debugPrintf("[SEQ] FFFF stop pat=%u row=%u col=%u\n",
                            seqPattern, seqRow, nn.col);
                // Already on the MIDI task (row walks only run there): stop
                // inline, but leave the organ/codec release to the task loop.
                seqStopImpl(/*deferOrganRelease=*/true);
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
                uint16_t newBpm  = (uint16_t)seqClampBpm(s, nn.param);
                seqCurrentBPM    = newBpm;
                seqA0RowOverride = true;
                seqA0RowBpm      = newBpm;
                debugPrintf("[SEQ] A0 set BPM pat=%u row=%u col=%u -> bpm=%u\n",
                            seqPattern, seqRow, nn.col, seqCurrentBPM);
            }
        }

        // Output-column effect 0x0A = "pan".  Param is the MIDI pan position
        // (CC 10): 00 hard left, 40 centre, 7F hard right.  Handled here, ahead
        // of the note below, so a note on the same row sounds at the new
        // position; it also works on a row with no note.  MIDI columns only
        // (SFX / pixel-post / organ columns use effect+param for their own
        // purposes), and a muted column sends nothing.
        if (nn.col != INPUT_COLUMN && nn.effect == EFFECT_PAN) {
            const ColumnSettings& csP = s->columns[nn.col];
            if (seqIsMidiChan(csP.midiChannel) && !csP.mute)
                seqSendPan((uint8_t)(csP.midiChannel - 1), nn.param);
        }

        if (nn.col == INPUT_COLUMN) {
            // WAIT/SYNC effects are handled by tick/performer logic, not here.
            // AVRG: when the playhead reaches this row, override seqCurrentBPM
            // with the running mean of the last 4 performer-derived BPMs.
            // Lets a new block enter at the tempo the performer was already at.
            if (nn.effect == EFFECT_AVRG) {
                uint16_t avg = seqAverageBpmHistory();
                if (avg > 0) {
                    seqCurrentBPM = (uint16_t)seqClampBpm(s, avg);
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
                pixelpostSetSlider(seqPpSlider(vel));
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
            if (seqIsMidiChan(cs.midiChannel) && !cs.mute) {
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
                if (sfxTrigger(col, cs, nn.note)) {
                    seqSetActive(col, 1, SEQ_CHAN_SFX);   // route stop to SamplePlayer
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
                seqSetActive(col, (uint8_t)raw, SEQ_CHAN_ORGAN);   // route stop to the organ
                continue;
            }

            if (cs.midiChannel == PIXELPOST_CHANNEL) {
                extern bool pixelpostManualActive();
                if (pixelpostManualActive()) continue;   // perf-page override owns the lights
                // PIXELPOST: a real note means "finger down at this XY with
                // this slider value, on this effect".  Effect index = note - 1
                // (so C-0 = 0, C#0 = 1, ...).  No per-column cache here:
                // every note calls pixelpostSetEffect(), which dedups against
                // the single global effect id and broadcasts only on a change.
                // Skip non-pitched notes (NOTE_ANY etc.); they have no
                // sensible effect mapping.
                if (nn.note < 1 || nn.note > NOTE_MAX) continue;
                extern void pixelpostSetEffect(uint8_t idx);
                extern void pixelpostSetSlider(uint8_t value);
                extern void pixelpostSetTouchpad(uint8_t x, uint8_t y, bool touched);
                seqPpPressed[col] = true;
                uint8_t effectIdx = (uint8_t)(nn.note - 1);
                pixelpostSetEffect(effectIdx);
                uint8_t vel = (nn.velocity & 0x80) ? 100 : nn.velocity;
                pixelpostSetSlider(seqPpSlider(vel));
                pixelpostSetTouchpad(nn.effect, nn.param, /*touched=*/true);
                continue;
            }

            if (!seqIsMidiChan(cs.midiChannel)) continue;
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
            seqSetActive(col, midiNote, midiCh);

            // Retrigger: subdivide the row into `hits` evenly spaced
            // re-articulations.  Interval is anchored to the current row
            // duration at trigger time; later tempo changes don't reshape an
            // in-flight roll.  seqNoteOff (next note/OFF) cancels it.
            if (nn.effect == EFFECT_RTRG) {
                uint8_t  hits  = retrigHits(nn.param);
                uint32_t interval = seqRowMs(s) / hits;
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
                if (!(nn.velocity & 0x80)) pixelpostSetSlider(seqPpSlider(nn.velocity));
                if (nn.effect != 0 || nn.param != 0) {
                    pixelpostSetTouchpad(nn.effect, nn.param, pressed);
                }
            }
        }
    }
}

// Resolve a pattern's blockEndNav byte into seqPattern (RNT / forward /
// back / absolute; anything else, incl. LOOP, stays on the current block).
static void seqApplyBlockEndNav(const Song* s, uint8_t nav) {
    uint8_t mode   = nav & NAV_MODE_MASK;
    uint8_t target = nav & NAV_TARGET_MASK;
    if (nav == NAV_RNT) {
        if (seqReturnFrom != 0xFF && seqReturnFrom < seqNumPat(s))
            seqPattern = seqReturnFrom;
        // else: behaves like LOOP (stay on this block)
    } else if (mode == NAV_FWD && target > 0) {
        seqPattern += target;
        if (seqPattern >= seqNumPat(s)) seqPattern = 0;
    } else if (mode == NAV_BACK && target > 0) {
        if (seqPattern >= target) seqPattern -= target;
        else seqPattern = seqNumPat(s) ? seqNumPat(s) - 1 : 0;
    } else if (mode == NAV_ABS) {
        seqPattern = (target < seqNumPat(s)) ? target : 0;
    }
}

// The current block has run out of rows: choose the next one (a queued block
// wins once, otherwise the block's own end-of-block navigation), start it at
// row 0, bring its patches up and put the note cursor on its first note.
// Shared by the tick and the performer-snap path so both end a block the same
// way.
static void seqEnterNextBlock(const Song* s) {
    uint8_t fromPattern = seqPattern;
    uint8_t nav         = s->patterns[seqPattern].blockEndNav;
    if (seqQueuedPattern >= 0) {
        // Performance mode override — use queued block
        seqPattern = (uint8_t)seqQueuedPattern;
        if (seqPattern >= seqNumPat(s)) seqPattern = 0;
        seqQueuedPattern = -1;  // one-shot
    } else {
        seqApplyBlockEndNav(s, nav);
    }
    if (seqPattern != fromPattern) seqReturnFrom = fromPattern;
    seqRow = 0;
    seqSendColumnSetup(seqPattern);
    seqReposition(0);
}

// ── BPM derivation ────────────────────────────────────────────────────────────
static void seqRecordSnap(uint32_t now) {
    if (seqHaveLastSnap) {
        uint32_t deltaMs   = now - seqLastSnapMs;
        uint32_t deltaRows = seqAbsRow - seqLastSnapAbsRow;

        if (deltaRows > 0 && deltaMs > 50) {  // ignore jitter / double-hits
            const Song* s = seqSong();
            uint32_t derived = ((uint32_t)s->speed * 2500UL * deltaRows) / deltaMs;

            seqCurrentBPM = (uint16_t)seqClampBpm(s, derived);
            seqPushBpmHistory(seqCurrentBPM);
        }
    }

    seqLastSnapMs      = now;
    seqLastSnapAbsRow  = seqAbsRow;
    seqHaveLastSnap    = true;
}

// ── Performer note handler ────────────────────────────────────────────────────
// Everything a performer key can change about WHERE the sequencer is and how
// fast it is going — saved before a key is acted on so that, if a second key
// follows inside the fumble window, the first can be taken back.  (What has
// already been sent to the synth cannot be; sounding notes are dealt with by
// the caller.  gen[] is a record for comparison, never restored.)
struct SeqPos {
    uint8_t  pattern, row;
    uint32_t nextRowMs;
    bool     waiting;
    uint32_t waitStartMs, waitTimeoutMs;
    int8_t   transpose;
    uint16_t currentBPM;
    bool     a0RowOverride;
    uint16_t a0RowBpm;
    int8_t   queuedPattern;
    uint8_t  returnFrom;
    uint32_t absRow, lastSnapMs, lastSnapAbsRow;
    bool     haveLastSnap;
    uint16_t bpmHistory[SEQ_BPM_HISTORY_SIZE];
    uint8_t  bpmHistoryHead, bpmHistoryCount;
    uint8_t  lastPitchClass, noiseConsumedRow, lastSnapRow, lastSeenPat, lastSeenRow;
    uint16_t nextNoteIdx;
    uint16_t gen[MAX_COLUMNS];
};

static void seqPosSave(SeqPos& p) {
    p.pattern = seqPattern;            p.row = seqRow;
    p.nextRowMs = seqNextRowMs;        p.waiting = seqWaiting;
    p.waitStartMs = seqWaitStartMs;    p.waitTimeoutMs = seqWaitTimeoutMs;
    p.transpose = seqTranspose;        p.currentBPM = seqCurrentBPM;
    p.a0RowOverride = seqA0RowOverride; p.a0RowBpm = seqA0RowBpm;
    p.queuedPattern = seqQueuedPattern; p.returnFrom = seqReturnFrom;
    p.absRow = seqAbsRow;              p.lastSnapMs = seqLastSnapMs;
    p.lastSnapAbsRow = seqLastSnapAbsRow; p.haveLastSnap = seqHaveLastSnap;
    memcpy(p.bpmHistory, seqBpmHistory, sizeof(p.bpmHistory));
    p.bpmHistoryHead = seqBpmHistoryHead; p.bpmHistoryCount = seqBpmHistoryCount;
    p.lastPitchClass = seqLastPitchClass; p.noiseConsumedRow = seqNoiseConsumedRow;
    p.lastSnapRow = seqLastSnapRow;    p.lastSeenPat = seqLastSeenPat;
    p.lastSeenRow = seqLastSeenRow;    p.nextNoteIdx = seqNextNoteIdx;
    memcpy(p.gen, seqActiveGen, sizeof(p.gen));
}

static void seqPosRestore(const SeqPos& p) {
    seqPattern = p.pattern;            seqRow = p.row;
    seqNextRowMs = p.nextRowMs;        seqWaiting = p.waiting;
    seqWaitStartMs = p.waitStartMs;    seqWaitTimeoutMs = p.waitTimeoutMs;
    seqTranspose = p.transpose;        seqCurrentBPM = p.currentBPM;
    seqA0RowOverride = p.a0RowOverride; seqA0RowBpm = p.a0RowBpm;
    seqQueuedPattern = p.queuedPattern; seqReturnFrom = p.returnFrom;
    seqAbsRow = p.absRow;              seqLastSnapMs = p.lastSnapMs;
    seqLastSnapAbsRow = p.lastSnapAbsRow; seqHaveLastSnap = p.haveLastSnap;
    memcpy(seqBpmHistory, p.bpmHistory, sizeof(p.bpmHistory));
    seqBpmHistoryHead = p.bpmHistoryHead; seqBpmHistoryCount = p.bpmHistoryCount;
    seqLastPitchClass = p.lastPitchClass; seqNoiseConsumedRow = p.noiseConsumedRow;
    seqLastSnapRow = p.lastSnapRow;    seqLastSeenPat = p.lastSeenPat;
    seqLastSeenRow = p.lastSeenRow;    seqNextNoteIdx = p.nextNoteIdx;
}

// What a key amounts to: which block it leads to, which cue row it plays (0xFF
// = none, 0xFE = a late SYNC hit that only stretches the row), the row the
// playhead is left on, and the transposition.  Two keys with the same decision
// would do the same thing.
struct SeqDecision { uint8_t pattern, cueRow, row; int8_t transpose; };
static inline bool seqSameDecision(const SeqDecision& a, const SeqDecision& b) {
    return a.pattern == b.pattern && a.cueRow == b.cueRow &&
           a.row == b.row && a.transpose == b.transpose;
}
enum { SEQ_NOTE_NONE = 0,   // the key means nothing here (no switch, no cue)
       SEQ_NOTE_DID  = 1,   // it took effect
       SEQ_NOTE_SAME = 2 }; // it would do exactly what `sameAs` already did — nothing was played

// Act on one performer key from the CURRENT state.  No fumble logic in here:
// that is seqOnPerformerNote's job.  With `sameAs` given, the key is dropped
// (SEQ_NOTE_SAME) at the point of decision — before any row is played — if it
// would only repeat that decision.
static uint8_t seqPerformerNoteCore(uint8_t midiNote, uint32_t now,
                                    const SeqDecision* sameAs, SeqDecision* decided) {
    bool    did        = false;   // the playhead was moved (block switch / key-change jump)
    uint8_t pitchClass = midiNote % 12;
    const Song* s      = seqSong();
    if (seqPattern >= seqNumPat(s)) return SEQ_NOTE_NONE;   // stale index from prior song
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
        if (target < seqNumPat(s)) {
            did = true;
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
            uint8_t fromPattern = seqPattern;
            seqApplyBlockEndNav(s, kPat.blockEndNav);
            if (seqPattern != fromPattern) seqReturnFrom = fromPattern;
            did                  = true;
            seqRow               = 0;
            seqWaiting           = false;
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
    uint8_t cueRow = 0xFF;
    bool    skipBpmUpdate = false;   // set for SYNC snaps (tempo is performer-locked elsewhere)
    if (seqWaiting) {
        // Halted — seqNextNoteIdx is parked at the WAIT node (tick peeked but didn't consume).
        // Resolve if pitch class matches, or if NOTE_ANY (match any input).
        if (seqNextNoteIdx < MAX_SONG_NOTES) {      // also excludes NOTE_NULL
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
            uint16_t idx = head, steps = 0;
            do {
                if (!seqIdxOk(idx, steps)) break;
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
            uint16_t idx = startIdx, steps = 0;
            do {
                if (!seqIdxOk(idx, steps)) break;
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
                                SeqDecision d = { seqPattern, 0xFE, seqRow, seqTranspose };
                                if (decided) *decided = d;
                                if (sameAs && seqSameDecision(d, *sameAs)) return SEQ_NOTE_SAME;
                                seqNextRowMs = now + seqRowMs(s);
                                return SEQ_NOTE_DID;
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
                            if (decided) *decided = { seqPattern, 0xFF, seqRow, seqTranspose };
                            return did ? SEQ_NOTE_DID : SEQ_NOTE_NONE;   // absorbed
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
                            if (decided) *decided = { seqPattern, 0xFF, seqRow, seqTranspose };
                            return did ? SEQ_NOTE_DID : SEQ_NOTE_NONE;   // absorbed
                        }
                    }
                    // else: passed / consumed / non-matching marker — keep walking
                }
                idx = s->notePool[idx].next;
            } while (idx != startIdx);
        }
    }

    // The decision is made; nothing has been played for this key yet.
    {
        SeqDecision d = { seqPattern, cueRow,
                          (uint8_t)(cueRow == 0xFF ? seqRow : cueRow), seqTranspose };
        if (decided) *decided = d;
        if (cueRow == 0xFF && !did) return SEQ_NOTE_NONE;
        if (sameAs && seqSameDecision(d, *sameAs)) return SEQ_NOTE_SAME;
        if (cueRow == 0xFF) return SEQ_NOTE_DID;      // switched block, no cue to play
    }

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
    uint32_t rowMs = seqRowMs(s);

    // Schedule and log AFTER MIDI is out
    seqAbsRow++;
    // The cue row has played; the next row is the one after it.  If the cue
    // was the LAST row of the block, the block is over: do exactly what the
    // tick does at a block end (queued block / block-end nav, patches, cursor
    // back to the first note).  Just wrapping the row number left the note
    // cursor at the end of the list, and the whole next pass played nothing.
    if ((uint8_t)(cueRow + 1) >= s->patterns[seqPattern].length) seqEnterNextBlock(s);
    else                                                         seqRow = (uint8_t)(cueRow + 1);
    seqWaiting           = false;
    seqNextRowMs         = afterPlay + rowMs;
    if (seqRowCallback) seqRowCallback(seqPattern, seqRow);
    return SEQ_NOTE_DID;
}

// A performer key.  The fumble rule — THE LAST KEY COUNTS:
//
// When a key takes effect it opens a short window (half a row).  A different
// key inside that window is taken to be what the performer actually meant, and
// the earlier one a slip: the sequencer is put back to where it was before the
// earlier key and the new key is acted on from there.  So in a block where G
// plays on and A♭ leads to the next block:
//   A♭ then G  → back in this block, G's row plays      (A♭ is taken back)
//   G then A♭  → the next block starts                  (G's row is cut short)
// What the earlier key already sounded cannot be unsent, so any note it started
// that the new key's row does not replace is silenced.
//
// Three cases leave the earlier key standing, with nothing played again:
//   • the same key again (a bounce, or its octave);
//   • a key that would do exactly the same thing (another note of a chord on
//     an "any note" cue);
//   • a key that means nothing where the earlier key was pressed (a brushed
//     wrong note).
static bool        seqFumbleHaveBase = false;
static SeqPos      seqFumbleBase;        // state before the first key of the fumble
static SeqDecision seqFumbleDecision;    // what the key now standing amounted to

static void seqOnPerformerNote(uint8_t midiNote) {
    if (seqPreview) return;
    if (!srvHasActive || !seqRunning) return;
    uint32_t now = millis();
    uint8_t  pc  = midiNote % 12;

    if (seqFumbleOn && seqFumbleHaveBase && (int32_t)(now - seqFumbleUntil) < 0) {
        if (pc == seqFumblePc) return;                       // same key again
        SeqPos after;
        seqPosSave(after);                                   // where the earlier key left us
        seqPosRestore(seqFumbleBase);                        // …as if it had not been pressed
        SeqDecision d;
        if (seqPerformerNoteCore(midiNote, now, &seqFumbleDecision, &d) != SEQ_NOTE_DID) {
            seqPosRestore(after);                            // earlier key stands
            return;
        }
        // The new key has taken over.  Silence whatever the earlier key (or a
        // row the tick played after it) started and this key did not replace.
        for (int col = 0; col < MAX_COLUMNS; col++) {
            if (seqActiveOn[col] && seqActiveGen[col] == after.gen[col] &&
                after.gen[col] != seqFumbleBase.gen[col])
                seqNoteOff(col);
        }
        seqFumblePc       = pc;
        seqFumbleDecision = d;
        seqFumbleUntil    = millis() + seqRowMs(seqSong()) / 2;   // base stays: a third key is judged from the same start
        return;
    }

    SeqPos base;
    seqPosSave(base);
    SeqDecision d;
    if (seqPerformerNoteCore(midiNote, now, nullptr, &d) == SEQ_NOTE_DID) {
        seqFumbleBase     = base;
        seqFumbleHaveBase = true;
        seqFumbleDecision = d;
        seqFumblePc       = pc;
        seqFumbleOn       = true;
        seqFumbleUntil    = millis() + seqRowMs(seqSong()) / 2;
    }
}

// ── MIDI input polling ────────────────────────────────────────────────────────
// Note-ons softer than this are dropped from both performer-sync and
// step-entry (the raw-note monitor and SFX-audition note-off still see them).
static const uint8_t SEQ_MIN_PERFORMER_VEL = 20;

static void sequencerPollMidiInBody() {
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
                // ADD SAMPLE's review screen is up: the keys play the new take.
                if (sampleRecKeysNote(midiByte1, velocity)) continue;
                // The client has a sample on the keys (its editor is open, or a
                // square was held during a song): the key plays it, on any
                // channel at any velocity.  Stopped, that is all the key does
                // — it is not step input.  With a song running it is ALSO the
                // performer's cue, as with the organ.
                uint8_t keysId = sequencerSampleKeysId();
                if (keysId && velocity > 0)
                    seqSamplePlayImpl(keysId, midiByte1, true, seqKeyVolume(keysId, velocity));
                if (keysId && !seqRunning) {
                    // (the sample's key only)
                } else if (velocity >= SEQ_MIN_PERFORMER_VEL && srvActiveBuf) {
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
                if (sampleRecKeysNote(midiByte1, 0)) continue;   // ADD SAMPLE's take
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
// Called at sequencer start/resume, every block transition, WAIT timeout and
// (forced) on song load.  Skips channels that are already configured with the
// correct bank/program/volume, and skips channels already touched this pass
// (first column wins when two columns share a channel but have different
// bank/program — mid-block, only a NOTE_OFF cell on the other column switches
// the channel over; note-ons never send patch changes).
static void seqSendColumnSetup(uint8_t pattern, bool force) {
    const Song* s = seqSong();
    if (pattern >= seqNumPat(s)) return;

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
        if (!seqIsMidiChan(cs.midiChannel)) continue;       // not a MIDI column
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
    if (seqPreviewPat >= seqNumPat(s)) return;
    const ColumnSettings& cs = s->columns[seqPreviewCol];
    if (cs.midiChannel == 0 || cs.mute) return;

    uint16_t head = s->patterns[seqPreviewPat].noteHead;
    if (head == NOTE_NULL) return;

    uint16_t idx = head, steps = 0;
    do {
        if (!seqIdxOk(idx, steps)) break;
        const NoteNode& nn = s->notePool[idx];
        if (nn.row == seqPreviewRow && nn.col == seqPreviewCol) {
            if (nn.note == NOTE_OFF) {
                seqNoteOff(seqPreviewCol);
            } else if (nn.note != NOTE_EMPTY && nn.note != NOTE_ANY) {
                seqNoteOff(seqPreviewCol);
                if (cs.midiChannel == SFX_CHANNEL) {
                    if (organActive()) return;   // organ owns the codec — SFX silent
                    if (sfxTrigger(seqPreviewCol, cs, nn.note)) {
                        seqSetActive(seqPreviewCol, 1, SEQ_CHAN_SFX);
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
                    seqSetActive(seqPreviewCol, (uint8_t)raw, SEQ_CHAN_ORGAN);
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
                    uint8_t vel = (nn.velocity & 0x80) ? 100 : nn.velocity;
                    pixelpostSetSlider(seqPpSlider(vel));
                    pixelpostSetTouchpad(nn.effect, nn.param, /*touched=*/true);
                    return;
                }
                if (!seqIsMidiChan(cs.midiChannel)) return;
                uint8_t midiCh = (uint8_t)(cs.midiChannel - 1);
                int raw = (int)nn.note + TRACKER_TO_MIDI_OFFSET + (int)cs.transpose;
                if (raw < 0)   raw = 0;
                if (raw > 127) raw = 127;
                uint8_t midiNote = (uint8_t)raw;
                uint8_t noteVel  = (nn.velocity & 0x80) ? 100 : nn.velocity;
                seqWriteStatus((uint8_t)(0x90 | midiCh));
                midiTx(midiNote);
                midiTx(noteVel);
                seqSetActive(seqPreviewCol, midiNote, midiCh);
            }
            return;
        }
        idx = nn.next;
    } while (idx != head);
}

// ── Public API ────────────────────────────────────────────────────────────────
static void seqStartImpl() {
    if (!srvHasActive) return;
    if (seqPreview) sequencerStopPreview();

    for (int c = 0; c < MAX_COLUMNS; c++) seqNoteOff(c);
    memset(seqActiveChan,   0,    sizeof(seqActiveChan));
    memset(seqChanBank,     0xFF, sizeof(seqChanBank));  // 0xFF = force first send
    memset(seqChanProg,     0xFF, sizeof(seqChanProg));
    memset(seqChanVol,      0xFF, sizeof(seqChanVol));
    memset(seqPpPressed,    0,    sizeof(seqPpPressed));

    const Song* s = seqSong();
    uint8_t startPat = s->startPattern < seqNumPat(s) ? s->startPattern : 0;

    // Organ codec ownership follows the song (~60 ms DMA swap, so at start
    // only): grab if any unmuted organ column, release ours otherwise.
    if (seqSongUsesOrgan()) seqOrganGrab();
    else                    seqOrganRelease();

    seqSendColumnSetup(startPat, /*force=*/true);

    seqSendSlotEnable();

    seqPattern       = startPat;   // Song::startPattern — setup above matches
    seqRow           = 0;
    seqWaiting       = false;
    seqQueuedPattern = -1;
    seqReturnFrom    = 0xFF;
    seqTranspose     = 0;
    seqCurrentBPM   = seqSong()->bpm > 0 ? seqSong()->bpm : 100;
    seqAbsRow            = 0;
    seqHaveLastSnap      = false;
    seqBpmHistoryHead    = 0;
    seqBpmHistoryCount   = 0;
    seqFumbleOn          = false;
    seqLastPitchClass    = 0xFF;
    seqRunning           = true;
    seqNextRowMs         = millis();
    seqReposition(0);

    debugPrintf("[SEQ] start t=%lu bpm=%u speed=%u\n",
                  millis(), seqCurrentBPM, seqSong()->speed);
    if (seqStateCallback) seqStateCallback(true);
}

static void seqResumeImpl() {
    seqFumbleOn = false;   // the saved "before the key" position is no longer valid
    if (!srvHasActive) return;
    if (seqPreview) sequencerStopPreview();
    // Clamp before any patterns[seqPattern] deref — see sequencerReset() for
    // the rationale; a bare MSG_PLAY (without a preceding seek) can land here
    // with seqPattern left over from a different song.
    const Song* s0 = seqSong();
    if (s0 && seqPattern >= seqNumPat(s0)) {
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

static void seqStopImpl(bool deferOrganRelease) {
    seqFumbleOn = false;   // the saved "before the key" position is no longer valid
    seqRunning        = false;
    seqPaused         = false;
    seqWaiting        = false;
    seqQueuedPattern  = -1;    // clear any performance mode queue
    seqLastPitchClass = 0xFF;
    for (int c = 0; c < MAX_COLUMNS; c++) seqNoteOff(c);
    if (deferOrganRelease) seqOrganReleasePending = seqOrganOwned;
    else                   seqOrganRelease();
    seqOutStatus = 0;  // force full status byte on next start
    debugPrintf("[SEQ] stop t=%lu\n", millis());
    if (seqStateCallback) seqStateCallback(false);
}

static void seqStartPreviewImpl(uint8_t pat, uint8_t col) {
    if (!srvHasActive) return;
    const Song* s = seqSong();
    if (pat >= seqNumPat(s))  return;
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

static void seqStopPreviewImpl() {
    if (!seqPreview) return;
    seqPreview = false;
    seqNoteOff(seqPreviewCol);
    seqOrganRelease();
    seqOutStatus = 0;
    debugPrintf("[SEQ] preview stop\n");
}

static void seqAuditionNoteImpl(uint8_t pat, uint8_t row, uint8_t col) {
    if (!srvHasActive)              return;
    if (seqRunning)                 return;   // song would mask anyway, skip
    if (seqPreview)                 return;   // preview is already auditioning
    if (col == 0 || col >= MAX_COLUMNS) return;

    const Song* s = seqSong();
    if (pat >= seqNumPat(s))      return;

    const ColumnSettings& cs = s->columns[col];
    if (cs.midiChannel == 0 || cs.mute) return;

    // Walk the pattern's note list looking for the cell at (row, col).
    uint16_t head = s->patterns[pat].noteHead;
    if (head == NOTE_NULL)          return;
    Note nn = { NOTE_EMPTY, 0, 0, 0 };
    {
        uint16_t idx = head, steps = 0;
        do {
            if (!seqIdxOk(idx, steps)) break;
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
        if (sfxTrigger(col, cs, nn.note)) {
            seqSetActive(col, 1, SEQ_CHAN_SFX);
            seqAuditionCol   = col;
            seqAuditionEndMs = millis() + 500;
            debugPrintf("[SEQ] audition col=%u SFX id=%u\n", col, cs.program);
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
        seqSetActive(col, (uint8_t)raw, SEQ_CHAN_ORGAN);
        seqAuditionCol   = col;
        seqAuditionEndMs = millis() + 500;
        debugPrintf("[SEQ] audition col=%u ORGAN note=%d\n", col, raw);
        return;
    }

    if (!seqIsMidiChan(cs.midiChannel)) return;        // PIXELPOST / unset: nothing to audition

    uint8_t midiCh = (uint8_t)(cs.midiChannel - 1);
    int raw = (int)nn.note + TRACKER_TO_MIDI_OFFSET + (int)cs.transpose;
    if (raw < 0)   raw = 0;
    if (raw > 127) raw = 127;
    uint8_t midiNote = (uint8_t)raw;
    uint8_t noteVel  = (nn.velocity & 0x80) ? 100 : nn.velocity;
    seqWriteStatus((uint8_t)(0x90 | midiCh));
    midiTx(midiNote);
    midiTx(noteVel);
    seqSetActive(col, midiNote, midiCh);

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

// Pending "flush every column's patch" request — set on song load by the
// command/loop task, emitted on the MIDI task for the same running-status
// reason as the audition program change above.
static volatile bool seqColSetupPending = false;

static void seqAuditionRawNoteImpl(uint8_t channel, uint8_t note, uint8_t velocity,
                                   uint8_t col) {
    // SFX: play column `col`'s sample at the raw MIDI pitch (60 = native +
    // TUNE) — the client's tune-by-ear path while the sequencer is stopped.
    // Fire-and-forget straight to the SamplePlayer queue (no SD, no MIDI
    // running-status involvement), so no need for the raw-audition hold queue.
    if (channel == SFX_CHANNEL) {
        if (seqRunning) return;   // tune-by-ear is a stopped-state feature: a
                                  // late/in-flight audition must never replace
                                  // a song-triggered sample
        if (col >= MAX_COLUMNS || note > 127) return;
        if (!srvHasActive) return;             // no song → no column to read
        if (organActive()) return;             // organ owns the codec
        const ColumnSettings& cs = seqSong()->columns[col];
        float semis = (float)((int)note - 60 + (int)cs.transpose)
                    + (float)((int)cs.sfxTuneCents
                              + (int)sampleTuneCentsFor(cs.program)) * 0.01f;
        // Keyed: the local MIDI parser stops it on that key's note-off.
        sfxAudStart(cs.program, semis, cs.volume, note, true);
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

void sequencerSendPatchesNow() {
    seqColSetupPending = true;
}

void sequencerRawAuditionTick() {
    // Organ release parked by an FFFF row-stop (see seqOrganReleasePending).
    if (seqOrganReleasePending) seqOrganRelease();

    // Song just loaded — put every channel on its column's bank/program/volume
    // now, so the performer's first notes (or a PLAY straight into a block)
    // sound on the right patches instead of whatever the previous song left.
    // Skipped while running or previewing: those paths own the cache and are
    // already sending their own setup at block boundaries.  Runs on the MIDI
    // task — same task seqSendColumnSetup already runs on at every block
    // transition, so the ORGAN-column patch apply it performs is nothing new.
    if (seqColSetupPending) {
        seqColSetupPending = false;
        seqSongReaderBusy = true;    // set BEFORE the srvHasActive check, as in the tick
        if (srvHasActive && !seqRunning && !seqPreview) {
            const Song* s = seqSong();
            uint8_t pat = (s && s->startPattern < seqNumPat(s)) ? s->startPattern : 0;
            seqSendColumnSetup(pat, /*force=*/true);
            seqOutStatus = 0;
        }
        seqSongReaderBusy = false;
    }

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

static void seqPanicImpl() {
    for (uint8_t ch = 0; ch < 16; ch++) {
        midiTx((uint8_t)(0xB0 | ch));   // CC on channel
        midiTx((uint8_t)123);           // All Notes Off
        midiTx((uint8_t)0);
    }
    seqOutStatus = 0;  // force full status byte on next note
    debugPrintf("[SEQ] panic — all notes off\n");
}

static void seqQueueBlockImpl(uint8_t pattern) {
    // If waiting on row 0, the current block hasn't logically started yet —
    // switch immediately instead of queuing for block-end.
    if (seqWaiting && seqRow == 0) {
        const Song* s = seqSong();
        if (pattern < seqNumPat(s)) {
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

static void seqCancelQueueImpl() {
    seqQueuedPattern = -1;
    debugPrintf("[SEQ] cancel queued block\n");
}

static void seqSlotEnableImpl() {
    if (!srvHasActive) return;
    const Song* s = seqSong();
    midiTx((uint8_t)0xBF);                          // CC on MIDI channel 16
    midiTx((uint8_t)115);
    midiTx((uint8_t)((s->performerMask & 0x0F) << 3));
    seqOutStatus = 0;   // raw status went out — next message must carry its own
    debugPrintf("[SEQ] slotEnable mask=0x%02X val=0x%02X\n",
                  s->performerMask & 0x0F, (s->performerMask & 0x0F) << 3);
}

static void seqResetImpl() {
    seqFumbleOn = false;   // the saved "before the key" position is no longer valid
    // Clamp seqPattern against the *current* song.  Every song-replacement
    // path (load from SD, client push, new song) lands here while seqPattern
    // still reflects the previous song; if that song had more blocks, the
    // stale index reads a Pattern slot that's logically deleted, and its
    // noteHead can be a garbage value that walks the linked list off the end
    // of the note pool.  Reset to block 0 if out of range.
    if (srvHasActive) {
        const Song* s = seqSong();
        if (s && seqPattern >= seqNumPat(s)) seqPattern = 0;
    }
    seqRow            = 0;
    seqWaiting        = false;
    seqQueuedPattern  = -1;
    seqReposition(0);
    debugPrintf("[SEQ] reset t=%lu pat=%u row=0\n", millis(), seqPattern);
    if (seqRowCallback) seqRowCallback(seqPattern, seqRow);
}

static void seqSeekImpl(uint8_t pattern, uint8_t row) {
    seqFumbleOn = false;   // the saved "before the key" position is no longer valid
    if (!srvHasActive) return;
    const Song* s = seqSong();
    if (pattern >= seqNumPat(s)) return;
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

static void seqGotoImpl(uint8_t pat, uint8_t row) {
    seqFumbleOn = false;   // the saved "before the key" position is no longer valid
    if (!srvHasActive) return;
    const Song* s = seqSong();
    if (pat >= seqNumPat(s)) return;
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

// One client note edit, applied on the MIDI task so the pool is never relinked
// under a row walk.  NoteGrid::set can free the node seqNextNoteIdx points at,
// or link a new node in ahead of it, so the cursor is re-derived afterwards.
// Requests only run between row walks, where seqRow is always the next row
// still to play (the tick and the snap path both advance it straight after
// seqPlayRow; a WAIT halt parks on its own, unplayed, row) — so "first node
// with row >= seqRow" neither replays a sounded row nor skips an unsounded
// one.  Done whenever the edit is in seqPattern, running or not: a stopped
// sequencer resumes from this same cursor.  Preview keeps no cursor (it
// re-walks from the head every row), so it needs nothing beyond running here.
static void seqApplyNoteSetImpl(uint8_t pattern, uint8_t row, uint8_t col, const Note& n) {
    seqFumbleOn = false;   // the saved "before the key" position is no longer valid
    if (!srvHasActive) return;
    Song* song = reinterpret_cast<Song*>(srvActiveBuf + sizeof(SongFileHeader));
    if (pattern >= song->numPatterns || pattern >= MAX_PATTERNS ||
        row >= MAX_ROWS || col >= MAX_COLUMNS) return;
    NoteGrid grid(song->notePool, &song->noteFreeHead, &song->patterns[pattern].noteHead);
    grid.set(row, col, n);   // clears the cell when n is all-empty
    if (pattern == seqPattern) seqReposition(seqRow);
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

static void seqPauseImpl() {
    seqPaused = true;
    for (int c = 0; c < MAX_COLUMNS; c++) seqNoteOff(c);
    debugPrintf("[SEQ] pause t=%lu\n", millis());
}

static void seqUnpauseImpl() {
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
        if ((int32_t)(now - seqPreviewNextMs) < 0) return;

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
                if (seqPattern >= seqNumPat(s2)) seqPattern = 0;
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
            seqNextRowMs    = now;
            seqHaveLastSnap = false;
            seqReposition(0);
            seqClockOnWaitTimeout();   // stop external gear; a row-play re-Starts it
            if (seqRowCallback) seqRowCallback(seqPattern, seqRow);
        }
        // else: silently waiting — no spam
        return;
    }

    if ((int32_t)(now - seqNextRowMs) < 0) return;

    // Check WAIT before playing — peek at the next node; if it is a col-0 WAIT
    // at the current row, halt until the performer resolves it.
    const Song*    s    = seqSong();
    const Pattern& pat  = s->patterns[seqPattern];
    if (seqNextNoteIdx < MAX_SONG_NOTES) {          // also excludes NOTE_NULL
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
    if (seqRow >= pat.length) seqEnterNextBlock(s);

    if (seqRowCallback) seqRowCallback(seqPattern, seqRow);

    uint32_t rowMs = seqRowMs(s);
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

void sequencerTick() {
    seqSongReaderBusy = true;
    sequencerTickBody();
    seqSongReaderBusy = false;
}

void sequencerPollMidiIn() {
    seqSongReaderBusy = true;
    sequencerPollMidiInBody();
    seqSongReaderBusy = false;
}

bool sequencerSongReaderBusy() {
    return seqSongReaderBusy;
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

// ── Cross-task entry ─────────────────────────────────────────────────────────
// midiTx, the running-status cache and the playhead state have exactly one
// owner: the MIDI task.  The client's commands arrive on the MagiLink worker
// and the screen / load path runs on the loop task, so every public control
// function below hands its work to the MIDI task and returns once it has run.
// Run there, a STOP cannot interleave with a row being played (stuck note) and
// two tasks can never interleave bytes of two MIDI messages.
//
// The hand-off is synchronous and has a fallback: if the MIDI task does not
// pick the request up within SEQ_CALL_PICKUP_MS (it only stalls that long in
// the ~60ms organ grab), the request is withdrawn and run on the calling task
// — exactly what every call did before this existed.  So the worst case is the
// old behaviour, never a lost STOP.
struct SeqReq {
    void (*fn)(const SeqReq&);
    uint8_t a, b, c, d;
    uint8_t n;
    uint8_t bytes[3];
};

#ifdef UNIT_TEST
static void seqRun(const SeqReq& r) { r.fn(r); }   // host test: single-threaded
#else
static const uint32_t SEQ_CALL_PICKUP_MS = 250;
static TaskHandle_t      seqMidiTask   = nullptr;   // owner; null until bound
static SemaphoreHandle_t seqCallMutex  = nullptr;   // one foreign caller at a time
static volatile TaskHandle_t seqCallHolder = nullptr;
static SeqReq            seqReq;
static volatile uint8_t  seqReqState   = 0;         // 0 idle, 1 posted, 2 running
static portMUX_TYPE      seqReqMux     = portMUX_INITIALIZER_UNLOCKED;

void sequencerBindMidiTask() {
    if (!seqCallMutex) seqCallMutex = xSemaphoreCreateMutex();
    seqMidiTask = xTaskGetCurrentTaskHandle();
}

static void seqRun(const SeqReq& r) {
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    // Already on the owner (or before the task exists, at boot), or a request
    // body that calls another entry point: just run it.
    if (!seqMidiTask || me == seqMidiTask || me == seqCallHolder) { r.fn(r); return; }

    xSemaphoreTake(seqCallMutex, portMAX_DELAY);
    seqCallHolder = me;
    seqReq        = r;
    seqReqState   = 1;
    uint32_t t0 = millis();
    while (seqReqState != 0) {
        if (seqReqState == 1 && millis() - t0 >= SEQ_CALL_PICKUP_MS) {
            bool withdrawn = false;
            portENTER_CRITICAL(&seqReqMux);
            if (seqReqState == 1) { seqReqState = 0; withdrawn = true; }
            portEXIT_CRITICAL(&seqReqMux);
            if (withdrawn) {
                debugPrintf("[SEQ] MIDI task busy >%ums — ran request on caller\n",
                            (unsigned)SEQ_CALL_PICKUP_MS);
                r.fn(r);
            }
            continue;           // withdrawn → loop exits; running → keep waiting
        }
        vTaskDelay(1);
    }
    seqCallHolder = nullptr;
    xSemaphoreGive(seqCallMutex);
}

// MIDI task: run a posted request, under the same busy flag as the tick so
// quiesceSongReaders() waits it out before the song buffer is rewritten.
void sequencerServiceRequests() {
    if (seqReqState != 1) return;
    bool mine = false;
    portENTER_CRITICAL(&seqReqMux);
    if (seqReqState == 1) { seqReqState = 2; mine = true; }
    portEXIT_CRITICAL(&seqReqMux);
    if (!mine) return;
    seqSongReaderBusy = true;
    seqReq.fn(seqReq);
    seqSongReaderBusy = false;
    seqReqState = 0;
}
#endif

#define SEQ_CALL0(impl)        do { SeqReq r{}; r.fn = [](const SeqReq&)  { impl(); };             seqRun(r); } while (0)
void sequencerStart()       { SEQ_CALL0(seqStartImpl); }
void sequencerResume()      { SEQ_CALL0(seqResumeImpl); }
void sequencerStop()        { SEQ_CALL0(seqStopImpl); }   // immediate organ release
void sequencerStopPreview() { SEQ_CALL0(seqStopPreviewImpl); }
void sequencerPanic()       { SEQ_CALL0(seqPanicImpl); }
void sequencerCancelQueue() { SEQ_CALL0(seqCancelQueueImpl); }
void seqSendSlotEnable()    { SEQ_CALL0(seqSlotEnableImpl); }
void sequencerReset()       { SEQ_CALL0(seqResetImpl); }
void sequencerPause()       { SEQ_CALL0(seqPauseImpl); }
void sequencerUnpause()     { SEQ_CALL0(seqUnpauseImpl); }
#undef SEQ_CALL0

void sequencerQueueBlock(uint8_t pattern) {
    SeqReq r{}; r.a = pattern;
    r.fn = [](const SeqReq& q) { seqQueueBlockImpl(q.a); };
    seqRun(r);
}
void sequencerStartPreview(uint8_t pat, uint8_t col) {
    SeqReq r{}; r.a = pat; r.b = col;
    r.fn = [](const SeqReq& q) { seqStartPreviewImpl(q.a, q.b); };
    seqRun(r);
}
void sequencerSeek(uint8_t pattern, uint8_t row) {
    SeqReq r{}; r.a = pattern; r.b = row;
    r.fn = [](const SeqReq& q) { seqSeekImpl(q.a, q.b); };
    seqRun(r);
}
void sequencerGoto(uint8_t pat, uint8_t row) {
    SeqReq r{}; r.a = pat; r.b = row;
    r.fn = [](const SeqReq& q) { seqGotoImpl(q.a, q.b); };
    seqRun(r);
}
void sequencerAuditionNote(uint8_t pat, uint8_t row, uint8_t col) {
    SeqReq r{}; r.a = pat; r.b = row; r.c = col;
    r.fn = [](const SeqReq& q) { seqAuditionNoteImpl(q.a, q.b, q.c); };
    seqRun(r);
}
void sequencerAuditionRawNote(uint8_t channel, uint8_t note, uint8_t velocity,
                              uint8_t col) {
    SeqReq r{}; r.a = channel; r.b = note; r.c = velocity; r.d = col;
    r.fn = [](const SeqReq& q) { seqAuditionRawNoteImpl(q.a, q.b, q.c, q.d); };
    seqRun(r);
}
void sequencerSamplePlay(uint8_t sampleId, uint8_t note) {
    SeqReq r{}; r.a = sampleId; r.b = note;
    r.fn = [](const SeqReq& q) { seqSamplePlayImpl(q.a, q.b, false); };
    seqRun(r);
}
void sequencerSamplePadsStop() {
    SeqReq r{};
    r.fn = [](const SeqReq&) { seqSamplePadsStopImpl(); };
    seqRun(r);
}
void sequencerApplyNoteSet(uint8_t pattern, uint8_t row, uint8_t col,
                           uint8_t note, uint8_t velocity, uint8_t effect, uint8_t param) {
    SeqReq r{}; r.a = pattern; r.b = row; r.c = col; r.d = note;
    r.bytes[0] = velocity; r.bytes[1] = effect; r.bytes[2] = param;
    r.fn = [](const SeqReq& q) {
        Note n;
        n.note = q.d; n.velocity = q.bytes[0]; n.effect = q.bytes[1]; n.param = q.bytes[2];
        seqApplyNoteSetImpl(q.a, q.b, q.c, n);
    };
    seqRun(r);
}
void midiSendRawBytes(const uint8_t* bytes, int n) {
    // A MIDI channel message is at most 3 bytes; anything longer (never sent
    // today) is forwarded in 3-byte pieces rather than truncated.
    while (n > 0) {
        SeqReq r{};
        r.n = (uint8_t)(n > 3 ? 3 : n);
        memcpy(r.bytes, bytes, r.n);
        r.fn = [](const SeqReq& q) { seqRawBytesImpl(q.bytes, q.n); };
        seqRun(r);
        bytes += r.n;
        n     -= r.n;
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
uint16_t _test_getNextNoteIdx()           { return seqNextNoteIdx; }
void     _test_pushBpmHistory(uint16_t b) { seqPushBpmHistory(b); }
#endif
