// Unit tests for sequencer WAIT/SYNC performer-snap logic.
//
// Run from Terminal — NOT through the Arduino IDE:
//   cd /Users/tonyweston/magitrac/magitrac_server_s3
//   g++ -std=c++11 -Wno-format -DUNIT_TEST -I test_mocks -I . \
//       -I ../magitrac_lib/src test_seq.cpp -o test_seq && ./test_seq
//
// The #ifndef ARDUINO guard below makes this file a no-op when the Arduino IDE
// accidentally tries to compile it as part of the sketch.
#ifndef ARDUINO

// ── Mock runtime (must come before any Arduino header) ────────────────────────
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <assert.h>

static uint32_t _mockMs = 0;
uint32_t millis() { return _mockMs; }

#include "test_mocks/Arduino.h"
HardwareSerial Serial;
HardwareSerial midi;

// Sketch-side functions that midi_player.cpp references but the test doesn't
// exercise — stub them out so the link succeeds.
void debugPrintf(const char* /*fmt*/, ...) {}
void samplePlayerPlay(const char* /*path*/, int /*vol*/) {}
void samplePlayerStop() {}
const char* sampleManifestNameFor(uint8_t /*id*/) { return nullptr; }
void pixelpostSetTouchpad(uint8_t, uint8_t, bool) {}
void pixelpostSetSlider(uint8_t) {}
void pixelpostSetEffect(uint8_t) {}

// ── External state required by midi_player.cpp ───────────────────────────────
#include "TrackerData.h"
Instrument srvInstruments[MAX_INSTRUMENTS] = {};

static uint8_t _buf[sizeof(SongFileHeader) + sizeof(Song)];
uint8_t   srvActiveBuf[sizeof(SongFileHeader) + sizeof(Song)];
uint32_t  srvActiveBufLen = sizeof(srvActiveBuf);
bool      srvHasActive    = true;

// ── Pull in the implementation under test ────────────────────────────────────
// The server has no TrackerData.cpp — midi_player.cpp only needs the .h types.
#include "NoteGrid.cpp"
#include "midi_player.cpp"

// ── Test infrastructure ───────────────────────────────────────────────────────
static int gPassed = 0, gFailed = 0;
static uint8_t gLastCallbackRow = 0xFF;

static void rowCallback(uint8_t /*pattern*/, uint8_t row) { gLastCallbackRow = row; }

#define CHECK(cond, msg) \
    do { if (cond) { printf("  PASS  %s\n", msg); gPassed++; } \
         else      { printf("  FAIL  %s  (line %d)\n", msg, __LINE__); gFailed++; } \
    } while(0)

// Rows-per-ms at bpm=120, speed=6:  6 * 2500 / 120 = 125 ms/row
static const uint32_t ROW_MS = 125;

// ── Song builder helpers ──────────────────────────────────────────────────────
static void initPool(Song* song) {
    for (uint16_t i = 0; i < MAX_SONG_NOTES - 1; i++)
        song->notePool[i].next = (uint16_t)(i + 1);
    song->notePool[MAX_SONG_NOTES - 1].next = NOTE_NULL;
    song->noteFreeHead = 0;
    for (int i = 0; i < MAX_PATTERNS; i++)
        song->patterns[i].noteHead = NOTE_NULL;
}

// 8-row pattern with C-4 WAIT on even rows (0,2,4,6).
// setTranspose=false → KEEP (only exact pitch resolves WAIT — good for isolation tests)
// setTranspose=true  → SET  (each pitch sets transpose to its own index — demo style)
static void buildWaitPattern(bool setTranspose) {
    memset(srvActiveBuf, 0, sizeof(srvActiveBuf));

    SongFileHeader* hdr = (SongFileHeader*)srvActiveBuf;
    hdr->magic   = SONG_FILE_MAGIC;
    hdr->version = SONG_FILE_VERSION;

    Song* song = (Song*)(srvActiveBuf + sizeof(SongFileHeader));
    song->bpm          = 120;
    song->minBPM       = 60;
    song->maxBPM       = 240;
    song->speed        = 6;
    song->numPatterns  = 1;
    song->startPattern = 0;

    initPool(song);

    Pattern& p = song->patterns[0];
    p.length = 8;

    // C-4 WAIT on rows 0, 2, 4, 6
    NoteGrid grid(song->notePool, &song->noteFreeHead, &p.noteHead);
    for (int r = 0; r < 8; r += 2) {
        Note n = { makeNote(0, 4), 0, EFFECT_WAIT, 0 };
        grid.set((uint8_t)r, INPUT_COLUMN, n);
    }

    for (int i = 0; i < 12; i++) {
        p.inputNotes[i].switchMode = BlockSwitch::STAY;
        if (setTranspose) {
            p.inputNotes[i].transposeAction = TransposeAction::CUSTOM;
            p.inputNotes[i].transposeValue  = (int8_t)i;
        } else {
            p.inputNotes[i].transposeAction = TransposeAction::KEEP;
            p.inputNotes[i].transposeValue  = 0;
        }
    }
}

