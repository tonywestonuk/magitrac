#include "TrackerEngine.h"

TrackerEngine::TrackerEngine(Song* song)
    : _song(song)
    , _row(0)
    , _patternIdx(0)
{}

void TrackerEngine::stop() {
    _row        = 0;
    _patternIdx = _song->startPattern;
}

void TrackerEngine::setPattern(uint8_t patternIdx) {
    if (patternIdx >= _song->numPatterns) return;
    _patternIdx = patternIdx;
    _row        = 0;
}

void TrackerEngine::setPosition(uint8_t pattern, uint8_t row) {
    if (pattern < _song->numPatterns) _patternIdx = pattern;
    if (row < _song->patterns[_patternIdx].length) _row = row;
}
