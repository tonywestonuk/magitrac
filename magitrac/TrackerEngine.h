#pragma once
#include <Arduino.h>
#include "TrackerData.h"

// View cursor: which pattern (block) and row the client is looking at.
// The client does not sequence — playback, performer-sync, tempo and
// transposition all run on the server.  The main loop feeds the server's
// reported position in through setPosition(); the block [<] [>] buttons move
// it with setPattern().
class TrackerEngine {
public:
    explicit TrackerEngine(Song* song);

    // Back to row 0 of the song's start pattern (new song loaded / cleared).
    void stop();

    uint8_t   currentRow()     const { return _row; }
    uint8_t   currentPattern() const { return _patternIdx; }

    // Jump to row 0 of the given pattern.  Ignored if out of range.
    void setPattern(uint8_t patternIdx);

    // Update position from the server.  Each of pattern / row is applied only
    // if it is in range.
    void setPosition(uint8_t pattern, uint8_t row);

private:
    Song*     _song;
    uint8_t   _row;
    uint8_t   _patternIdx;
};
