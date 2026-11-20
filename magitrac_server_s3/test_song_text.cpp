// Host test for song_text.cpp (the AI's song ⇄ text form).
//
// Run from Terminal — NOT through the Arduino IDE (it writes a test_song_text
// binary here — don't commit it):
//   cd /Users/tonyweston/magitrac/magitrac_server_s3
//   g++ -std=c++11 -Wno-format -DUNIT_TEST -I test_mocks -I . -I ../magitrac_lib/src \
//       test_song_text.cpp -o test_song_text && ./test_song_text
#ifndef ARDUINO
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_mocks/Arduino.h"
HardwareSerial Serial;
#include "TrackerData.h"
#include "TrackerData.cpp"
#include "NoteGrid.cpp"
#include "song_text.cpp"

static int sPass = 0, sFail = 0;
static void check(bool ok, const char* what) {
    printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    ok ? sPass++ : sFail++;
}

static void put(Song* s, int b, int row, int col, uint8_t note, uint8_t vel = VEL_DEFAULT,
                uint8_t fx = 0, uint8_t prm = 0) {
    NoteGrid g(s->notePool, &s->noteFreeHead, &s->patterns[b].noteHead);
    Note n = { note, vel, fx, prm };
    g.set((uint8_t)row, (uint8_t)col, n);
}

static Note get(const Song* s, int b, int row, int col, bool* has = nullptr) {
    NoteGrid g(s->notePool, &s->patterns[b].noteHead);
    if (has) *has = g.has((uint8_t)row, (uint8_t)col);
    return g.get((uint8_t)row, (uint8_t)col);
}

static int countNotes(const Song* s) {
    int n = 0;
    for (int b = 0; b < s->numPatterns; b++) {
        NoteGrid g(s->notePool, &s->patterns[b].noteHead);
        g.forAll([](uint8_t, uint8_t, const Note&, void* c) { (*(int*)c)++; }, &n);
    }
    return n;
}

static TextBuf makeBuf(size_t cap) {
    TextBuf t = { (char*)malloc(cap), cap, 0, false };
    t.p[0] = '\0';
    return t;
}

static Song* sampleSong() {
    Song* s = (Song*)calloc(1, sizeof(Song));
    initSong(s);
    strcpy(s->name, "Test \"Song\"");
    s->bpm = 128; s->minBPM = 90; s->maxBPM = 160; s->speed = 6;
    s->numPatterns = 3; s->startPattern = 1;
    s->columns[1] = ColumnSettings{ 1, 0, 33, 110, -12, 0, 0, "Bass" };
    s->columns[2] = ColumnSettings{ 10, 0, 0, 100, 0, 1, 0, "Drums" };
    s->columns[3] = ColumnSettings{ SFX_CHANNEL, 0, 7, 90, 0, 0, -350, "Hit" };
    s->columns[4] = ColumnSettings{ ORGAN_CHANNEL, 0, 0, 100, 0, 0, 0, "" };
    s->columns[5] = ColumnSettings{ PIXELPOST_CHANNEL, 0, 0, 100, 0, 0, 0, "Lights" };
    s->patterns[0].length = 16; strcpy(s->patterns[0].name, "Intro");
    s->patterns[0].blockEndNav = NAV_FWD | 1;
    s->patterns[1].length = 32; strcpy(s->patterns[1].name, "Verse");
    s->patterns[1].referenceNote = 7; s->patterns[1].keyChangeMode = 1;
    s->patterns[1].blockEndNav = NAV_LOOP;
    s->patterns[1].inputNotes[4] = { BlockSwitch::TOP, 2, TransposeAction::KEEP, 0 };
    s->patterns[1].inputNotes[3] = { BlockSwitch::STAY, 0, TransposeAction::NOTE, 0 };
    s->patterns[1].inputNotes[5] = { BlockSwitch::SAME_POS, 0, TransposeAction::CUSTOM, -3 };
    s->patterns[2].length = 64; s->patterns[2].blockEndNav = NAV_RNT;
    s->organ.flags = 1; s->organ.bars[0] = 8;          // not in the text: kept from base
    strcpy(s->perfPads[0].name, "GO");

    put(s, 0, 0, 0, makeNote(0, 4), VEL_DEFAULT, EFFECT_WAIT);
    put(s, 0, 4, 0, NOTE_ANY, VEL_DEFAULT, EFFECT_SYNC);
    put(s, 0, 8, 0, makeNote(7, 3));                      // PASS
    put(s, 0, 12, 0, makeNote(9, 3), VEL_DEFAULT, EFFECT_PASA);
    put(s, 0, 0, 1, makeNote(0, 2), 100);
    put(s, 0, 2, 1, NOTE_OFF);
    put(s, 0, 3, 1, makeNote(1, 2), 0);                   // explicit velocity 0
    put(s, 0, 0, 2, makeNote(0, 3), 127, EFFECT_RTRG, 4);
    put(s, 0, 1, 2, makeNote(6, 3), VEL_DEFAULT, EFFECT_PAN, 0x40);
    put(s, 0, 5, 5, 4, 64, 0x20, 0x30);                   // lights: XY touchpad
    put(s, 0, 6, 1, NOTE_EMPTY, 90, 0x0C, 0x01);          // effect only, no note
    put(s, 1, 31, 4, makeNote(11, 7));                    // B-7, last row of a 32-row block
    put(s, 2, 63, 20, makeNote(10, 5), 1);
    return s;
}

