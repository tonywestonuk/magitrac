// midi_player.cpp — sequencer + performer sync for M5Stack server

#include <Arduino.h>
#include "TrackerData.h"
#include "MagiMsg.h"
#include "midi_player.h"
#include "debug_log.h"
#include "hal/uart_ll.h"
#include "SamplePlayer.h"
#include "SampleManifest.h"

// Direct UART2 hardware FIFO access — bypasses Arduino's software ring buffer
// to minimise performer-note latency (saves ~50-200 µs per byte).
#define MIDI_UART_NUM 2
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
static int8_t   seqTranspose     = 0;
static uint16_t seqCurrentBPM    = 100;   // live BPM — derived from performer timing

// Block-end navigation — legacy variable, kept for reset paths only.
// -1 = BLK- (go to previous), 0 = loop current, 1 = BLK+ (go to next)
static int8_t   seqEndNav        = 0;

// Performance mode: queued block override — if >= 0, this block plays after the
// current block ends (overrides blockEndNav). Reset to -1 after use.
static int8_t   seqQueuedPattern = -1;

// Monotonic row counter — increments every time any row is played.
// Only the delta between two snaps matters, so absolute value is irrelevant.
static uint32_t seqAbsRow        = 0;

// Last performer snap — used to derive BPM on the next snap.
static uint32_t seqLastSnapMs     = 0;
static uint32_t seqLastSnapAbsRow = 0;
static bool     seqHaveLastSnap   = false;

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
extern HardwareSerial midi;
extern Instrument     srvInstruments[];

// Direct UART TX — write a byte straight to the hardware FIFO, busy-waiting
// only if the 128-byte FIFO is full (rare at MIDI rates).
static inline void midiTx(uint8_t b) {
    while (uart_ll_get_txfifo_len(midiHw) == 0) {}
    uart_ll_write_txfifo(midiHw, &b, 1);
}

// Last MIDI status byte written — enables running status on output.
// Reset to 0 on sequencer stop so the first message after a restart always sends its status.
static uint8_t  seqOutStatus = 0;

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

// Chan sentinel marking a column as currently playing a sample (SFX).
// Distinct from any real 0..15 midi channel.
#define SEQ_CHAN_SFX 0xFF

static void seqNoteOff(int col) {
    if (seqActiveNote[col] == 0) return;
    if (seqActiveChan[col] == SEQ_CHAN_SFX) {
        samplePlayerStop();
    } else {
        uint8_t note = seqActiveNote[col];
        seqWriteStatus((uint8_t)(0x80 | seqActiveChan[col]));
        midiTx(note);
        midiTx((uint8_t)0);
    }
    seqActiveNote[col] = 0;
    seqActiveChan[col] = 0;
}