// Tick the sequencer forward one row-period from the current mock time.
static void tickNextRow() { _mockMs += ROW_MS + 1; sequencerTick(); }

// ── Tests ─────────────────────────────────────────────────────────────────────

// Waiting at row 0 — C resolves only row 0, advances to row 1 (not row 2/4/6)
static void test_wait_resolves_current_row_only() {
    printf("\ntest_wait_resolves_current_row_only\n");
    buildWaitPattern(false);
    _mockMs = 0; gLastCallbackRow = 0xFF;
    seqSetRowCallback(rowCallback);
    sequencerStart();

    sequencerTick();                                // row 0 has WAIT → halt
    CHECK(_test_isWaiting(),      "halts at row 0 WAIT");
    CHECK(_test_getRow() == 0,    "halted row is 0");

    _mockMs += 100;
    _test_onPerformerNote(60);                      // C4, pitch class 0
    CHECK(!_test_isWaiting(),     "WAIT resolved");
    CHECK(_test_getRow() == 1,    "advanced to row 1 (NOT 2, 4, or 6)");
    CHECK(gLastCallbackRow == 1,  "row callback fired for row 1");
}

// After resolving row 0, sequencer naturally hits row 2 WAIT.
// Performer resolves row 2 → advances to row 3 (NOT row 4 or 6).
static void test_sequential_waits_not_skipped() {
    printf("\ntest_sequential_waits_not_skipped\n");
    buildWaitPattern(false);
    _mockMs = 0;
    sequencerStart();

    // Resolve row 0 WAIT
    sequencerTick();
    _mockMs += 100;
    _test_onPerformerNote(60);                      // → seqRow=1
    CHECK(_test_getRow() == 1, "at row 1 after resolving row 0");

    // Advance through row 1 (no WAIT)
    tickNextRow();
    CHECK(!_test_isWaiting(),  "row 1 plays normally");

    // Row 2 has WAIT — sequencer halts
    tickNextRow();
    CHECK(_test_isWaiting(),   "halts at row 2 WAIT");
    CHECK(_test_getRow() == 2, "halted at row 2 (not skipped to 4 or 6)");

    // Resolve row 2 WAIT
    _mockMs += 100;
    _test_onPerformerNote(60);
    CHECK(!_test_isWaiting(),  "row 2 WAIT resolved");
    CHECK(_test_getRow() == 3, "advanced to row 3 (NOT 4 or 6)");
}

// Wrong pitch (D) does not resolve a C-4 WAIT when transpose is KEEP.
static void test_wrong_pitch_does_not_resolve_wait() {
    printf("\ntest_wrong_pitch_does_not_resolve_wait\n");
    buildWaitPattern(false);    // KEEP transpose — D cannot match C-4 WAIT
    _mockMs = 0;
    sequencerStart();
    sequencerTick();            // halt at row 0

    _mockMs += 100;
    _test_onPerformerNote(62);  // D4, pitch class 2
    CHECK(_test_isWaiting(),   "WAIT still active after wrong pitch");
    CHECK(_test_getRow() == 0, "row unchanged");
}

// SYNC semantics (Option C, 2026-06-04):
//   Rule 1 (on-row late): playhead is currently on the SYNC row (the tick has
//     just played it; nn.row + 1 == seqRow).  A matching performer note resets
//     the row-timer to (now + rowMs), extending the row by however long was
//     left of it.  No snap, no BPM update.
//   Rule 2 (one row before): playhead is on the row right before the SYNC
//     (nn.row == seqRow).  A matching performer note snaps forward to the SYNC
//     row immediately.  No BPM update.
//   Out of proximity: SYNC is ignored.  The walk continues so a later WAIT or
//   PASS can still match.
// WAIT semantics are unchanged.
static void buildSyncAtRow2Pattern() {
    memset(srvActiveBuf, 0, sizeof(srvActiveBuf));
    SongFileHeader* hdr = (SongFileHeader*)srvActiveBuf;
    hdr->magic   = SONG_FILE_MAGIC;
    hdr->version = SONG_FILE_VERSION;
    Song* song = (Song*)(srvActiveBuf + sizeof(SongFileHeader));
    song->bpm = 120; song->minBPM = 60; song->maxBPM = 240;
    song->speed = 6; song->numPatterns = 1;

    initPool(song);

    Pattern& p = song->patterns[0];
    p.length = 8;

    NoteGrid grid(song->notePool, &song->noteFreeHead, &p.noteHead);
    // Row 0: C-4 WAIT (must resolve via seqWaiting path only)
    Note waitNote = { makeNote(0, 4), 0, EFFECT_WAIT, 0 };
    grid.set(0, INPUT_COLUMN, waitNote);
    // Row 2: C-4 SYNC (forward-snappable while running)
    Note syncNote = { makeNote(0, 4), 0, EFFECT_SYNC, 0 };
    grid.set(2, INPUT_COLUMN, syncNote);

    for (int i = 0; i < 12; i++) {
        p.inputNotes[i].switchMode      = BlockSwitch::STAY;
        p.inputNotes[i].transposeAction = TransposeAction::KEEP;
    }
}