static void testRoundTrip() {
    printf("round trip\n");
    Song* a = sampleSong();
    TextBuf t = makeBuf(64 * 1024);
    check(songToText(a, t), "writes");
    printf("----\n%s----\n", t.p);

    Song* base = (Song*)calloc(1, sizeof(Song));
    memcpy(base, a, sizeof(Song));
    Song* b = (Song*)calloc(1, sizeof(Song));
    SongTextResult r;
    bool ok = songFromText(t.p, t.len, base, b, r);
    if (!ok) printf("  err: %s\n", r.err);
    check(ok, "parses its own output");
    check(r.skipped == 0, "no skipped lines");
    check(b->numPatterns == 3 && r.blocks == 3, "3 blocks");
    check(countNotes(b) == countNotes(a) && r.notes == countNotes(a), "same note count");
    check(b->bpm == 128 && b->minBPM == 90 && b->maxBPM == 160 && b->startPattern == 1,
          "song settings");
    check(strcmp(b->name, "Test 'Song'") == 0, "quotes in names become apostrophes");
    check(memcmp(b->columns, a->columns, sizeof(a->columns)) == 0, "columns identical");
    bool pats = true;
    for (int i = 0; i < 3; i++) {
        const Pattern &x = a->patterns[i], &y = b->patterns[i];
        pats &= x.length == y.length && !strcmp(x.name, y.name) && x.referenceNote == y.referenceNote &&
                x.keyChangeMode == y.keyChangeMode && x.blockEndNav == y.blockEndNav &&
                !memcmp(x.inputNotes, y.inputNotes, sizeof(x.inputNotes));
    }
    check(pats, "blocks identical (length, name, ref, keymode, end, keys)");
    bool notes = true;
    for (int bk = 0; bk < 3; bk++)
        for (int row = 0; row < 64; row++)
            for (int col = 0; col < MAX_COLUMNS; col++) {
                bool ha, hb;
                Note na = get(a, bk, row, col, &ha), nb = get(b, bk, row, col, &hb);
                if (ha != hb || (ha && memcmp(&na, &nb, sizeof(Note)))) {
                    printf("    differs at %d/%d/%d\n", bk, row, col);
                    notes = false;
                }
            }
    check(notes, "every cell identical");
    check(b->organ.flags == 1 && b->organ.bars[0] == 8 && !strcmp(b->perfPads[0].name, "GO"),
          "organ + perf pads kept from base");
    free(a); free(b); free(base); free(t.p);
}

static bool parse(const char* text, Song* base, Song* out, SongTextResult& r) {
    return songFromText(text, strlen(text), base, out, r);
}

