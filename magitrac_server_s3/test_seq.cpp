// Unit tests for the sequencer: WAIT/SYNC performer-snap logic, live note
// edits (sequencerApplyNoteSet) against the play cursor, and MIDI-out checks
// via the mock UART capture.
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
// Sample player stub.  By default nothing is cached and the manifest is empty
// (as before: SFX plays go nowhere).  A test sets _mockRamCached to make RAM
// plays succeed and be recorded, _mockHaveFiles to make every sample id
// resolve to a file (so uncached plays reach the stream), and _mockTrim to
// give one sample metadata.
static int  _mockStreamPlays = 0, _mockStreamStops = 0, _mockStopAlls = 0;
static bool _mockHaveFiles   = false;
void samplePlayerPlay(const char* /*path*/, float /*pitch*/, uint8_t /*vol*/,
                      uint32_t /*startFrame*/, uint32_t /*endFrame*/, bool /*loop*/) { _mockStreamPlays++; }
void samplePlayerStop()       { _mockStopAlls++; }
void samplePlayerStopStream() { _mockStreamStops++; }
static bool    _mockRamCached   = false;
static int     _mockRamPlays    = 0;
static int     _mockVoiceStops  = 0;
static uint8_t _mockRamId       = 0;
static uint8_t _mockRamVol      = 0;
static float   _mockRamPitch    = 0;
int  samplePlayRam(uint8_t id, float pitch, uint8_t vol) {
    if (!_mockRamCached) return -1;
    _mockRamId = id; _mockRamPitch = pitch; _mockRamVol = vol;
    return ++_mockRamPlays;                  // a distinct voice handle each time
}
void sampleStopVoice(int /*voice*/) { _mockVoiceStops++; }
bool sampleRecKeysNote(uint8_t, uint8_t) { return false; }   // no ADD SAMPLE review up
const char* sampleManifestNameFor(uint8_t /*id*/) { return _mockHaveFiles ? "x.wav" : nullptr; }
#include "SampleManifest.h"   // SampleTrim type for the stub below
static SampleTrim _mockTrim = {};            // id 0 = no metadata for anything
const SampleTrim* sampleTrimFor(uint8_t id) {
    return (_mockTrim.id && _mockTrim.id == id) ? &_mockTrim : nullptr;
}
void pixelpostSetTouchpad(uint8_t, uint8_t, bool) {}
void pixelpostSetSlider(uint8_t) {}
void pixelpostSetEffect(uint8_t) {}
bool pixelpostManualActive() { return false; }

// Organ synth stubs (drawbar_organ.cpp is hardware-bound).  Only the active
// flag is modelled — it starts "inactive" and follows organSetActive(), so the
// sequencer's grab / release of the organ can be observed.
static bool _mockOrganActive   = false;
static int  _mockOrganOffCalls = 0;     // active → inactive transitions
bool organActive() { return _mockOrganActive; }
void organSetActive(bool on) {
    if (!on && _mockOrganActive) _mockOrganOffCalls++;
    _mockOrganActive = on;
}
void organNoteOn(uint8_t, uint8_t) {}
void organNoteOff(uint8_t) {}
void organSetSustain(bool) {}
int  organGetType() { return 0; }
void organSetType(int) {}
int  organGetProcSound() { return 0; }
void organSetProcSound(int) {}
int  organGetDrawbar(int) { return 0; }
void organSetDrawbar(int, int) {}
int  organGetParam(int) { return 0; }
void organSetParam(int, int) {}
int  organGetVibChorus() { return 0; }
void organSetVibChorus(int) {}
int  organGetLeslie() { return 0; }
void organSetLeslie(int) {}
int  organGetDrive() { return 0; }
void organSetDrive(int) {}
int  organGetReverb() { return 0; }
void organSetReverb(int) {}

// ── External state required by midi_player.cpp ───────────────────────────────
#include "TrackerData.h"

static uint8_t _srvBufStore[sizeof(SongFileHeader) + sizeof(Song)];
uint8_t*  srvActiveBuf    = _srvBufStore;   // firmware: PSRAM pointer
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
    memset(srvActiveBuf, 0, sizeof(_srvBufStore));

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
    memset(srvActiveBuf, 0, sizeof(_srvBufStore));
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
    memset(srvActiveBuf, 0, sizeof(_srvBufStore));
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
    memset(srvActiveBuf, 0, sizeof(_srvBufStore));
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
    memset(srvActiveBuf, 0, sizeof(_srvBufStore));
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
    memset(srvActiveBuf, 0, sizeof(_srvBufStore));
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
    memset(srvActiveBuf, 0, sizeof(_srvBufStore));
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
// The row deadline is compared wrap-safely: a row scheduled just past the
// millis() rollover must neither fire early nor stall.
static void test_row_clock_survives_millis_wrap() {
    printf("\ntest_row_clock_survives_millis_wrap\n");
    buildWaitPattern(false);
    _mockMs = 0xFFFFFFFFu - 150;
    sequencerStart();
    sequencerTick();                       // halt at row 0 WAIT
    _mockMs += 100;
    _test_onPerformerNote(60);             // resolve → row 1 due ROW_MS later, past the wrap
    CHECK(_test_getRow() == 1,            "at row 1 after resolving row 0");
    CHECK(_test_getNextRowMs() < _mockMs, "row deadline has wrapped below 'now'");

    sequencerTick();
    CHECK(_test_getRow() == 1,            "row 1 not played early before its wrapped deadline");

    _mockMs += ROW_MS + 1;                 // crosses the rollover
    sequencerTick();
    CHECK(_test_getRow() == 2,            "row 1 played once the wrapped deadline passed");
    sequencerStop();
    _mockMs = 0;
}

// The snap cooldown deadline is compared wrap-safely too: a cooldown whose
// deadline lands just past the millis() rollover must still suppress a fumble.
static void test_snap_cooldown_survives_millis_wrap() {
    printf("\ntest_snap_cooldown_survives_millis_wrap\n");
    buildWaitPattern(false);               // WAITs on rows 0,2,4,6
    _mockMs = 0xFFFFFFFFu - 130;
    sequencerStart();
    sequencerTick();                       // halt at row 0 WAIT
    _mockMs += 100;
    _test_onPerformerNote(60);             // snap; cooldown ends ~62 ms later, past the wrap
    CHECK(_test_getRow() == 1,            "at row 1 after resolving row 0");

    _mockMs += 5;                          // still before the rollover, inside the cooldown
    _test_onPerformerNote(60);             // fumble — would pre-resolve the row 2 WAIT
    CHECK(_test_getRow() == 1,            "fumble inside a wrapped cooldown is ignored");

    _mockMs += 80;                         // across the rollover, cooldown over
    CHECK(_mockMs < 1000,                 "clock has wrapped");
    _test_onPerformerNote(60);
    CHECK(_test_getRow() == 3,            "snap to row 2 allowed once the wrapped cooldown expired");
    sequencerStop();
    _mockMs = 0;
}