// Rule 2: playhead is on the row right before a SYNC (seqRow == SYNC row),
// performer plays a matching note → snap forward to the SYNC row.  BPM does
// not get updated by this snap (tempo stays where it was).
static void test_sync_rule2_snap_one_row_before() {
    printf("\ntest_sync_rule2_snap_one_row_before\n");
    buildSyncAtRow2Pattern();
    _mockMs = 0;
    sequencerStart();

    sequencerTick();                       // halt at row 0 WAIT
    _mockMs += 100;
    _test_onPerformerNote(60);             // resolve row 0 → seqRow=1
    CHECK(_test_getRow() == 1, "row 1 after WAIT-0 resolve");

    tickNextRow();                         // play row 1 → seqRow=2 (one before SYNC)
    CHECK(_test_getRow() == 2, "seqRow=2 (playhead on the row before SYNC)");

    _mockMs += 10;
    _test_onPerformerNote(60);             // Rule 2 — snap forward to SYNC row 2
    CHECK(!_test_isWaiting(),  "not waiting after Rule 2 snap");
    CHECK(_test_getRow() == 3, "snapped to row 2, advanced to row 3");
}

// Rule 1: tick has just played a SYNC row (playhead is "on" the SYNC row;
// seqRow == SYNC row + 1).  A matching performer note resets the row timer
// to now + rowMs, extending the current row.  Position, abs row, and BPM
// must all stay where they were.
static void test_sync_rule1_extends_row() {
    printf("\ntest_sync_rule1_extends_row\n");
    buildSyncAtRow2Pattern();
    _mockMs = 0;
    sequencerStart();

    sequencerTick();                       // halt at row 0 WAIT
    _mockMs += 100;
    _test_onPerformerNote(60);             // resolve → seqRow=1
    tickNextRow();                         // play row 1 → seqRow=2
    tickNextRow();                         // play SYNC row 2 → seqRow=3
    CHECK(_test_getRow() == 3, "tick walked past SYNC to seqRow=3");

    uint32_t nextRowBefore = _test_getNextRowMs();
    uint32_t absBefore     = _test_getAbsRow();
    uint16_t bpmBefore     = _test_getBPM();

    _mockMs += 10;
    _test_onPerformerNote(60);             // late hit on the SYNC row → Rule 1

    CHECK(_test_getRow()      == 3,                "playhead unchanged (no snap)");
    CHECK(_test_getAbsRow()   == absBefore,        "absRow unchanged (no row played)");
    CHECK(_test_getBPM()      == bpmBefore,        "BPM unchanged");
    CHECK(_test_getNextRowMs() == _mockMs + ROW_MS, "next row tick reset to now + rowMs");
    CHECK(_test_getNextRowMs() > nextRowBefore,    "row extended (next tick pushed forward)");
}

// Rule 2 must NOT update BPM.  Without the skip, a snap arriving 10 ms after
// the SYNC row was tick-played would derive a very different BPM.
static void test_sync_rule2_does_not_update_bpm() {
    printf("\ntest_sync_rule2_does_not_update_bpm\n");
    buildSyncAtRow2Pattern();
    _mockMs = 0;
    sequencerStart();

    sequencerTick();
    _mockMs += 100;
    _test_onPerformerNote(60);             // resolve WAIT-0 (seqHaveLastSnap=true now)
    uint16_t bpmBefore = _test_getBPM();
    CHECK(bpmBefore == 120, "BPM at song default after first snap");

    tickNextRow();                         // play row 1 → seqRow=2
    _mockMs += 10;
    _test_onPerformerNote(60);             // Rule 2 snap
    CHECK(_test_getRow()  == 3,           "Rule 2 snap advanced to row 3");
    CHECK(_test_getBPM()  == bpmBefore,   "BPM unchanged by SYNC snap (Option C)");
}