// Scan the active pattern's circular list from its head and position
// seqNextNoteIdx at the first node with row >= targetRow.
// Call whenever seqRow or seqPattern changes discontinuously.
static void seqReposition(uint8_t targetRow) {
    const Song*    s    = seqSong();
    uint16_t       head = s->patterns[seqPattern].noteHead;
    if (head == NOTE_NULL) { seqNextNoteIdx = NOTE_NULL; return; }
    uint16_t idx = head;
    do {
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

    // Walk nodes at seqRow in list order (col 0 first, then col 1+).
    // seqNextNoteIdx is already positioned at the first node for this row.
    while (seqNextNoteIdx != NOTE_NULL) {
        const NoteNode& nn = s->notePool[seqNextNoteIdx];
        if (nn.row != seqRow) break;                    // past this row — stop

        uint16_t nxt   = nn.next;
        seqNextNoteIdx = (nxt == head) ? NOTE_NULL : nxt;

        if (nn.col == INPUT_COLUMN) {
            // WAIT/SYNC effects are handled by tick/performer logic, not here
        } else if (nn.note == NOTE_OFF) {
            const ColumnSettings& cs = s->columns[nn.col];

            // PIXELPOST: "lift finger".  Send MOVE + SLIDER with pressed=0
            // using whatever values are on the OFF row (typically zeros, but
            // composer can also set a final position to release at).
            if (cs.midiChannel == PIXELPOST_CHANNEL) {
                extern void pixelpostSendSlider(uint8_t value, bool pressed);
                extern void pixelpostSendMove(uint8_t x, uint8_t y, bool pressed);
                seqPpPressed[nn.col] = false;
                uint8_t vel = (nn.velocity & 0x80) ? 0 : nn.velocity;
                uint8_t slider = (vel <= 127) ? (uint8_t)(vel * 2) : 255;
                pixelpostSendSlider(slider, /*pressed=*/false);
                pixelpostSendMove(nn.effect, nn.param, /*pressed=*/false);
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
                cs.midiChannel != PIXELPOST_CHANNEL && !cs.mute) {
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
                const char* fname = sampleManifestNameFor(cs.program);
                if (fname) {
                    char path[64];
                    snprintf(path, sizeof(path), "/samples/%s", fname);
                    samplePlayerPlay(path);
                    seqActiveNote[col] = 1;               // any non-zero marks "active"
                    seqActiveChan[col] = SEQ_CHAN_SFX;    // route stop to SamplePlayer
                }
                continue;
            }

            if (cs.midiChannel == PIXELPOST_CHANNEL) {
                // PIXELPOST: a real note means "finger down at this XY with
                // this slider value, on this effect".  Effect index = note - 1
                // (so C-0 = 0, C#0 = 1, ...).  SELECT_EFFECT is deduped per
                // column — only sent when the effect index changes.
                // Skip non-pitched notes (NOTE_ANY etc.); they have no
                // sensible effect mapping.
                if (nn.note < 1 || nn.note > NOTE_MAX) continue;
                extern void pixelpostSendSelectEffect(uint8_t idx);
                extern void pixelpostSendSlider(uint8_t value, bool pressed);
                extern void pixelpostSendMove(uint8_t x, uint8_t y, bool pressed);
                seqPpPressed[col] = true;
                uint8_t effectIdx = (uint8_t)(nn.note - 1);
                if (effectIdx != seqPpLastEffect[col]) {
                    pixelpostSendSelectEffect(effectIdx);
                    seqPpLastEffect[col] = effectIdx;
                }
                uint8_t vel = (nn.velocity & 0x80) ? 100 : nn.velocity;
                uint8_t slider = (vel <= 127) ? (uint8_t)(vel * 2) : 255;
                pixelpostSendSlider(slider, /*pressed=*/true);
                pixelpostSendMove(nn.effect, nn.param, /*pressed=*/true);
                continue;
            }

            uint8_t midiCh = (uint8_t)(cs.midiChannel - 1);
            int8_t  colXp  = cs.transpose;

            int raw = (int)nn.note + TRACKER_TO_MIDI_OFFSET + seqTranspose + (int)colXp;
            if (raw < 0)   raw = 0;
            if (raw > 127) raw = 127;
            uint8_t midiNote = (uint8_t)raw;

            uint8_t noteVel = (nn.velocity & 0x80) ? 100 : nn.velocity;
            seqWriteStatus((uint8_t)(0x90 | midiCh));
            midiTx(midiNote);
            midiTx(noteVel);
            seqActiveNote[col] = midiNote;
            seqActiveChan[col] = midiCh;
        } else {
            // NOTE_EMPTY — only meaningful on PIXELPOST columns: velocity-only
            // rows fire MSG_SLIDER, attribute-only rows fire MSG_MOVE.  Both
            // ride the column's current "pressed" state (set by the last NOTE
            // or NOTE_OFF row).
            const ColumnSettings& cs = s->columns[nn.col];
            if (cs.midiChannel == PIXELPOST_CHANNEL) {
                extern void pixelpostSendSlider(uint8_t value, bool pressed);
                extern void pixelpostSendMove(uint8_t x, uint8_t y, bool pressed);
                bool pressed = seqPpPressed[nn.col];
                if (!(nn.velocity & 0x80)) {
                    uint8_t slider = (nn.velocity <= 127)
                                     ? (uint8_t)(nn.velocity * 2) : 255;
                    pixelpostSendSlider(slider, pressed);
                }
                if (nn.effect != 0 || nn.param != 0) {
                    pixelpostSendMove(nn.effect, nn.param, pressed);
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
            seqPattern = target;
            if (entry.switchMode == BlockSwitch::TOP) seqRow = 0;
            seqWaiting = false;
            seqSendColumnSetup(seqPattern);
            seqReposition(seqRow);

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
            if (mode == NAV_FWD && target > 0) {
                seqPattern += target;
                if (seqPattern >= s->numPatterns) seqPattern = 0;
            } else if (mode == NAV_BACK && target > 0) {
                if (seqPattern >= target) seqPattern -= target;
                else seqPattern = s->numPatterns - 1;
            } else if (mode == NAV_ABS) {
                seqPattern = (target < s->numPatterns) ? target : 0;
            }
            seqRow               = 0;
            seqEndNav            = 0;
            seqWaiting           = false;
            seqSnapCooldownUntil = 0;
            seqSendColumnSetup(seqPattern);
            seqReposition(0);
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
    }
    seqLastSeenPat = seqPattern;
    seqLastSeenRow = seqRow;

    // Find the cue row to snap to (using updated seqPattern/seqTranspose).
    // Walk runs even during snap cooldown so noise markers still get consumed;
    // cooldown only suppresses the snap itself.
    uint8_t cueRow = 0xFF;
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
        // (the row after the last WAIT/SYNC snap), not from the playhead.
        // This keeps noise markers visible for the whole section, even if the
        // tick has advanced past them.  WAIT/SYNC stops the search (cue row).
        // A no-effect col-0 ("expected noise") swallows a matching pitch so
        // it doesn't snap to the next WAIT; non-matching pitches walk past it
        // to find the real cue.  Already-consumed noise markers are skipped
        // so a subsequent same-pitch note can still reach the WAIT.
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
                    bool isWaitSync = (nn.effect == EFFECT_SYNC || nn.effect == EFFECT_WAIT);
                    if (isWaitSync) {
                        if (pitchMatches) cueRow = nn.row;
                        break;   // WAIT/SYNC stops the search regardless of match
                    }
                    bool consumedAlready = (seqNoiseConsumedRow != 0xFF) &&
                                           (nn.row <= seqNoiseConsumedRow);
                    if (!consumedAlready && pitchMatches) {
                        seqNoiseConsumedRow = nn.row;
                        return;  // swallow — no snap, no tempo, no action
                    }
                    // else: consumed noise OR non-matching marker — keep walking
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

    // BPM derivation after MIDI is out (uses pre-increment absRow)
    seqRecordSnap(now);
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
void sequencerPollMidiIn() {
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
                if (velocity >= 20) {
                    const Song* s = seqSong();
                    uint8_t ch = midiStatus & 0x0F;  // 0-based channel
                    bool chanOk = (s->midiInChannel == 0) || (ch == s->midiInChannel - 1);
                    bool noteOk = (midiByte1 >= s->midiInNoteMin) && (midiByte1 <= s->midiInNoteMax);
                    if (chanOk && noteOk && !seqPreview) {
                        if (seqRunning) {
                            seqOnPerformerNote(midiByte1);
                        } else if (seqMidiNoteInCallback) {
                            seqMidiNoteInCallback(midiByte1, velocity);
                        }
                    }
                }
            }
        } else if ((midiStatus & 0xF0) == 0x80) {
            midiCount = (midiCount == 0) ? 1 : 0;
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
                    const char* fname = sampleManifestNameFor(cs.program);
                    if (fname) {
                        char path[64];
                        snprintf(path, sizeof(path), "/samples/%s", fname);
                        samplePlayerPlay(path);
                        seqActiveNote[seqPreviewCol] = 1;
                        seqActiveChan[seqPreviewCol] = SEQ_CHAN_SFX;
                    }
                    return;
                }
                if (cs.midiChannel == PIXELPOST_CHANNEL) {
                    // Audition path — always send SELECT_EFFECT (no dedup —
                    // the user explicitly asked for this preview).
                    if (nn.note < 1 || nn.note > NOTE_MAX) return;
                    extern void pixelpostSendSelectEffect(uint8_t idx);
                    extern void pixelpostSendSlider(uint8_t value, bool pressed);
                    extern void pixelpostSendMove(uint8_t x, uint8_t y, bool pressed);
                    uint8_t effectIdx = (uint8_t)(nn.note - 1);
                    pixelpostSendSelectEffect(effectIdx);
                    seqPpLastEffect[seqPreviewCol] = effectIdx;
                    uint8_t vel = (nn.velocity & 0x80) ? 100 : nn.velocity;
                    uint8_t slider = (vel <= 127) ? (uint8_t)(vel * 2) : 255;
                    pixelpostSendSlider(slider, /*pressed=*/true);
                    pixelpostSendMove(nn.effect, nn.param, /*pressed=*/true);
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
    seqSendColumnSetup(startPat, /*force=*/true);

    seqSendSlotEnable();

    seqPattern       = 0;
    seqRow           = 0;
    seqWaiting       = false;
    seqEndNav        = 0;
    seqQueuedPattern = -1;
    seqTranspose     = 0;
    seqCurrentBPM   = seqSong()->bpm > 0 ? seqSong()->bpm : 100;
    seqAbsRow            = 0;
    seqHaveLastSnap      = false;
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
    seqPaused    = false;
    seqWaiting   = false;
    seqRunning   = true;
    seqCurrentBPM = seqSong()->bpm > 0 ? seqSong()->bpm : 100;
    seqHaveLastSnap = false;
    seqNextRowMs = millis();
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
        const char* fname = sampleManifestNameFor(cs.program);
        if (fname) {
            char path[64];
            snprintf(path, sizeof(path), "/samples/%s", fname);
            samplePlayerPlay(path);
            seqActiveNote[col] = 1;
            seqActiveChan[col] = SEQ_CHAN_SFX;
            seqAuditionCol   = col;
            seqAuditionEndMs = millis() + 500;
            debugPrintf("[SEQ] audition col=%u SFX=%s\n", col, fname);
        }
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
    seqActiveNote[col] = midiNote;
    seqActiveChan[col] = midiCh;

    seqAuditionCol   = col;
    seqAuditionEndMs = millis() + 500;
    debugPrintf("[SEQ] audition col=%u note=%u\n", col, midiNote);
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
    seqPattern = pattern;
    seqRow     = row;
    seqWaiting = false;
    seqReposition(row);
    seqPlayRow();
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

void sequencerTick() {
    // Audition timeout — fires regardless of play / preview state, so a
    // stranded audition note never sits on indefinitely.
    if (seqAuditionCol != 0xFF && (int32_t)(millis() - seqAuditionEndMs) >= 0) {
        seqNoteOff(seqAuditionCol);
        seqAuditionCol = 0xFF;
        seqOutStatus = 0;
    }

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
        if (elapsed > 500) {
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
            peek.note != NOTE_EMPTY && peek.effect == EFFECT_WAIT) {
            seqWaiting     = true;
            seqWaitStartMs = now;
            debugPrintf("[T] WAIT      t=%lu pat=%u row=%u\n", now, seqPattern, seqRow);
            return;
        }
    }

    seqPlayRow();
    uint32_t afterPlay = millis();  // schedule from MIDI-out time
    seqAbsRow++;

    seqRow++;
    if (seqRow >= pat.length) {
        if (seqQueuedPattern >= 0) {
            // Performance mode override — use queued block
            seqPattern = (uint8_t)seqQueuedPattern;
            if (seqPattern >= s->numPatterns) seqPattern = 0;
            seqQueuedPattern = -1;  // one-shot
        } else {
            uint8_t nav = pat.blockEndNav;
            uint8_t mode = nav & NAV_MODE_MASK;
            uint8_t target = nav & NAV_TARGET_MASK;
            if (mode == NAV_FWD && target > 0) {
                seqPattern += target;
                if (seqPattern >= s->numPatterns) seqPattern = 0;
            } else if (mode == NAV_BACK && target > 0) {
                if (seqPattern >= target) seqPattern -= target;
                else seqPattern = s->numPatterns - 1;
            } else if (mode == NAV_ABS) {
                seqPattern = (target < s->numPatterns) ? target : 0;
            }
        }
        seqRow = 0;
        seqSendColumnSetup(seqPattern);
        seqReposition(0);
    }

    if (seqRowCallback) seqRowCallback(seqPattern, seqRow);

    uint32_t rowMs = (uint32_t)s->speed * 2500UL / seqCurrentBPM;
    seqNextRowMs = afterPlay + rowMs;
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
void    _test_onPerformerNote(uint8_t n) { seqOnPerformerNote(n); }
uint8_t _test_getRow()                   { return seqRow; }
uint8_t _test_getPattern()               { return seqPattern; }
bool    _test_isWaiting()                { return seqWaiting; }
bool    _test_isRunning()                { return seqRunning; }
#endif