// ── MIDI-out capture ─────────────────────────────────────────────────────────
// The mock UART records every byte the sequencer writes.  txReset() must be
// called where the running-status cache is clear (straight after
// sequencerStart()), so the capture begins on a status byte.
static void txReset() { _mockUartTxLen = 0; }

// Count captured channel messages with this status byte and first data byte,
// following running status the way a synth would.
static int txCount(uint8_t status, uint8_t d1) {
    int     count = 0;
    uint8_t rs = 0, have = 0, first = 0;
    for (size_t i = 0; i < _mockUartTxLen; i++) {
        uint8_t b = _mockUartTx[i];
        if (b >= 0xF8) continue;                       // real-time
        if (b & 0x80) { rs = b; have = 0; continue; }
        if (rs < 0x80 || rs >= 0xF0) continue;         // no status / SysEx payload
        int need = ((rs & 0xF0) == 0xC0 || (rs & 0xF0) == 0xD0) ? 1 : 2;
        if (have == 0) first = b;
        if (++have < need) continue;
        have = 0;
        if (rs == status && first == d1) count++;
    }
    return count;
}
static int noteOns(uint8_t ch0, uint8_t midiNote)  { return txCount((uint8_t)(0x90 | ch0), midiNote); }
static int noteOffs(uint8_t ch0, uint8_t midiNote) { return txCount((uint8_t)(0x80 | ch0), midiNote); }

// Two 8-row patterns, no cues.  Col 1 → MIDI ch 1, col 2 → MIDI ch 2.
static Song* buildPlainSong() {
    memset(srvActiveBuf, 0, sizeof(_srvBufStore));
    SongFileHeader* hdr = (SongFileHeader*)srvActiveBuf;
    hdr->magic = SONG_FILE_MAGIC; hdr->version = SONG_FILE_VERSION;
    Song* song = (Song*)(srvActiveBuf + sizeof(SongFileHeader));
    song->bpm = 120; song->minBPM = 60; song->maxBPM = 240;
    song->speed = 6; song->numPatterns = 2;
    initPool(song);
    for (int p = 0; p < 2; p++) {
        song->patterns[p].length = 8;
        for (int i = 0; i < 12; i++) {
            song->patterns[p].inputNotes[i].switchMode      = BlockSwitch::STAY;
            song->patterns[p].inputNotes[i].transposeAction = TransposeAction::KEEP;
        }
    }
    song->columns[1].midiChannel = 1; song->columns[1].volume = 100;
    song->columns[2].midiChannel = 2; song->columns[2].volume = 100;
    return song;
}

static void putNote(Song* song, uint8_t pat, uint8_t row, uint8_t col, uint8_t note,
                    uint8_t effect = 0, uint8_t param = 0) {
    NoteGrid grid(song->notePool, &song->noteFreeHead, &song->patterns[pat].noteHead);
    Note n = { note, VEL_DEFAULT, effect, param };
    grid.set(row, col, n);
}

// Tracker note → the MIDI note a column with no transpose sends.
static uint8_t midiOf(uint8_t trackerNote) { return (uint8_t)(trackerNote + 11); }

// ── sequencerApplyNoteSet ────────────────────────────────────────────────────

// A cell written ahead of the playhead sounds on this pass — including one at
// the very row about to play, and one after the last existing note (where the
// cursor had already run off the end of the list).  A cell written behind the
// playhead does not sound until the next pass.
static void test_noteset_unplayed_row_plays() {
    printf("\ntest_noteset_unplayed_row_plays\n");
    Song* song = buildPlainSong();
    const uint8_t C = makeNote(0, 4), D = makeNote(2, 4), E = makeNote(4, 4),
                  F = makeNote(5, 4), G = makeNote(7, 4);
    putNote(song, 0, 0, 1, C);
    putNote(song, 0, 1, 1, D);
    _mockMs = 0;
    sequencerStart();
    txReset();
    sequencerTick();                       // row 0
    tickNextRow();                         // row 1 — the last note in the pattern
    CHECK(_test_getRow() == 2,                    "playhead at row 2");
    CHECK(noteOns(0, midiOf(C)) == 1 && noteOns(0, midiOf(D)) == 1, "rows 0 and 1 sounded");
    CHECK(_test_getNextNoteIdx() == NOTE_NULL,    "cursor ran off the end of the list");

    sequencerApplyNoteSet(0, 2, 1, G, VEL_DEFAULT, 0, 0);   // the row about to play
    sequencerApplyNoteSet(0, 4, 1, E, VEL_DEFAULT, 0, 0);   // further ahead
    sequencerApplyNoteSet(0, 0, 1, F, VEL_DEFAULT, 0, 0);   // already played: C → F
    CHECK(_test_getNextNoteIdx() != NOTE_NULL &&
          song->notePool[_test_getNextNoteIdx()].row == 2,  "cursor re-seated on the row-2 node");

    tickNextRow();                         // row 2
    CHECK(noteOns(0, midiOf(G)) == 1,             "note written at the next row plays");
    tickNextRow();                         // row 3
    CHECK(noteOns(0, midiOf(E)) == 0,             "row-4 note not played early");
    tickNextRow();                         // row 4
    CHECK(noteOns(0, midiOf(E)) == 1,             "note written ahead of the playhead plays");
    CHECK(noteOns(0, midiOf(F)) == 0 && noteOns(0, midiOf(C)) == 1,
                                                  "edited row 0 not replayed on this pass");
    tickNextRow(); tickNextRow(); tickNextRow();   // rows 5, 6, 7 → wraps
    CHECK(_test_getRow() == 0,                    "wrapped to row 0");
    tickNextRow();                         // row 0, second pass
    CHECK(noteOns(0, midiOf(F)) == 1 && noteOns(0, midiOf(C)) == 1,
                                                  "edited row 0 plays its new note next pass");
    sequencerStop();
}

// Deleting the very node the cursor is parked on must not leave the cursor on
// the freed node: here the freed node is immediately reused by another
// pattern, so a stale cursor would play that pattern's note and walk off into
// its list.
static void test_noteset_delete_cursor_node() {
    printf("\ntest_noteset_delete_cursor_node\n");
    Song* song = buildPlainSong();
    const uint8_t C = makeNote(0, 4), D = makeNote(2, 4), E = makeNote(4, 4),
                  A = makeNote(9, 4);
    putNote(song, 0, 0, 1, C);
    putNote(song, 0, 3, 1, D);
    putNote(song, 0, 5, 1, E);
    _mockMs = 0;
    sequencerStart();
    txReset();
    sequencerTick();                       // row 0
    tickNextRow(); tickNextRow();          // rows 1, 2
    uint16_t parked = _test_getNextNoteIdx();
    CHECK(_test_getRow() == 3 && parked != NOTE_NULL && song->notePool[parked].row == 3,
                                                  "cursor parked on the row-3 node");

    sequencerApplyNoteSet(0, 3, 1, NOTE_EMPTY, VEL_DEFAULT, 0, 0);   // delete it
    uint16_t reseated = _test_getNextNoteIdx();
    CHECK(reseated != parked && reseated != NOTE_NULL && song->notePool[reseated].row == 5,
                                                  "cursor moved on to the row-5 node");
    sequencerApplyNoteSet(1, 3, 1, A, VEL_DEFAULT, 0, 0);            // reuses the freed node
    CHECK(song->patterns[1].noteHead == parked,   "freed node reused by the other pattern");
    CHECK(_test_getNextNoteIdx() == reseated,     "cursor unaffected by the other pattern's edit");

    tickNextRow();                         // row 3
    CHECK(noteOns(0, midiOf(D)) == 0,             "deleted note does not play");
    CHECK(noteOns(0, midiOf(A)) == 0,             "no stale read of the reused node");
    tickNextRow();                         // row 4
    tickNextRow();                         // row 5
    CHECK(noteOns(0, midiOf(E)) == 1,             "playback continues with the row-5 note");
    CHECK(_test_isRunning() && _test_getRow() == 6, "still running, playhead at row 6");
    sequencerStop();
}