// Performer plays while playhead is 2+ rows before the SYNC — outside the
// proximity window.  Under Option C the SYNC is ignored, the note doesn't
// cause a snap, and the playhead is unaffected.
static void test_sync_far_ahead_ignored() {
    printf("\ntest_sync_far_ahead_ignored\n");
    buildSyncAtRow2Pattern();
    _mockMs = 0;
    sequencerStart();

    sequencerTick();
    _mockMs += 100;
    _test_onPerformerNote(60);             // resolve WAIT-0 → seqRow=1
    CHECK(_test_getRow() == 1, "row 1 after WAIT-0 resolve");

    // Playhead on row 0 (seqRow=1), SYNC at row 2 — 2 rows away.  Ignored.
    uint32_t absBefore = _test_getAbsRow();
    _mockMs += ROW_MS;                     // past cooldown but still seqRow=1
    _test_onPerformerNote(60);
    CHECK(_test_getRow()    == 1,          "playhead unchanged (far SYNC ignored)");
    CHECK(_test_getAbsRow() == absBefore,  "no snap fired");
}

// Key pressed during row 1 (while running), next col-0 is a WAIT at row 2.
// Expected: snap to row 2 immediately, play it, advance to row 3 WITHOUT halting.
// The WAIT is pre-resolved — the performer answered before the clock arrived.
static void test_preresolve_wait_while_running() {
    printf("\ntest_preresolve_wait_while_running\n");
    buildWaitPattern(false);   // WAITs on rows 0,2,4,6
    _mockMs = 0;
    sequencerStart();

    // Resolve row 0 WAIT → seqRow=1, running
    sequencerTick();
    _mockMs += 100;
    _test_onPerformerNote(60);
    CHECK(_test_getRow() == 1,  "at row 1 after resolving row 0 WAIT");

    // Tick fires for row 1 (past cooldown)
    tickNextRow();
    CHECK(!_test_isWaiting(),   "row 1 plays normally, not waiting");
    CHECK(_test_getRow() == 2,  "tick advanced to row 2");

    // Before the clock fires again, performer plays C — row 2 is a WAIT, pre-resolve it
    _mockMs += 10;
    _test_onPerformerNote(60);
    CHECK(!_test_isWaiting(),   "WAIT at row 2 pre-resolved, not halting");
    CHECK(_test_getRow() == 3,  "snapped to row 2 and advanced to row 3");

    // Clock continues — row 3 should play normally (no WAIT), no halt
    tickNextRow();
    CHECK(!_test_isWaiting(),   "row 3 plays normally after pre-resolve");
}

// Fumble / chord: any number of notes within one row-duration of a snap must not
// trigger further snaps. Simulates the performer landing on several keys at once.
static void test_fumble_double_note_ignored() {
    printf("\ntest_fumble_double_note_ignored\n");
    buildWaitPattern(false);
    _mockMs = 0;
    sequencerStart();
    sequencerTick();            // halt at row 0

    _mockMs += 100;
    _test_onPerformerNote(60);  // C → resolves WAIT, seqRow=1, cooldown active
    CHECK(_test_getRow() == 1, "first note snaps to row 1");

    // Simulate several more notes arriving within cooldown (chord / fumble)
    _mockMs += 5;  _test_onPerformerNote(64);  // E
    _mockMs += 5;  _test_onPerformerNote(67);  // G
    _mockMs += 5;  _test_onPerformerNote(60);  // C again
    _mockMs += 5;  _test_onPerformerNote(62);  // D
    CHECK(_test_getRow() == 1, "all chord notes ignored during cooldown");

    _mockMs += 200;             // cooldown expired (ROW_MS = 125ms)
    sequencerTick();            // let sequencer naturally advance through row 1
    // Row 1 has no WAIT, so tick advances to row 2, which has WAIT → halt
    tickNextRow();
    CHECK(_test_isWaiting(),   "sequencer halts at row 2 WAIT after cooldown expires");
    CHECK(_test_getRow() == 2, "at row 2, not skipped by the fumbled note");
}

