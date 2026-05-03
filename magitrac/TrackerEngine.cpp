#include "TrackerEngine.h"
#include "NoteGrid.h"

TrackerEngine::TrackerEngine(Song* song)
    : _song(song)
    , _state(PlayState::STOPPED)
    , _row(0)
    , _patternIdx(0)
    , _lastRowMs(0)
    , _globalTranspose(0)
    , _waiting(false)
{}

void TrackerEngine::play() {
    if (_state == PlayState::PLAYING) return;
    _state     = PlayState::PLAYING;
    _lastRowMs = millis();
}

void TrackerEngine::stop() {
    _state      = PlayState::STOPPED;
    _waiting    = false;
    _row        = 0;
    _patternIdx = _song->startPattern;
}

bool TrackerEngine::tick(uint32_t now) {
    if (_state != PlayState::PLAYING) return false;

    uint32_t elapsed = now - _lastRowMs;
    if (elapsed < msPerRow()) return false;

    _lastRowMs = now;

    Pattern& pat = _song->patterns[_patternIdx];
    _row++;
    if (_row >= pat.length) {
        _row = 0;   // loop the current block — performer drives any block switch
    }

    return true;
}

uint8_t TrackerEngine::transposedSemitone(uint8_t trackerNote) const {
    int semitone = ((trackerNote - 1) % 12) + _globalTranspose;
    return (uint8_t)((semitone % 12 + 12) % 12);  // keep in 0–11 range
}

PerformerResult TrackerEngine::onPerformerNote(uint8_t midiNote) {
    PerformerResult result = { false, false, false };

    uint8_t pitchClass = midiNote % 12;
    const Pattern& pat = _song->patterns[_patternIdx];
    const InputNoteEntry& entry = pat.inputNotes[pitchClass];

    // ── 1. Apply transpose ────────────────────────────────────────────────────
    if (entry.transposeAction == TransposeAction::NOTE) {
        _globalTranspose        = (int8_t)pitchClass;
        result.transposeChanged = true;
    } else if (entry.transposeAction == TransposeAction::CUSTOM) {
        _globalTranspose        = entry.transposeValue;
        result.transposeChanged = true;
    }

    // ── 2. Apply block switch ─────────────────────────────────────────────────
    if (entry.switchMode != BlockSwitch::STAY) {
        uint8_t target = entry.switchTarget;
        if (target < _song->numPatterns) {
            _patternIdx = target;
            if (entry.switchMode == BlockSwitch::TOP) {
                _row = 0;
            }
            // SAME_POS keeps _row as-is
            _waiting = false;
            result.patternChanged = true;
        }
    }

    // ── 3. Check snap / WAIT on column 0 ─────────────────────────────────────
    NoteGrid snapGrid(_song->notePool, &_song->patterns[_patternIdx].noteHead);
    Note col0 = snapGrid.get(_row, INPUT_COLUMN);
    if (col0.note != NOTE_EMPTY) {
        uint8_t expected = transposedSemitone(col0.note);
        if (pitchClass == expected) {
            if (_waiting) {
                // Resolve the WAIT — advance to next row
                const Pattern& curPat = _song->patterns[_patternIdx];
                _row++;
                if (_row >= curPat.length) _row = 0;
                _waiting = false;
                _lastRowMs = millis();
                result.rowSnapped = true;
            } else {
                // Snap to this row immediately (no WAIT — just alignment)
                _lastRowMs = millis();
                result.rowSnapped = true;
            }
        }
    }

    return result;
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

void TrackerEngine::setBPM(uint16_t bpm) {
    if (bpm < 32)  bpm = 32;
    if (bpm > 999) bpm = 999;
    _song->bpm = bpm;
}

void TrackerEngine::setSpeed(uint8_t speed) {
    if (speed < 1)  speed = 1;
    if (speed > 31) speed = 31;
    _song->speed = speed;
}

uint32_t TrackerEngine::msPerRow() const {
    // Classic ProTracker timing: ms/row = (2500 * speed) / bpm
    // At bpm=125, speed=6 → 120ms/row ≈ 8.3 rows/sec
    return (2500UL * _song->speed) / _song->bpm;
}