// An edit to a pattern that isn't the one loaded on the playhead leaves the
// cursor alone; out-of-range or no-song edits change nothing at all.
static void test_noteset_other_pattern_and_bounds() {
    printf("\ntest_noteset_other_pattern_and_bounds\n");
    Song* song = buildPlainSong();
    const uint8_t C = makeNote(0, 4), D = makeNote(2, 4), A = makeNote(9, 4);
    putNote(song, 0, 0, 1, C);
    putNote(song, 0, 3, 1, D);
    _mockMs = 0;
    sequencerStart();
    sequencerTick();                       // row 0
    uint16_t cursor = _test_getNextNoteIdx();
    CHECK(cursor != NOTE_NULL && song->notePool[cursor].row == 3, "cursor on the row-3 node");

    sequencerApplyNoteSet(1, 0, 1, A, VEL_DEFAULT, 0, 0);
    NoteGrid other(song->notePool, &song->noteFreeHead, &song->patterns[1].noteHead);
    CHECK(other.has(0, 1) && other.get(0, 1).note == A, "edit landed in pattern 1");
    CHECK(_test_getNextNoteIdx() == cursor,       "cursor untouched by an edit in another pattern");

    uint16_t freeBefore = song->noteFreeHead;
    sequencerApplyNoteSet(2, 0, 1, A, VEL_DEFAULT, 0, 0);            // pattern >= numPatterns
    sequencerApplyNoteSet(0, MAX_ROWS, 1, A, VEL_DEFAULT, 0, 0);     // row out of range
    sequencerApplyNoteSet(0, 0, MAX_COLUMNS, A, VEL_DEFAULT, 0, 0);  // col out of range
    srvHasActive = false;
    sequencerApplyNoteSet(0, 6, 1, A, VEL_DEFAULT, 0, 0);            // no active song
    srvHasActive = true;
    NoteGrid cur(song->notePool, &song->noteFreeHead, &song->patterns[0].noteHead);
    CHECK(song->noteFreeHead == freeBefore && !cur.has(6, 1), "rejected edits allocate nothing");
    CHECK(_test_getNextNoteIdx() == cursor,       "cursor untouched by rejected edits");
    sequencerStop();
}

// The cursor is also kept valid while stopped — PLAY resumes from it.
static void test_noteset_while_stopped_then_resume() {
    printf("\ntest_noteset_while_stopped_then_resume\n");
    Song* song = buildPlainSong();
    const uint8_t C = makeNote(0, 4), G = makeNote(7, 4);
    putNote(song, 0, 0, 1, C);
    _mockMs = 0;
    sequencerStart();
    sequencerTick();                       // row 0
    tickNextRow();                         // row 1 → playhead at row 2
    sequencerStop();
    sequencerApplyNoteSet(0, 2, 1, G, VEL_DEFAULT, 0, 0);
    sequencerResume();
    txReset();                             // resume's forced column setup cleared running status
    sequencerTick();                       // row 2
    CHECK(noteOns(0, midiOf(G)) == 1,             "note entered while stopped plays on resume");
    sequencerStop();
}

// ── Note-off for MIDI note 0 ─────────────────────────────────────────────────
// C-0 with a -24 column transpose clamps to MIDI note 0.  It must still be
// tracked as sounding, so the OFF cell (and STOP) release it.
static void test_midi_note_zero_gets_note_off() {
    printf("\ntest_midi_note_zero_gets_note_off\n");
    Song* song = buildPlainSong();
    song->columns[1].transpose = -24;
    putNote(song, 0, 0, 1, makeNote(0, 0));
    putNote(song, 0, 2, 1, NOTE_OFF);
    putNote(song, 0, 4, 1, makeNote(0, 0));
    _mockMs = 0;
    sequencerStart();
    txReset();
    sequencerTick();                       // row 0
    CHECK(noteOns(0, 0) == 1,                     "note-on for MIDI note 0 sent");
    tickNextRow();                         // row 1
    CHECK(noteOffs(0, 0) == 0,                    "still sounding before the OFF row");
    tickNextRow();                         // row 2 — OFF
    CHECK(noteOffs(0, 0) == 1,                    "OFF cell releases MIDI note 0");
    tickNextRow(); tickNextRow();          // rows 3, 4
    CHECK(noteOns(0, 0) == 2,                     "note 0 retriggered at row 4");
    sequencerStop();
    CHECK(noteOffs(0, 0) == 2,                    "STOP releases a sounding MIDI note 0");
}

// ── FFFF row-stop and the organ ──────────────────────────────────────────────
// Col 1 (MIDI) carries FFFF at row 2; col 2 is an ORGAN column, so the
// sequencer grabs the organ at start.
static Song* buildOrganStopSong() {
    Song* song = buildPlainSong();
    song->columns[2].midiChannel = ORGAN_CHANNEL;
    putNote(song, 0, 0, 2, makeNote(0, 4));
    putNote(song, 0, 2, 1, NOTE_EMPTY, 0xFF, 0xFF);
    return song;
}

static void test_ffff_stop_defers_only_the_organ_release() {
    printf("\ntest_ffff_stop_defers_only_the_organ_release\n");
    gLastCallbackRow = 0xFF;
    static bool sawStopped;
    sawStopped = false;
    seqSetStateCallback([](bool playing) { if (!playing) sawStopped = true; });

    buildOrganStopSong();
    _mockMs = 0;
    sequencerStart();
    CHECK(organActive() && sequencerOrganOwned(), "start grabs the organ for the ORGAN column");
    sequencerTick();                       // row 0
    tickNextRow();                         // row 1
    int offBefore = _mockOrganOffCalls;
    tickNextRow();                         // row 2 — FFFF
    CHECK(!_test_isRunning() && sawStopped,       "FFFF stops the sequencer inline");
    CHECK(organActive() && _mockOrganOffCalls == offBefore,
                                                  "organ release not run inside the tick");
    sequencerRawAuditionTick();
    CHECK(!organActive() && !sequencerOrganOwned() && _mockOrganOffCalls == offBefore + 1,
                                                  "organ released by the MIDI-task loop");
    sequencerRawAuditionTick();
    CHECK(_mockOrganOffCalls == offBefore + 1,    "released exactly once");

    // A normal STOP still releases the organ immediately.
    _mockMs = 0;
    sequencerStart();
    CHECK(organActive(),                          "organ grabbed again on start");
    sequencerStop();
    CHECK(!organActive() && !sequencerOrganOwned(), "sequencerStop() releases the organ at once");

    // PLAY arriving before the parked release runs keeps the organ.
    _mockMs = 0;
    sequencerStart();
    sequencerTick(); tickNextRow(); tickNextRow();   // → FFFF
    CHECK(!_test_isRunning() && organActive(),    "stopped by FFFF, release parked");
    sequencerResume();
    sequencerRawAuditionTick();
    CHECK(organActive() && sequencerOrganOwned(), "resume cancels the parked release");
    sequencerStop();
    CHECK(!organActive(),                         "organ released on the final stop");
    seqSetStateCallback(nullptr);
}