// Pattern for the "passed SYNC" cases:
//   row 0: WAIT C-4   (so we can start by resolving it)
//   row 2: SYNC <pitch>   (will be naturally walked past — no performer match)
//   row 5: WAIT C-4   (the cue a later C should still be able to reach)
// 8-row pattern, KEEP transpose.
static void buildPassedSyncPattern(uint8_t syncPitchTracker) {
    memset(srvActiveBuf, 0, sizeof(srvActiveBuf));
    SongFileHeader* hdr = (SongFileHeader*)srvActiveBuf;
    hdr->magic   = SONG_FILE_MAGIC;
    hdr->version = SONG_FILE_VERSION;
    Song* song = (Song*)(srvActiveBuf + sizeof(SongFileHeader));
    song->bpm = 120; song->minBPM = 60; song->maxBPM = 240;
    song->speed = 6; song->numPatterns = 1;

    initPool(song);

    Pattern& p = song->patterns[0];
    p.length = 8;

    NoteGrid grid(song->notePool, &song->noteFreeHead, &p.noteHead);
    Note wait0  = { makeNote(0, 4),              0, EFFECT_WAIT, 0 };
    Note sync2  = { syncPitchTracker,            0, EFFECT_SYNC, 0 };
    Note wait5  = { makeNote(0, 4),              0, EFFECT_WAIT, 0 };
    grid.set(0, INPUT_COLUMN, wait0);
    grid.set(2, INPUT_COLUMN, sync2);
    grid.set(5, INPUT_COLUMN, wait5);

    for (int i = 0; i < 12; i++) {
        p.inputNotes[i].switchMode      = BlockSwitch::STAY;
        p.inputNotes[i].transposeAction = TransposeAction::KEEP;
    }
}

// After the playhead naturally walks past a SYNC (performer didn't play in time),
// a later performer note must NOT pull the playhead backwards to that SYNC.
// SYNC is forward-only — cannot slow the track down.
static void test_passed_sync_does_not_snap_backward() {
    printf("\ntest_passed_sync_does_not_snap_backward\n");
    // Only row 0 WAIT + row 2 SYNC, both C-4.  No later cue.
    memset(srvActiveBuf, 0, sizeof(srvActiveBuf));
    SongFileHeader* hdr = (SongFileHeader*)srvActiveBuf;
    hdr->magic = SONG_FILE_MAGIC; hdr->version = SONG_FILE_VERSION;
    Song* song = (Song*)(srvActiveBuf + sizeof(SongFileHeader));
    song->bpm = 120; song->minBPM = 60; song->maxBPM = 240;
    song->speed = 6; song->numPatterns = 1;
    initPool(song);
    Pattern& p = song->patterns[0];
    p.length = 8;
    NoteGrid grid(song->notePool, &song->noteFreeHead, &p.noteHead);
    Note wait0 = { makeNote(0, 4), 0, EFFECT_WAIT, 0 };
    Note sync2 = { makeNote(0, 4), 0, EFFECT_SYNC, 0 };
    grid.set(0, INPUT_COLUMN, wait0);
    grid.set(2, INPUT_COLUMN, sync2);
    for (int i = 0; i < 12; i++) {
        p.inputNotes[i].switchMode      = BlockSwitch::STAY;
        p.inputNotes[i].transposeAction = TransposeAction::KEEP;
    }

    _mockMs = 0;
    sequencerStart();
    sequencerTick();                       // halt at row 0 WAIT
    _mockMs += 100;
    _test_onPerformerNote(60);             // resolve → seqRow=1
    CHECK(_test_getRow() == 1, "row 1 after resolving row 0 WAIT");

    tickNextRow();                         // row 1 plays → seqRow=2
    CHECK(_test_getRow() == 2, "tick advanced to row 2 (SYNC)");
    tickNextRow();                         // row 2 SYNC plays (no perf note) → seqRow=3
    CHECK(_test_getRow() == 3, "tick walked past SYNC to row 3");

    // Snapshot — a snap (correct or buggy) would increment absRow and set
    // seqLastSnapRow.  Performer now plays C: with the guard in place this
    // must do nothing (no later cue exists).
    uint32_t absBefore = _test_getAbsRow();
    _mockMs += ROW_MS;                     // past cooldown
    _test_onPerformerNote(60);
    CHECK(_test_getAbsRow() == absBefore,         "no snap occurred (absRow unchanged)");
    CHECK(_test_getLastSnapRow() != 2,            "did not record a backward snap to row 2");
    CHECK(_test_getRow() == 3,                    "playhead still at row 3");
}

// After the playhead naturally walks past a SYNC whose pitch the performer
// never matched, a later matching note must still be able to reach a WAIT
// further down the pattern (the passed SYNC must not block forward search).
static void test_passed_sync_does_not_block_later_wait() {
    printf("\ntest_passed_sync_does_not_block_later_wait\n");
    // Row 0 WAIT C-4, row 2 SYNC E-4 (different pitch class), row 5 WAIT C-4.
    buildPassedSyncPattern(makeNote(4, 4));   // E-4 SYNC

    _mockMs = 0;
    sequencerStart();
    sequencerTick();                       // halt at row 0 WAIT
    _mockMs += 100;
    _test_onPerformerNote(60);             // C → resolve row 0 → seqRow=1
    CHECK(_test_getRow() == 1, "row 1 after resolving row 0 WAIT");

    // Tick through rows 1, 2 (SYNC E-4 — no E played, so just plays through), 3
    tickNextRow();                         // → row 2
    tickNextRow();                         // SYNC row plays → row 3
    CHECK(_test_getRow() == 3, "tick walked past E-4 SYNC to row 3");

    // Performer plays C: the SYNC at row 2 (now passed) must not break the walk.
    // The walk should reach the WAIT C-4 at row 5 and snap there.
    _mockMs += ROW_MS;                     // past cooldown
    _test_onPerformerNote(60);
    CHECK(_test_getLastSnapRow() == 5,    "snapped forward to the WAIT at row 5");
    CHECK(_test_getRow() == 6,            "advanced to row 6 after snapping to row 5");
}

