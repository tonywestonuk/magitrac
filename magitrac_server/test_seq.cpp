// Unit tests for sequencer WAIT/SYNC performer-snap logic.
//
// Run from Terminal — NOT through the Arduino IDE:
//   cd /Users/tonyweston/Documents/Arduino/magitrac_server
//   g++ -std=c++11 -DUNIT_TEST -I test_mocks -I . test_seq.cpp -o test_seq && ./test_seq
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

// While running between row 1 and row 2, performer plays C → forward search
// finds row 2 SYNC or WAIT, snaps to it, advances to row 3.
// If the next col-0 note is a WAIT and the performer plays it early (pre-resolve),
// the sequencer snaps there and continues WITHOUT halting.
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

static void test_sync_jump_while_running() {
    printf("\ntest_sync_jump_while_running\n");
    buildSyncAtRow2Pattern();
    _mockMs = 0;
    sequencerStart();

    // Resolve row 0 WAIT → seqRow=1, running
    sequencerTick();
    _mockMs += 100;
    _test_onPerformerNote(60);
    CHECK(_test_getRow() == 1, "at row 1 after resolving row 0 WAIT");
    CHECK(!_test_isWaiting(),  "not waiting");

    // Performer plays C again after cooldown expires — forward search finds row 2 SYNC
    _mockMs += ROW_MS + 1;    // past the one-row cooldown from the WAIT resolve
    _test_onPerformerNote(60);
    CHECK(!_test_isWaiting(),  "snapped to SYNC row and continued");
    CHECK(_test_getRow() == 3, "forward-snapped to row 2 SYNC, now at row 3");
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
    test_sync_jump_while_running();
    test_preresolve_wait_while_running();
    test_fumble_double_note_ignored();
    test_transpose_applied_unconditionally();
    test_wait_timeout_resets_to_top();
    printf("\n%d passed, %d failed\n", gPassed, gFailed);
    return gFailed > 0 ? 1 : 0;
}

#endif // ARDUINO