// A WAIT on the LAST row of a block: once the performer resolves it the block
// has ended, so the next row to play is row 0 of the following block — with
// its notes.  (The snap used to wrap the row number without re-seating the
// note cursor or running the block-end navigation, so the whole next pass was
// silent.)
static void test_wait_on_last_row_then_next_pass_plays() {
    printf("\ntest_wait_on_last_row_then_next_pass_plays\n");
    Song* song = buildPlainSong();
    uint8_t n = makeNote(0, 4);
    for (uint8_t p = 0; p < 2; p++) putNote(song, p, 0, 1, n);      // row 0 note in both blocks
    {
        NoteGrid grid(song->notePool, &song->noteFreeHead, &song->patterns[0].noteHead);
        Note w = { makeNote(0, 4), 0, EFFECT_WAIT, 0 };
        grid.set(7, INPUT_COLUMN, w);                               // WAIT on the last row
    }
    _mockMs = 0; txReset();
    sequencerStart();
    sequencerTick();                                 // row 0
    CHECK(noteOns(0, midiOf(n)) == 1, "row 0 note played on the first pass");
    for (int i = 0; i < 6; i++) tickNextRow();       // rows 1..6
    tickNextRow();                                   // row 7 → WAIT halts
    CHECK(_test_isWaiting(),        "halted at the last-row WAIT");
    CHECK(_test_getRow() == 7,      "halted row is 7");

    _mockMs += 100;
    _test_onPerformerNote(60);                       // performer resolves it
    CHECK(!_test_isWaiting(),       "WAIT resolved");
    CHECK(_test_getRow() == 0,      "next row to play is row 0");

    txReset();
    tickNextRow();                                   // plays row 0 of the next pass
    CHECK(noteOns(0, midiOf(n)) == 1, "row 0 note plays on the pass after a last-row WAIT");
    CHECK(_test_isRunning(),        "still running");
}

// ── PAN effect (EFFECT_PAN → MIDI CC 10) ─────────────────────────────────────

// Position of the first captured channel message {status, d1, d2} among all
// channel messages (running status followed), or -1 if it never went out.
static int txFind(uint8_t status, uint8_t d1, int d2 /* -1 = any */) {
    int     index = 0;
    uint8_t rs = 0, have = 0, first = 0;
    for (size_t i = 0; i < _mockUartTxLen; i++) {
        uint8_t b = _mockUartTx[i];
        if (b >= 0xF8) continue;
        if (b & 0x80) { rs = b; have = 0; continue; }
        if (rs < 0x80 || rs >= 0xF0) continue;
        int need = ((rs & 0xF0) == 0xC0 || (rs & 0xF0) == 0xD0) ? 1 : 2;
        if (have == 0) first = b;
        if (++have < need) continue;
        have = 0;
        if (rs == status && first == d1 && (d2 < 0 || b == (uint8_t)d2)) return index;
        index++;
    }
    return -1;
}

static void test_pan_effect_sends_cc10() {
    printf("\ntest_pan_effect_sends_cc10\n");
    Song* song = buildPlainSong();                    // col 1 = ch 1, col 2 = ch 2
    uint8_t n = makeNote(0, 4);
    putNote(song, 0, 0, 1, n, EFFECT_PAN, 0x10);          // note + pan left-ish on ch 1
    putNote(song, 0, 1, 2, NOTE_EMPTY, EFFECT_PAN, 0xC8); // pan only, over range, on ch 2
    putNote(song, 0, 2, 1, n, EFFECT_PAN, 0x40);          // back to centre on ch 1
    _mockMs = 0; txReset();
    sequencerStart();
    sequencerTick();                                   // row 0
    int pan  = txFind(0xB0, 10, 0x10);
    int note = txFind(0x90, midiOf(n), -1);
    CHECK(pan >= 0,                 "row 0: CC 10 = 0x10 sent on channel 1");
    CHECK(note >= 0 && pan < note,  "row 0: pan goes out before the note-on");
    CHECK(txCount(0xB1, 10) == 0,   "row 0: nothing on channel 2 yet");

    txReset(); tickNextRow();                          // row 1
    CHECK(txFind(0xB1, 10, 127) >= 0, "row 1: param C8 is sent as 127 on channel 2 (no note needed)");

    txReset(); tickNextRow();                          // row 2
    CHECK(txFind(0xB0, 10, 64) >= 0,  "row 2: CC 10 = 64 (centre) on channel 1");

    // The sequencer never sends a pan of its own accord: stopping and loading
    // a song (the forced patch flush) leave pan to the synth / its preset.
    sequencerStop();
    txReset();
    sequencerSendPatchesNow();
    sequencerRawAuditionTick();
    int cc10 = 0;
    for (uint8_t ch = 0; ch < 16; ch++) cc10 += txCount((uint8_t)(0xB0 | ch), 10);
    CHECK(cc10 == 0,                  "stop + song load send no CC 10");
    CHECK(txCount(0xC1, song->columns[2].program) >= 1,
                                      "song load does still send the column's program change");
}

static void test_pan_effect_ignored_where_it_does_not_apply() {
    printf("\ntest_pan_effect_ignored_where_it_does_not_apply\n");
    Song* song = buildPlainSong();
    song->columns[1].mute = 1;                         // muted MIDI column
    song->columns[3].midiChannel = SFX_CHANNEL;        // not a MIDI column
    putNote(song, 0, 0, 1, NOTE_EMPTY, EFFECT_PAN, 0x00);
    putNote(song, 0, 0, 3, NOTE_EMPTY, EFFECT_PAN, 0x00);
    _mockMs = 0; txReset();
    sequencerStart();
    sequencerTick();
    int cc10 = 0;
    for (uint8_t ch = 0; ch < 16; ch++) cc10 += txCount((uint8_t)(0xB0 | ch), 10);
    CHECK(cc10 == 0, "no CC 10 from a muted column or a non-MIDI column");
    sequencerStop();
}

// ── Sample page: MIDI keys play a sample, pads, and the sample's own TUNE ────