// ── PASS (noise-marker) tests ────────────────────────────────────────────────
//
// A PASS marker is a col-0 note with no effect (neither WAIT nor SYNC).  It
// absorbs a matching performer note while the playhead is at or before the
// PASS row, so that note doesn't snap forward to a later WAIT.  Once consumed,
// the marker is skipped by subsequent walks (a second matching note can still
// reach the WAIT).  And once the playhead has walked past the marker, it no
// longer absorbs anything.

// Pattern: row 0 WAIT C-4, row 3 PASS C-4, row 5 WAIT C-4.  Used by tests where
// the PASS sits AHEAD of the playhead.
static void buildPassAheadPattern() {
    memset(srvActiveBuf, 0, sizeof(srvActiveBuf));
    SongFileHeader* hdr = (SongFileHeader*)srvActiveBuf;
    hdr->magic = SONG_FILE_MAGIC; hdr->version = SONG_FILE_VERSION;
    Song* song = (Song*)(srvActiveBuf + sizeof(SongFileHeader));
    song->bpm = 120; song->minBPM = 60; song->maxBPM = 240;
    song->speed = 6; song->numPatterns = 1;
    initPool(song);
    Pattern& p = song->patterns[0];
    p.length = 8;
    NoteGrid grid(song->notePool, &song->noteFreeHead, &p.noteHead);
    Note wait0 = { makeNote(0, 4), 0, EFFECT_WAIT, 0 };
    Note pass3 = { makeNote(0, 4), 0, /*effect=*/0, 0 };  // PASS = no effect
    Note wait5 = { makeNote(0, 4), 0, EFFECT_WAIT, 0 };
    grid.set(0, INPUT_COLUMN, wait0);
    grid.set(3, INPUT_COLUMN, pass3);
    grid.set(5, INPUT_COLUMN, wait5);
    for (int i = 0; i < 12; i++) {
        p.inputNotes[i].switchMode      = BlockSwitch::STAY;
        p.inputNotes[i].transposeAction = TransposeAction::KEEP;
    }
}

// PASS ahead of playhead absorbs the performer note — no snap to the later WAIT.
static void test_pass_swallows_matching_note() {
    printf("\ntest_pass_swallows_matching_note\n");
    buildPassAheadPattern();
    _mockMs = 0;
    sequencerStart();
    sequencerTick();                       // halt at row 0 WAIT
    _mockMs += 100;
    _test_onPerformerNote(60);             // resolve row 0 → seqRow=1
    CHECK(_test_getRow() == 1, "row 1 after resolving row 0 WAIT");

    // Performer plays C again while playhead is at row 1.  PASS at row 3 (>= 1)
    // must absorb the note — no snap to the WAIT at row 5.
    uint32_t absBefore = _test_getAbsRow();
    _mockMs += ROW_MS;                     // past cooldown
    _test_onPerformerNote(60);
    CHECK(_test_getAbsRow() == absBefore,         "no snap occurred (absRow unchanged)");
    CHECK(_test_getLastSnapRow() == 0,            "still anchored to the row-0 snap");
    CHECK(_test_getRow() == 1,                    "playhead still at row 1");
}

// After the PASS has consumed one matching note, a second matching note must
// still be able to reach the WAIT past it — the consumed marker is skipped on
// subsequent walks.
static void test_consumed_pass_lets_next_note_reach_wait() {
    printf("\ntest_consumed_pass_lets_next_note_reach_wait\n");
    buildPassAheadPattern();
    _mockMs = 0;
    sequencerStart();
    sequencerTick();                       // halt at row 0 WAIT
    _mockMs += 100;
    _test_onPerformerNote(60);             // resolve row 0 → seqRow=1

    _mockMs += ROW_MS;                     // past cooldown
    _test_onPerformerNote(60);             // C consumed by PASS at row 3
    CHECK(_test_getRow() == 1,            "first C swallowed by PASS — playhead unchanged");

    _mockMs += ROW_MS;                     // past cooldown again
    _test_onPerformerNote(60);             // second C should reach WAIT at row 5
    CHECK(_test_getLastSnapRow() == 5,    "second C snapped forward to WAIT at row 5");
    CHECK(_test_getRow() == 6,            "advanced to row 6 after snapping to row 5");
}

