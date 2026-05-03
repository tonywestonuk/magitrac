#pragma once
#include <Arduino.h>

// ── HoldRepeat — reusable hold-to-repeat button helper ──────────────────────
//
// Tracks a held +/- button and fires repeated adjustments after an initial
// delay.  Two timing presets:
//   FAST: 400ms initial, 80ms repeat  (ColumnEditor, SettingsPage)
//   SLOW: 1000ms initial, 250ms repeat (SongConfigPage, InstrumentsPage)

class HoldRepeat {
public:
    HoldRepeat() = default;

    // Call on rising edge when a +/- button is pressed.
    void start(int field, int delta, int type = 0) {
        _field = field;
        _delta = delta;
        _type  = type;
        _start = millis();
        _last  = _start;
        _fired = false;
    }

    // Call on falling edge (finger up).  Returns saved state before clearing.
    // Use wasFired() right after release() to decide whether to fire a single tap.
    void release() {
        _savedField = _field;
        _savedDelta = _delta;
        _savedType  = _type;
        _savedFired = _fired;
        _field = -1;
    }

    // Call every loop iteration while finger is down.
    // Returns true when a repeat should fire — caller should adjustField + redraw.
    bool tick(uint32_t initialMs, uint32_t repeatMs) {
        if (_field < 0) return false;
        uint32_t now = millis();
        if (!_fired && now - _start >= initialMs) {
            _fired = true;
            _last  = now;
            return true;
        }
        if (_fired && now - _last >= repeatMs) {
            _last = now;
            return true;
        }
        return false;
    }

    // Convenience: fast preset (400/80)
    bool tickFast() { return tick(400, 80); }

    // Convenience: slow preset (1000/250)
    bool tickSlow() { return tick(1000, 250); }

    // Active state
    bool active()    const { return _field >= 0; }
    int  field()     const { return _field; }
    int  delta()     const { return _delta; }
    int  type()      const { return _type; }

    // Saved state (valid after release(), before next start())
    bool wasFired()    const { return _savedFired; }
    int  savedField()  const { return _savedField; }
    int  savedDelta()  const { return _savedDelta; }
    int  savedType()   const { return _savedType; }

private:
    int      _field = -1;
    int      _delta = 0;
    int      _type  = 0;       // optional: section identifier (e.g. 1=BPM, 2=MIDI)
    uint32_t _start = 0;
    uint32_t _last  = 0;
    bool     _fired = false;

    int  _savedField = -1;
    int  _savedDelta = 0;
    int  _savedType  = 0;
    bool _savedFired = false;
};