// Queue raw MIDI-in bytes and run the parser over them.
static void midiIn(const uint8_t* b, size_t n) {
    memcpy(_mockUartRx, b, n);
    _mockUartRxLen = n; _mockUartRxPos = 0;
    sequencerPollMidiIn();
}
static void midiInNoteOn(uint8_t note, uint8_t vel)  { uint8_t m[3] = { 0x90, note, vel }; midiIn(m, 3); }
static void midiInNoteOff(uint8_t note)              { uint8_t m[3] = { 0x80, note, 0 };   midiIn(m, 3); }

static int     gStepInCalls = 0;
static uint8_t gStepInNote  = 0;
static void stepInCallback(uint8_t note, uint8_t /*vel*/) { gStepInCalls++; gStepInNote = note; }

static void sampleMockReset() {
    _mockHaveFiles = false; _mockStreamPlays = 0; _mockStreamStops = 0; _mockStopAlls = 0;
    _mockRamCached = true; _mockRamPlays = 0; _mockVoiceStops = 0;
    _mockRamId = 0; _mockRamPitch = 0;
    _mockTrim = SampleTrim{};
    gStepInCalls = 0;
    seqSetMidiNoteInCallback(stepInCallback);
}
static void sampleMockDone() {
    sequencerSampleKeys(0);
    seqSetMidiNoteInCallback(nullptr);
    _mockRamCached = false;
    _mockHaveFiles = false;
    _mockTrim = SampleTrim{};
}
static bool near(float a, float b) { return fabsf(a - b) < 0.001f; }

// While the client's sample editor holds the key lease, keys play its sample
// at the played pitch and are NOT step input; each key's release stops its
// own voice.
static void test_sample_keys_play_the_sample_not_step_input() {
    printf("\ntest_sample_keys_play_the_sample_not_step_input\n");
    buildPlainSong();
    sampleMockReset();
    _mockMs = 1000;

    midiInNoteOn(64, 90);
    CHECK(gStepInCalls == 1 && _mockRamPlays == 0, "no lease: the key is step input, no sample");

    sequencerSampleKeys(7);
    midiInNoteOn(60, 90);
    CHECK(_mockRamPlays == 1 && _mockRamId == 7, "lease: the key plays the sample");
    CHECK(near(_mockRamPitch, 0.0f),             "middle C = native pitch");
    CHECK(_mockRamVol == 114,                    "…at velocity 90, a little under the sample's own level");
    midiInNoteOff(60);
    midiInNoteOn(60, 100);
    CHECK(_mockRamVol == 127,                    "velocity 100 = the sample's own level exactly");
    midiInNoteOff(60);
    midiInNoteOn(60, 127);
    CHECK(_mockRamVol == 161,                    "velocity 127 = louder than that (boost)");
    midiInNoteOff(60);
    midiInNoteOn(67, 5);
    CHECK(_mockRamPlays == 4 && near(_mockRamPitch, 7.0f), "G above = +7 semitones, even played softly");
    CHECK(_mockRamVol == 6,                      "…and softly it is");
    CHECK(gStepInCalls == 1,                     "no key reached step input");
    int stopsBefore = _mockVoiceStops;
    midiInNoteOn(67, 0);                         // note-on velocity 0 = note-off
    CHECK(_mockVoiceStops == stopsBefore + 1 && _mockRamPlays == 4, "velocity-0 note-on is a release, not a play");

    sequencerSampleKeys(0);
    midiInNoteOn(62, 90);
    CHECK(gStepInCalls == 2 && _mockRamPlays == 4, "lease ended: keys are step input again");
    sampleMockDone();
}

// The lease runs out by itself if the client stops renewing it.
static void test_sample_keys_lease_expires() {
    printf("\ntest_sample_keys_lease_expires\n");
    buildPlainSong();
    sampleMockReset();
    _mockMs = 1000;
    sequencerSampleKeys(7);
    _mockMs = 1000 + SAMPLE_KEYS_LEASE_MS - 1;
    CHECK(sequencerSampleKeysId() == 7, "lease still live just inside the window");
    sequencerSampleKeys(7);                      // renewal restarts the clock
    _mockMs += SAMPLE_KEYS_LEASE_MS - 1;
    midiInNoteOn(60, 90);
    CHECK(_mockRamPlays == 1,           "renewed lease: keys still play the sample");
    midiInNoteOff(60);
    _mockMs += 2;                                // now past the renewed lease
    midiInNoteOn(60, 90);
    CHECK(_mockRamPlays == 1 && gStepInCalls == 1, "expired: the key is step input again");
    CHECK(sequencerSampleKeysId() == 0, "and the lease reports off");
    sampleMockDone();
}

// With a sample on the keys, a running song gets both: the key is still the
// performer's cue, and it plays the sample — and its release stops that note.
// Pads play over the song too.
static void test_sample_keys_and_pads_over_a_running_song() {
    printf("\ntest_sample_keys_and_pads_over_a_running_song\n");
    Song* song = buildPlainSong();
    putNote(song, 0, 0, INPUT_COLUMN, NOTE_ANY, EFFECT_WAIT);     // waits at row 0 for any key
    song->midiInNoteMin = 0; song->midiInNoteMax = 127;           // every key is in the cue range
    sampleMockReset();
    _mockMs = 0;
    sequencerSampleKeys(7);
    sequencerStart();
    sequencerTick();
    CHECK(_test_isWaiting(), "the song is waiting on its cue");
    midiInNoteOn(60, 90);
    CHECK(_mockRamPlays == 1 && _mockRamId == 7, "running: the key plays the sample");
    CHECK(!_test_isWaiting(),                    "…and is still the song's cue");
    midiInNoteOff(60);
    CHECK(_mockVoiceStops == 1,                  "releasing the key stops its note, song running or not");

    sequencerSamplePlay(7, 60);
    CHECK(_mockRamPlays == 2, "running: a pad plays too");
    midiInNoteOff(60);
    CHECK(_mockVoiceStops == 1, "a pad has no key: a note-off for 60 doesn't cut it");
    midiInNoteOn(64, 90);                        // a held key…
    sequencerSamplePadsStop();
    CHECK(_mockVoiceStops >= 3 && _mockStopAlls == 0,
          "running: STOP releases the page's pad and key voices, not the whole player");
    sequencerStop();
    sequencerSamplePlay(7, 60);
    CHECK(_mockRamPlays == 4, "stopped: the pad plays too");
    sequencerSamplePadsStop();
    CHECK(_mockStopAlls == 1, "stopped: STOP silences the whole player");
    sampleMockDone();
}