// A PASS the playhead has walked past must not absorb later performer notes —
// the (nn.row >= seqRow) guard.  Without it, a passed-by PASS would silently
// eat notes meant for the later WAIT.
static void test_passed_pass_does_not_consume() {
    printf("\ntest_passed_pass_does_not_consume\n");
    // Pattern: row 0 WAIT C-4, row 2 PASS C-4, row 5 WAIT C-4
    memset(srvActiveBuf, 0, sizeof(srvActiveBuf));
    SongFileHeader* hdr = (SongFileHeader*)srvActiveBuf;
    hdr->magic = SONG_FILE_MAGIC; hdr->version = SONG_FILE_VERSION;
    Song* song = (Song*)(srvActiveBuf + sizeof(SongFileHeader));
    song->bpm = 120; song->minBPM = 60; song->maxBPM = 240;
    song->speed = 6; song->numPatterns = 1;
    initPool(song);
    Pattern& p = song->patterns[0];
    p.length = 8;
    NoteGrid grid(song->notePool, &song->noteFreeHead, &p.noteHead);
    Note wait0 = { makeNote(0, 4), 0, EFFECT_WAIT, 0 };
    Note pass2 = { makeNote(0, 4), 0, /*effect=*/0, 0 };
    Note wait5 = { makeNote(0, 4), 0, EFFECT_WAIT, 0 };
    grid.set(0, INPUT_COLUMN, wait0);
    grid.set(2, INPUT_COLUMN, pass2);
    grid.set(5, INPUT_COLUMN, wait5);
    for (int i = 0; i < 12; i++) {
        p.inputNotes[i].switchMode      = BlockSwitch::STAY;
        p.inputNotes[i].transposeAction = TransposeAction::KEEP;
    }

    _mockMs = 0;
    sequencerStart();
    sequencerTick();                       // halt at row 0 WAIT
    _mockMs += 100;
    _test_onPerformerNote(60);             // resolve → seqRow=1

    // Tick through row 1 → row 2 (PASS plays through naturally) → row 3
    tickNextRow();
    CHECK(_test_getRow() == 2, "tick advanced to row 2 (PASS row)");
    tickNextRow();
    CHECK(_test_getRow() == 3, "tick walked past PASS to row 3");

    // Performer plays C: the passed PASS at row 2 must NOT absorb it — the
    // walk should reach the WAIT at row 5 and snap.
    _mockMs += ROW_MS;                     // past cooldown
    _test_onPerformerNote(60);
    CHECK(_test_getLastSnapRow() == 5, "snapped forward to WAIT at row 5 (passed PASS did not consume)");
    CHECK(_test_getRow() == 6,         "advanced to row 6");
}

// ── AVRG (running-mean tempo) tests ──────────────────────────────────────────
// AVRG is a col-0 effect that, when the tick plays its row, overrides
// seqCurrentBPM with the mean of the last 4 WAIT-derived BPMs.  Lets a new
// block enter at the same tempo the performer was already at.

static void buildAvrgAtRow0Pattern() {
    memset(srvActiveBuf, 0, sizeof(srvActiveBuf));
    SongFileHeader* hdr = (SongFileHeader*)srvActiveBuf;
    hdr->magic = SONG_FILE_MAGIC; hdr->version = SONG_FILE_VERSION;
    Song* song = (Song*)(srvActiveBuf + sizeof(SongFileHeader));
    song->bpm = 120; song->minBPM = 60; song->maxBPM = 240;
    song->speed = 6; song->numPatterns = 1;
    initPool(song);
    Pattern& p = song->patterns[0];
    p.length = 8;
    NoteGrid grid(song->notePool, &song->noteFreeHead, &p.noteHead);
    Note avrg0 = { NOTE_ANY, 0, EFFECT_AVRG, 0 };
    grid.set(0, INPUT_COLUMN, avrg0);
    for (int i = 0; i < 12; i++) {
        p.inputNotes[i].switchMode      = BlockSwitch::STAY;
        p.inputNotes[i].transposeAction = TransposeAction::KEEP;
    }
}

