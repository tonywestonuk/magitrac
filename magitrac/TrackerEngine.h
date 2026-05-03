#pragma once
#include <Arduino.h>
#include "TrackerData.h"

enum class PlayState { STOPPED, PLAYING };

// Result of onPerformerNote — tells the caller what changed so it can redraw.
struct PerformerResult {
    bool rowSnapped;       // true if block jumped to a new row
    bool patternChanged;   // true if block switched to a new pattern
    bool transposeChanged; // true if globalTranspose was updated
};

class TrackerEngine {
public:
    explicit TrackerEngine(Song* song);

    void play();
    void stop();

    // Call every loop iteration with millis(). Returns true if the row advanced.
    bool tick(uint32_t now);

    PlayState state()           const { return _state; }
    uint8_t   currentRow()     const { return _row; }
    uint8_t   currentPattern() const { return _patternIdx; }
    int8_t    globalTranspose() const { return _globalTranspose; }
    bool      isWaiting()       const { return _waiting; }

    // Called whenever a MIDI note arrives from the performer.
    // midiNote is the raw MIDI note number (0–127).
    PerformerResult onPerformerNote(uint8_t midiNote);

    // Performer-driven block switch — jumps to the given pattern index.
    void setPattern(uint8_t patternIdx);

    // Update position from server (client-server mode).
    void setPosition(uint8_t pattern, uint8_t row);

    // BPM / speed controls
    void setBPM(uint16_t bpm);
    void setSpeed(uint8_t speed);

private:
    Song*     _song;
    PlayState _state;
    uint8_t   _row;
    uint8_t   _patternIdx;
    uint32_t  _lastRowMs;
    int8_t    _globalTranspose;  // semitones, persists globally
    bool      _waiting;          // true when held at a WAIT row

    // ProTracker formula: ms per row = (2500 * speed) / bpm
    uint32_t msPerRow() const;

    // Returns the effective (transposed) semitone for a column-0 note.
    uint8_t transposedSemitone(uint8_t trackerNote) const;
};