// A key whose (uncached) sample was streaming: once the song takes the stream
// for one of its own samples, that key's release must not cut the song's.
static void test_key_release_does_not_cut_a_song_stream() {
    printf("\ntest_key_release_does_not_cut_a_song_stream\n");
    Song* song = buildPlainSong();
    sampleMockReset();
    _mockRamCached = false;                      // everything streams
    _mockHaveFiles = true;
    song->columns[3].midiChannel = SFX_CHANNEL;
    song->columns[3].program     = 5;
    song->columns[3].volume      = 127;
    putNote(song, 0, 2, 3, makeNote(0, 4));      // the song streams sample 5 at row 2
    _mockMs = 0;
    sequencerSampleKeys(9);
    sequencerStart();
    sequencerTick();
    midiInNoteOn(60, 90);
    CHECK(_mockStreamPlays == 1, "the key's sample streams");
    for (int i = 0; i < 2; i++) { _mockMs = _test_getNextRowMs(); sequencerTick(); }
    CHECK(_mockStreamPlays == 2, "the song's sample takes the stream at row 2");
    midiInNoteOff(60);
    CHECK(_mockStreamStops == 0, "releasing the key leaves the song's stream alone");
    midiInNoteOn(62, 90);
    midiInNoteOff(62);
    CHECK(_mockStreamPlays == 3 && _mockStreamStops == 1, "a fresh key still starts and stops its own");
    sequencerStop();
    sampleMockDone();
}

// An uncached pad sample takes the single SD stream.  The song column whose
// streamed sample it replaced must not cut it with its own note-off, and the
// pads' STOP must stop it; once the song takes the stream back, the pads' STOP
// leaves it alone.
static void test_pad_on_the_stream_while_running() {
    printf("\ntest_pad_on_the_stream_while_running\n");
    Song* song = buildPlainSong();
    sampleMockReset();
    _mockRamCached = false;                      // nothing preloaded: everything streams
    _mockHaveFiles = true;
    song->columns[3].midiChannel = SFX_CHANNEL;
    song->columns[3].program     = 5;
    song->columns[3].volume      = 127;
    putNote(song, 0, 0, 3, makeNote(0, 4));      // row 0: the song streams sample 5
    putNote(song, 0, 2, 3, NOTE_OFF);            // row 2: and stops it
    putNote(song, 0, 4, 3, makeNote(0, 4));      // row 4: streams it again
    _mockMs = 0;
    sequencerStart();
    sequencerTick();
    CHECK(_mockStreamPlays == 1, "song starts its streamed sample");

    sequencerSamplePlay(9, 60);                  // pad: takes the stream
    CHECK(_mockStreamPlays == 2, "pad sample streams (replacing the song's)");
    uint32_t rowMs = _test_getNextRowMs();       // step to row 2 (the column's OFF)
    for (int i = 0; i < 2; i++) { _mockMs = _test_getNextRowMs(); sequencerTick(); }
    CHECK(_test_getRow() >= 2 && _mockStreamStops == 0,
          "the column's note-off does not cut the pad's stream");
    (void)rowMs;

    sequencerSamplePadsStop();
    CHECK(_mockStreamStops == 1 && _mockStopAlls == 0, "pads' STOP stops the pad's stream");

    for (int i = 0; i < 2; i++) { _mockMs = _test_getNextRowMs(); sequencerTick(); }
    CHECK(_mockStreamPlays == 3, "song streams its sample again at row 4");
    sequencerSamplePadsStop();
    CHECK(_mockStreamStops == 1, "pads' STOP now leaves the song's stream alone");
    sequencerStop();
    sampleMockDone();
}

// VEL OFF in the sample editor: a key plays the sample at its own level,
// however hard it is struck.  Other samples still follow velocity.
static void test_sample_velocity_switch() {
    printf("\ntest_sample_velocity_switch\n");
    buildPlainSong();
    sampleMockReset();
    _mockTrim.id = 7; _mockTrim.velOff = 1;
    _mockMs = 1000;

    sequencerSampleKeys(7);
    midiInNoteOn(60, 20);
    CHECK(_mockRamPlays == 1 && _mockRamVol == 127, "soft key: the sample's own level");
    midiInNoteOff(60);
    midiInNoteOn(60, 127);
    CHECK(_mockRamVol == 127,                       "hard key: no boost either");
    midiInNoteOff(60);

    sequencerSampleKeys(8);                         // no metadata: velocity on
    midiInNoteOn(60, 127);
    CHECK(_mockRamId == 8 && _mockRamVol == 161,    "another sample still follows velocity");
    midiInNoteOff(60);
    sequencerSampleKeys(0);
    sampleMockDone();
}

// The sample's own TUNE applies wherever it plays, on top of a column's TUNE.
static void test_sample_tune_adds_to_pitch() {
    printf("\ntest_sample_tune_adds_to_pitch\n");
    Song* song = buildPlainSong();
    sampleMockReset();
    _mockTrim.id = 7; _mockTrim.tuneCents = -150;
    _mockMs = 1000;

    sequencerSamplePlay(7, 60);
    CHECK(near(_mockRamPitch, -1.5f), "pad: native pitch + the sample's TUNE");
    CHECK(_mockRamVol == 127,         "a pad is always full level");
    sequencerSamplePlay(8, 60);
    CHECK(near(_mockRamPitch, 0.0f),  "another sample is untouched");

    song->columns[3].midiChannel  = SFX_CHANNEL;
    song->columns[3].program      = 7;
    song->columns[3].volume       = 127;
    song->columns[3].sfxTuneCents = 50;
    sequencerAuditionRawNote(SFX_CHANNEL, 62, 100, 3);
    CHECK(near(_mockRamPitch, 2.0f - 1.5f + 0.5f), "column audition: key + sample TUNE + column TUNE");

    putNote(song, 0, 0, 3, makeNote(0, 4));      // C-4 = the sample's root note
    _mockMs = 0;
    sequencerStart();
    sequencerTick();
    CHECK(near(_mockRamPitch, -1.5f + 0.5f), "song: a C-4 cell plays at sample TUNE + column TUNE");
    sequencerStop();
    sampleMockDone();
}

// ── Block switch inside the snap cooldown ────────────────────────────────────

// A stray key caught just BEFORE the key that switches block: the stray one
// takes effect and opens the fumble window, then the real one arrives inside
// it.  The switch must still play the new block's first row — here that row carries an
// OFF for the column the old block left sounding.  (It used to move to the new
// block and then sit waiting on its row 0, so the OFF never went out and the
// note hung until the next key.)
static void test_block_switch_inside_cooldown_plays_new_block() {
    printf("\ntest_block_switch_inside_cooldown_plays_new_block\n");
    Song* song = buildPlainSong();                          // col 1 = ch 1, col 2 = ch 2
    uint8_t n = makeNote(0, 4);
    putNote(song, 0, 0, INPUT_COLUMN, NOTE_ANY, EFFECT_WAIT);
    putNote(song, 0, 0, 1, n);                              // block 0 row 0: note on col 1
    song->patterns[0].inputNotes[8].switchMode   = BlockSwitch::TOP;   // G# → block 1
    song->patterns[0].inputNotes[8].switchTarget = 1;
    putNote(song, 1, 0, INPUT_COLUMN, NOTE_ANY, EFFECT_WAIT);
    putNote(song, 1, 0, 1, NOTE_OFF);                       // block 1 row 0: OFF on col 1
    putNote(song, 1, 0, 2, n);                              //               note on col 2

    _mockMs = 0;
    sequencerStart();
    sequencerTick();
    txReset();                                              // count only what follows                                        // halts on block 0 row 0
    _mockMs = 100;
    _test_onPerformerNote(69);                              // stray A: resolves the ANY wait
    CHECK(noteOns(0, midiOf(n)) == 1,  "stray note played block 0 row 0 (col 1 sounding)");
    _mockMs = 110;                                          // well inside the cooldown
    _test_onPerformerNote(68);                              // G#: the intended block switch
    CHECK(_test_getPattern() == 1,     "switched to block 1");
    CHECK(!_test_isWaiting(),          "not left waiting on the new block's row 0");
    CHECK(noteOffs(0, midiOf(n)) == 1, "new block's row 0 OFF silenced col 1");
    CHECK(noteOns(1, midiOf(n)) == 1,  "new block's row 0 note played on col 2");
    CHECK(_test_getRow() == 1,         "moved on to row 1 of the new block");
}