static void test_avrg_sets_bpm_to_mean_of_recent_4() {
    printf("\ntest_avrg_sets_bpm_to_mean_of_recent_4\n");
    buildAvrgAtRow0Pattern();
    _mockMs = 0;
    sequencerStart();

    _test_pushBpmHistory(150);
    _test_pushBpmHistory(160);
    _test_pushBpmHistory(170);
    _test_pushBpmHistory(180);
    // (150 + 160 + 170 + 180) / 4 = 165

    CHECK(_test_getBPM() == 120, "BPM at song default before AVRG fires");

    sequencerTick();                  // plays row 0 (AVRG)
    CHECK(_test_getBPM() == 165, "AVRG set BPM to mean of last 4 entries");
}

static void test_avrg_handles_partial_history() {
    printf("\ntest_avrg_handles_partial_history\n");
    buildAvrgAtRow0Pattern();
    _mockMs = 0;
    sequencerStart();

    _test_pushBpmHistory(140);
    _test_pushBpmHistory(160);
    // Only 2 entries — mean = 150

    sequencerTick();
    CHECK(_test_getBPM() == 150, "AVRG averages whatever entries are present");
}

static void test_avrg_no_op_when_history_empty() {
    printf("\ntest_avrg_no_op_when_history_empty\n");
    buildAvrgAtRow0Pattern();
    _mockMs = 0;
    sequencerStart();
    // No history entries.

    uint16_t bpmBefore = _test_getBPM();
    sequencerTick();
    CHECK(_test_getBPM() == bpmBefore, "AVRG with empty history is a no-op");
}

static void test_avrg_keeps_only_last_4() {
    printf("\ntest_avrg_keeps_only_last_4\n");
    buildAvrgAtRow0Pattern();
    _mockMs = 0;
    sequencerStart();

    _test_pushBpmHistory(80);           // pushed out of the ring
    _test_pushBpmHistory(100);
    _test_pushBpmHistory(120);
    _test_pushBpmHistory(140);
    _test_pushBpmHistory(160);
    // Ring holds the last 4: {100, 120, 140, 160}; mean = 130

    sequencerTick();
    CHECK(_test_getBPM() == 130, "AVRG uses only the last 4 entries");
}

// With SET transpose, playing D sets seqTranspose=2, which makes C-4 resolve
// (semi 0 + transpose 2 = 2 = D's pitch class). Verifies transpose is always applied.
static void test_transpose_applied_unconditionally() {
    printf("\ntest_transpose_applied_unconditionally\n");
    buildWaitPattern(true);     // SET transpose: each pitch sets seqTranspose=pitchClass
    _mockMs = 0;
    sequencerStart();
    sequencerTick();            // halt at row 0 C-4 WAIT

    _mockMs += 100;
    _test_onPerformerNote(62);  // D4, pitch class 2
                                // → sets seqTranspose=2; WAIT check: (0+2)%12=2 = pitchClass → match
    CHECK(!_test_isWaiting(),  "D resolves C-4 WAIT via transpose SET");
    CHECK(_test_getRow() == 1, "advanced to row 1");
}

// WAIT timeout resets to row 0 and row callback fires.
static void test_wait_timeout_resets_to_top() {
    printf("\ntest_wait_timeout_resets_to_top\n");
    buildWaitPattern(false);
    _mockMs = 0;
    gLastCallbackRow = 0xFF;
    seqSetRowCallback(rowCallback);
    sequencerStart();
    sequencerTick();            // halt at row 0

    _mockMs += 501;             // > 500ms timeout
    sequencerTick();            // timeout fires → seqRow=0, seqWaiting=false, callback
    CHECK(!_test_isWaiting(),      "not waiting after timeout");
    CHECK(_test_getRow() == 0,     "reset to row 0");
    CHECK(gLastCallbackRow == 0,   "row callback fired for row 0");
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main() {
    printf("=== Sequencer WAIT/SYNC unit tests ===\n");
    test_wait_resolves_current_row_only();
    test_sequential_waits_not_skipped();
    test_wrong_pitch_does_not_resolve_wait();
    test_sync_rule2_snap_one_row_before();
    test_sync_rule1_extends_row();
    test_sync_rule2_does_not_update_bpm();
    test_sync_far_ahead_ignored();
    test_preresolve_wait_while_running();
    test_fumble_double_note_ignored();
    test_passed_sync_does_not_snap_backward();
    test_passed_sync_does_not_block_later_wait();
    test_pass_swallows_matching_note();
    test_consumed_pass_lets_next_note_reach_wait();
    test_passed_pass_does_not_consume();
    test_avrg_sets_bpm_to_mean_of_recent_4();
    test_avrg_handles_partial_history();
    test_avrg_no_op_when_history_empty();
    test_avrg_keeps_only_last_4();
    test_transpose_applied_unconditionally();
    test_wait_timeout_resets_to_top();
    printf("\n%d passed, %d failed\n", gPassed, gFailed);
    return gFailed > 0 ? 1 : 0;
}

#endif // ARDUINO