static void testModelReply() {
    printf("a reply the way a model writes it\n");
    Song* base = sampleSong();
    Song* out  = (Song*)calloc(1, sizeof(Song));
    SongTextResult r;
    const char* reply =
        "Here's a happy song in G.\n"
        "```\n"
        "MAGITRAC SONG 1\n"
        "SUMMARY A bright 2-block pop song in G major at 124 BPM.\n"
        "SONG name=\"Happy G\" bpm=124 minbpm=100 maxbpm=140 speed=6 start=0\n"
        "COL 1 ch=1 prog=0 name=\"Piano\"\n"
        "COL 2 ch=10 name=\"Drums\"\n"
        "BLOCK 0 len=16 name=\"Verse\" ref=G end=NEXT:1\n"
        "KEY 0 C switch=TOP target=1\n"
        "N 0 0 0 G-3 - WAIT\n"
        "N 0 0 1 G4 96 -\n"
        "N 0 2 1 Bb4 - -\n"
        "N 0 4 1 D-5 -- -\n"
        "N 0 0 2 C-3 110 -\n"
        "BLOCK 1 len=16 name=\"Chorus\" end=GOTO:0\n"
        "N 1 0 1 C5 - -\n"
        "END\n"
        "```\n"
        "Enjoy!\n";
    bool ok = parse(reply, base, out, r);
    if (!ok) printf("  err: %s\n", r.err);
    check(ok, "parses");
    check(!strcmp(r.summary, "A bright 2-block pop song in G major at 124 BPM."), "summary captured");
    check(out->numPatterns == 2 && out->bpm == 124 && !strcmp(out->name, "Happy G"), "song fields");
    check(out->columns[1].midiChannel == 1 && out->columns[1].volume == 100 &&
          out->columns[3].midiChannel == 0, "unlisted columns cleared, defaults filled");
    check(get(out, 0, 0, 1).note == makeNote(7, 4), "G4 without a dash");
    check(get(out, 0, 2, 1).note == makeNote(10, 4), "Bb4 flat");
    check(get(out, 0, 0, 0).effect == EFFECT_WAIT, "WAIT cue");
    check(out->patterns[0].blockEndNav == (NAV_FWD | 1) && out->patterns[1].blockEndNav == NAV_ABS,
          "end NEXT:1 / GOTO:0");
    check(out->patterns[0].inputNotes[0].switchMode == BlockSwitch::TOP &&
          out->patterns[0].inputNotes[0].switchTarget == 1, "KEY line");
    check(out->patterns[1].inputNotes[4].switchMode == BlockSwitch::STAY, "old keys cleared");
    check(out->organ.flags == 1, "organ kept");
    check(countNotes(out) == 6, "6 notes");
    free(base); free(out);
}

static void testFailures() {
    printf("replies that must be refused\n");
    Song* base = sampleSong();
    Song* out  = (Song*)calloc(1, sizeof(Song));
    SongTextResult r;
    check(!parse("I can't help with that.", base, out, r), "no song at all");
    printf("    (%s)\n", r.err);
    check(!parse("MAGITRAC SONG 1\nBLOCK 0 len=16\nN 0 0 1 C-4 - -\n", base, out, r),
          "truncated (no END)");
    printf("    (%s)\n", r.err);
    check(!parse("MAGITRAC SONG 1\nBLOCK 0 len=16\nBLOCK 2 len=16\nEND\n", base, out, r),
          "a gap in the block numbers");
    printf("    (%s)\n", r.err);
    check(!parse("MAGITRAC SONG 1\nBLOCK 0 len=20\nEND\n", base, out, r), "bad block length");
    check(!parse("MAGITRAC SONG 1\nEND\n", base, out, r), "no blocks");

    // A few bad lines are skipped; lots of them refuse the song.
    check(parse("MAGITRAC SONG 1\nBLOCK 0 len=16\nN 0 0 1 C-4 - -\nN 0 99 1 C-4 - -\n"
                "N 0 1 0 C-4 - PAN:3\nEND\n", base, out, r) && r.skipped == 2 && r.notes == 1,
          "a couple of bad notes skipped");
    printf("    (%s)\n", r.err);
    char big[8192] = "MAGITRAC SONG 1\nBLOCK 0 len=16\nN 0 0 1 C-4 - -\n";
    for (int i = 0; i < 20; i++) strcat(big, "N 0 0 1 H-9 - -\n");
    strcat(big, "END\n");
    check(!parse(big, base, out, r), "many bad lines refuse the song");
    printf("    (%s)\n", r.err);
    check(parse("MAGITRAC SONG 1\nBLOCK 0 len=16\nN 0 16 1 C-4 - -\nN 0 15 1 C-4 - -\nEND\n",
                base, out, r) && r.notes == 1, "row past the block length skipped");

    // Pool exhaustion: 50 blocks × 64 rows × 2 columns = 6400 > 4000.
    size_t cap = 400 * 1024;
    char* huge = (char*)malloc(cap);
    TextBuf t = { huge, cap, 0, false };
    textPrintf(t, "MAGITRAC SONG 1\n");
    for (int b = 0; b < 50; b++) {
        textPrintf(t, "BLOCK %d len=64\n", b);
        for (int row = 0; row < 64; row++)
            for (int c = 1; c <= 2; c++) textPrintf(t, "N %d %d %d C-4 - -\n", b, row, c);
    }
    textPrintf(t, "END\n");
    check(!parse(huge, base, out, r), "more than 4000 notes refused");
    printf("    (%s)\n", r.err);
    free(huge);

    // Writer: a buffer too small reports it.
    TextBuf small = makeBuf(64);
    check(!songToText(base, small) && small.overflow, "writer reports a full buffer");
    free(small.p);
    free(base); free(out);
}

int main() {
    testRoundTrip();
    testModelReply();
    testFailures();
    printf("\n%d passed, %d failed\n", sPass, sFail);
    return sFail ? 1 : 0;
}
#endif