// The bounce of a key that restarts its OWN block: inside the cooldown it is
// ignored, rather than rewinding to a row 0 that will not be played.
static void test_same_block_restart_bounce_is_ignored() {
    printf("\ntest_same_block_restart_bounce_is_ignored\n");
    Song* song = buildPlainSong();
    uint8_t n = makeNote(0, 4);
    putNote(song, 0, 0, INPUT_COLUMN, NOTE_ANY, EFFECT_WAIT);
    putNote(song, 0, 0, 1, n);
    putNote(song, 0, 1, 1, NOTE_OFF);                       // row 1: OFF
    song->patterns[0].inputNotes[0].switchMode   = BlockSwitch::TOP;   // C → top of block 0
    song->patterns[0].inputNotes[0].switchTarget = 0;

    _mockMs = 0;
    sequencerStart();
    sequencerTick();
    txReset();                                              // count only what follows
    _mockMs = 100;
    _test_onPerformerNote(60);                              // C: restart + play row 0
    CHECK(_test_getRow() == 1 && !_test_isWaiting(), "row 0 played, on to row 1");
    _mockMs = 110;
    _test_onPerformerNote(60);                              // key bounce
    CHECK(_test_getRow() == 1 && !_test_isWaiting(), "bounce did not rewind the block");
    CHECK(noteOns(0, midiOf(n)) == 1,  "row 0 note was not retriggered");
    tickNextRow();
    CHECK(noteOffs(0, midiOf(n)) == 1, "row 1 OFF still plays");

    // Outside the cooldown the same key does restart the block.
    _mockMs += 400;
    _test_onPerformerNote(60);
    CHECK(noteOns(0, midiOf(n)) == 2,  "after the cooldown the key restarts the block again");
}

// ── Fumble rule: the last key counts ─────────────────────────────────────────

// Two blocks in the shape of the real song that prompted this: in block 0, G
// plays on (it is the WAIT note) and A-flat leads to block 1, whose first row
// silences column 1 and starts a note on column 2.
static Song* buildFumbleSong() {
    Song* song = buildPlainSong();                          // col 1 = ch 1, col 2 = ch 2
    uint8_t G = makeNote(7, 4), n = makeNote(0, 4);
    putNote(song, 0, 0, INPUT_COLUMN, G, EFFECT_WAIT);
    putNote(song, 0, 0, 1, n);
    putNote(song, 0, 1, 1, NOTE_OFF);
    putNote(song, 0, 4, INPUT_COLUMN, G, EFFECT_WAIT);
    putNote(song, 0, 4, 1, n);
    putNote(song, 0, 5, 1, NOTE_OFF);
    song->patterns[0].inputNotes[8].switchMode   = BlockSwitch::TOP;   // A-flat → block 1
    song->patterns[0].inputNotes[8].switchTarget = 1;
    putNote(song, 1, 0, INPUT_COLUMN, NOTE_ANY, EFFECT_WAIT);
    putNote(song, 1, 0, 1, NOTE_OFF);
    putNote(song, 1, 0, 2, n);
    return song;
}

// Play block 0 up to its second WAIT (row 4) and stop there, capture cleared.
static void fumbleSongToRow4() {
    _mockMs = 0;
    sequencerStart();
    sequencerTick();                                        // halts on row 0
    _mockMs = 100;
    _test_onPerformerNote(67);                              // G: row 0
    for (int i = 0; i < 4; i++) tickNextRow();              // rows 1..3, then halts on row 4
    _mockMs += 300;                                         // well clear of the first fumble window
    txReset();
}

static void test_fumble_last_key_counts_stray_switch_then_real_key() {
    printf("\ntest_fumble_last_key_counts_stray_switch_then_real_key\n");
    buildFumbleSong();
    uint8_t n = makeNote(0, 4);
    fumbleSongToRow4();
    CHECK(_test_isWaiting() && _test_getRow() == 4 && _test_getPattern() == 0, "waiting on block 0 row 4");

    _test_onPerformerNote(68);                              // A-flat (the slip): block 1 starts
    CHECK(_test_getPattern() == 1,     "A-flat alone would take us to block 1");
    CHECK(noteOns(1, midiOf(n)) == 1,  "…and block 1's row 0 has sounded on col 2");
    _mockMs += 10;
    _test_onPerformerNote(67);                              // G (what was meant)
    CHECK(_test_getPattern() == 0,     "G straight after: back in block 0");
    CHECK(_test_getRow() == 5 && !_test_isWaiting(), "block 0's row 4 was played, now on row 5");
    CHECK(noteOns(0, midiOf(n)) == 1,  "row 4's note sounded on col 1");
    CHECK(noteOffs(1, midiOf(n)) == 1, "the note the slip started on col 2 was silenced");
    // (the snap re-derived the tempo from the long gap before it, so a row is
    // now longer than ROW_MS — step until row 5 has gone)
    for (int i = 0; i < 6 && _test_getRow() == 5; i++) tickNextRow();
    CHECK(_test_getRow() == 6,         "block 0 carries on to row 6");
    CHECK(noteOffs(0, midiOf(n)) == 1, "…having played row 5's OFF");
}

static void test_fumble_last_key_counts_real_key_then_switch() {
    printf("\ntest_fumble_last_key_counts_real_key_then_switch\n");
    buildFumbleSong();
    uint8_t n = makeNote(0, 4);
    fumbleSongToRow4();
    _test_onPerformerNote(67);                              // G: row 4 plays (col 1 sounding)
    CHECK(_test_getPattern() == 0 && _test_getRow() == 5, "G played block 0 row 4");
    _mockMs += 10;
    _test_onPerformerNote(68);                              // A-flat: the last key
    CHECK(_test_getPattern() == 1,     "A-flat straight after: block 1");
    CHECK(_test_getRow() == 1 && !_test_isWaiting(), "block 1's row 0 was played");
    CHECK(noteOffs(0, midiOf(n)) == 1, "col 1 silenced (block 1's row 0 OFF)");
    CHECK(noteOns(1, midiOf(n)) == 1,  "block 1's row 0 note sounded on col 2");
}

