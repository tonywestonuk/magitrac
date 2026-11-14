// Replay harness for paper/fumble-recall.md.
//
// Loads a real song file, feeds the sequencer a scripted list of performer keys
// (arguments "ms:midiNote", in time order) on a mocked 1 ms clock, and prints
// every note-on / note-off it sends plus each change of block.  The sequencer
// source is compiled in unmodified; which version is chosen by -DMIDI_PLAYER_CPP
// (see run.sh), so the same performance can be replayed against two rules.
#include "prelude.inc"
#include "SongMigration.h"
#include <vector>
#include <string>
struct Ev { uint32_t t; uint8_t note; };
static size_t txPos = 0; static uint8_t rs = 0, have = 0, d1 = 0;
static const char* NN[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
static void drainTx() {
    for (; txPos < _mockUartTxLen; txPos++) {
        uint8_t b = _mockUartTx[txPos];
        if (b >= 0xF8) continue;
        if (b & 0x80) { rs = b; have = 0; continue; }
        if (rs < 0x80 || rs >= 0xF0) continue;
        int need = ((rs & 0xF0) == 0xC0 || (rs & 0xF0) == 0xD0) ? 1 : 2;
        if (have == 0) d1 = b;
        if (++have < need) continue;
        have = 0;
        uint8_t ch = (rs & 0x0F) + 1, k = rs & 0xF0;
        if (k == 0x90 && b > 0) printf("  %6u  ch%-2u ON   %s%d\n", _mockMs, ch, NN[d1 % 12], d1 / 12 - 1);
        else if (k == 0x80 || (k == 0x90 && b == 0)) printf("  %6u  ch%-2u OFF  %s%d\n", _mockMs, ch, NN[d1 % 12], d1 / 12 - 1);
    }
}
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    File f; f.fp = fopen(argv[1], "rb"); if (!f) return 3;
    SongFileHeader hdr; f.read((uint8_t*)&hdr, sizeof(hdr));
    memset(srvActiveBuf, 0, sizeof(_srvBufStore));
    memcpy(srvActiveBuf, &hdr, sizeof(hdr));
    Song* song = (Song*)(srvActiveBuf + sizeof(SongFileHeader));
    if (!songReadCompact(f, song)) return 4;
    srvHasActive = true;
    std::vector<Ev> evs;                       // args: t:note pairs
    for (int i = 2; i < argc; i++) { unsigned t, n; if (sscanf(argv[i], "%u:%u", &t, &n) == 2) evs.push_back({t, (uint8_t)n}); }
    uint32_t endMs = evs.empty() ? 1000 : evs.back().t + 1500;
    _mockMs = 0; _mockUartTxLen = 0;
    sequencerStart();
    int lastPat = -1, lastRow = -1, lastWait = -1; size_t ei = 0;
    for (_mockMs = 0; _mockMs <= endMs; _mockMs++) {
        while (ei < evs.size() && evs[ei].t == _mockMs) {
            printf("  %6u  >>> performer plays %s%d\n", _mockMs, NN[evs[ei].note % 12], evs[ei].note / 12 - 1);
            _test_onPerformerNote(evs[ei].note); ei++;
            drainTx();
        }
        sequencerTick();
        drainTx();
        int p = _test_getPattern(), r = _test_getRow(), w = _test_isWaiting();
        if (p != lastPat || w != lastWait) {
            printf("  %6u  -- block %d row %d %s\n", _mockMs, p, r, w ? "WAITING" : "running");
            lastPat = p; lastWait = w;
        }
        lastRow = r;
    }
    (void)lastRow;
    return 0;
}