static void test_fumble_third_key_is_judged_from_the_start() {
    printf("\ntest_fumble_third_key_is_judged_from_the_start\n");
    buildFumbleSong();
    fumbleSongToRow4();
    _test_onPerformerNote(68);  _mockMs += 8;               // A-flat → block 1
    _test_onPerformerNote(67);  _mockMs += 8;               // G      → back to block 0
    CHECK(_test_getPattern() == 0,     "after A-flat, G: block 0");
    _test_onPerformerNote(68);                              // A-flat again: it is the last key
    CHECK(_test_getPattern() == 1 && _test_getRow() == 1, "after A-flat, G, A-flat: block 1, row 0 played");
}

static void test_fumble_wrong_note_after_does_not_undo() {
    printf("\ntest_fumble_wrong_note_after_does_not_undo\n");
    buildFumbleSong();
    uint8_t n = makeNote(0, 4);
    fumbleSongToRow4();
    _test_onPerformerNote(67);                              // G: row 4 plays
    _mockMs += 10;
    _test_onPerformerNote(66);                              // F-sharp brushed: means nothing here
    CHECK(_test_getPattern() == 0 && _test_getRow() == 5 && !_test_isWaiting(), "G still stands");
    CHECK(noteOns(0, midiOf(n)) == 1 && noteOffs(0, midiOf(n)) == 0, "nothing replayed or cut");
}

static void test_fumble_chord_on_any_cue_plays_once() {
    printf("\ntest_fumble_chord_on_any_cue_plays_once\n");
    Song* song = buildPlainSong();
    uint8_t n = makeNote(0, 4);
    putNote(song, 0, 0, INPUT_COLUMN, NOTE_ANY, EFFECT_WAIT);
    putNote(song, 0, 0, 1, n);
    _mockMs = 0;
    sequencerStart();
    sequencerTick();
    txReset();
    _mockMs = 100; _test_onPerformerNote(60);               // C
    _mockMs += 6;  _test_onPerformerNote(64);               // E  — same cue, same result
    _mockMs += 6;  _test_onPerformerNote(67);               // G
    CHECK(noteOns(0, midiOf(n)) == 1,  "a chord on an any-note cue plays the row once");
    CHECK(_test_getRow() == 1,         "and advances one row");
}

// The owner's confirming scenario: block A sends F to block B and G to block C,
// anything else stays.  The performer fumbles E, F, G → must end on block C.
// (Blocks A/B/C are patterns 0/1/2 here.)  Run twice: with E meaning something
// in block A (an any-note cue it resolves) and with E meaning nothing there.
static Song* buildThreeWaySong(bool anyCueInBlockA) {
    Song* song = buildPlainSong();
    uint8_t n = makeNote(0, 4);
    song->numPatterns = 3;
    song->patterns[2].length = 8;
    for (int i = 0; i < 12; i++) {
        song->patterns[2].inputNotes[i].switchMode      = BlockSwitch::STAY;
        song->patterns[2].inputNotes[i].transposeAction = TransposeAction::KEEP;
    }
    song->columns[3].midiChannel = 3; song->columns[3].volume = 100;
    if (anyCueInBlockA) putNote(song, 0, 0, INPUT_COLUMN, NOTE_ANY, EFFECT_WAIT);
    putNote(song, 0, 0, 1, n);                                          // block A sounds on ch 1
    song->patterns[0].inputNotes[5].switchMode   = BlockSwitch::TOP;    // F → block B
    song->patterns[0].inputNotes[5].switchTarget = 1;
    song->patterns[0].inputNotes[7].switchMode   = BlockSwitch::TOP;    // G → block C
    song->patterns[0].inputNotes[7].switchTarget = 2;
    putNote(song, 1, 0, INPUT_COLUMN, NOTE_ANY, EFFECT_WAIT);
    putNote(song, 1, 0, 2, n);                                          // block B sounds on ch 2
    putNote(song, 2, 0, INPUT_COLUMN, NOTE_ANY, EFFECT_WAIT);
    putNote(song, 2, 0, 3, n);                                          // block C sounds on ch 3
    return song;
}

static void fumbleEFG(bool anyCueInBlockA, const uint8_t keys[3], uint8_t expectPattern, const char* what) {
    buildThreeWaySong(anyCueInBlockA);
    uint8_t n = makeNote(0, 4);
    _mockMs = 0;
    sequencerStart();
    sequencerTick();
    _mockMs = 100; txReset();
    for (int i = 0; i < 3; i++) { _test_onPerformerNote(keys[i]); _mockMs += 8; }
    CHECK(_test_getPattern() == expectPattern, what);
    if (expectPattern == 2) {
        CHECK(noteOns(2, midiOf(n)) == 1,                       "   block C's first row sounded");
        CHECK(noteOns(1, midiOf(n)) == noteOffs(1, midiOf(n)),  "   nothing from block B left sounding");
    }
    sequencerStop();
}

static void test_fumble_e_f_g_lands_on_g_block() {
    printf("\ntest_fumble_e_f_g_lands_on_g_block\n");
    const uint8_t EFG[3] = { 64, 65, 67 }, EGF[3] = { 64, 67, 65 }, GFE[3] = { 67, 65, 64 };
    fumbleEFG(true,  EFG, 2, "E, F, G (E resolves a cue in block A)  → block C");
    fumbleEFG(false, EFG, 2, "E, F, G (E means nothing in block A)   → block C");
    fumbleEFG(true,  EGF, 1, "E, G, F                                → block B (F was last)");
    fumbleEFG(false, GFE, 1, "G, F, E where E means nothing          → block B (E is ignored, F stands)");
    fumbleEFG(true,  GFE, 0, "G, F, E where E resolves block A's cue → block A (E was last)");
}

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
    test_row_clock_survives_millis_wrap();
    test_snap_cooldown_survives_millis_wrap();
    test_noteset_unplayed_row_plays();
    test_noteset_delete_cursor_node();
    test_noteset_other_pattern_and_bounds();
    test_noteset_while_stopped_then_resume();
    test_midi_note_zero_gets_note_off();
    test_ffff_stop_defers_only_the_organ_release();
    test_wait_on_last_row_then_next_pass_plays();
    test_block_switch_inside_cooldown_plays_new_block();
    test_same_block_restart_bounce_is_ignored();
    test_fumble_last_key_counts_stray_switch_then_real_key();
    test_fumble_last_key_counts_real_key_then_switch();
    test_fumble_third_key_is_judged_from_the_start();
    test_fumble_wrong_note_after_does_not_undo();
    test_fumble_chord_on_any_cue_plays_once();
    test_fumble_e_f_g_lands_on_g_block();
    test_pan_effect_sends_cc10();
    test_pan_effect_ignored_where_it_does_not_apply();
    test_sample_keys_play_the_sample_not_step_input();
    test_sample_keys_lease_expires();
    test_sample_keys_and_pads_over_a_running_song();
    test_key_release_does_not_cut_a_song_stream();
    test_pad_on_the_stream_while_running();
    test_sample_tune_adds_to_pitch();
    test_sample_velocity_switch();
    printf("\n%d passed, %d failed\n", gPassed, gFailed);
    return gFailed > 0 ? 1 : 0;
}

#endif // ARDUINO
