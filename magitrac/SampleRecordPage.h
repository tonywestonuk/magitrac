#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "KeyboardPopup.h"
#include "MagiMsg.h"

// ── SampleRecordPage — ADD SAMPLE: record a new sample through the server's mic ─
//
// Opened from the samples page's ADD button.  The recording is done on the
// SERVER's screen (sample_record.cpp, MSG_SAMPLE_REC): opening this page arms
// it, and the server shows PRESS AND HOLD TO RECORD.  Holding its glass
// records; letting go plays the take back there, with KEEP / CANCEL /
// RESAMPLE.  This page follows along until KEEP hands the take back here.
//
//   ARMED      "press and hold the server's screen"
//   RECORDING  seconds + level as the server reports them
//   REVIEW     "listen on the server" — its KEEP / CANCEL / RESAMPLE decide
//   (KEEP)     straight into the keyboard (defaulting to the first free
//              "recNN"); DONE saves it into /samples.  The keyboard's CANCEL
//              leaves the take here: PLAY / STOP listen to it, AGAIN re-arms
//              the server, KEEP names it
//   (CANCEL)   the server threw it away: back to the samples page
//   SAVED      poll() returns SAVED: the sketch opens the sample editor on the
//              new sample (trim it, tune it), whose BACK leads to the squares.
//
// IDLE (refused, cancelled on the server, no reply) offers TRY AGAIN.
// BACK throws away anything not kept and takes the server off its hold screen.

static const int SRC_HDR_H  = 60;
static const int SRC_BACK_X = 820;
static const int SRC_BACK_W = 125;
static const int SRC_BTN_Y  = 410;
static const int SRC_BTN_H  = 110;
static const int SRC_NAME_MAX = SAMPLE_NAME_LEN - 4;   // base name + NUL (".wav" added)

class SampleRecordPage {
public:
    SampleRecordPage(EPD_PainterAdafruit& display, GT911_Lite& touch)
        : _d(display), _touch(touch), _keyboard(display, touch) {}

    enum class Result : uint8_t { NONE, BACK, SAVED };

    void   open();
    Result poll();

    // Valid after poll() returned SAVED.
    uint8_t     savedId()   const { return _savedId; }
    const char* savedName() const { return _name; }

private:
    enum class St : uint8_t { IDLE, ARMING, ARMED, RECORDING, REVIEW, TAKE, SAVING, NAMING };

    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;
    KeyboardPopup        _keyboard;

    St       _st        = St::IDLE;
    bool     _wasDown   = false;
    bool     _backArmed = false;
    uint32_t _waitMs    = 0;          // when the request we're waiting on went
    uint32_t _frames    = 0;
    uint32_t _rate      = 32000;
    uint8_t  _peak      = 0;          // 0..100: live while recording, the take's after
    bool     _clipped   = false;
    uint8_t  _savedId   = 0;
    bool     _leave     = false;      // cancelled on the server: poll() returns BACK
    char     _name[SRC_NAME_MAX] = {};
    char     _msg[64]   = {};         // an error or note under the title

    void draw();
    void drawHeader();
    void drawStatus();               // title + message lines
    void drawMeter();                // level bar + seconds
    void drawButtons();
    void request(uint8_t op, St waitState, const char* name = nullptr);
    void handleStatus(const MsgSampleRecStatus& s);
    void keepTake();                 // KEEP: open the keyboard on a default name
    void nameDone();                 // the keyboard closed with DONE
    void defaultName();
    bool nameTaken(const char* base) const;
    int  rowLabels(const char* out[4]) const;
    int  buttonAt(int sx, int sy) const;   // index into the current row, or -1
    void rawToScreen(int rx, int ry, int& sx, int& sy) const { sx = ry; sy = 540 - rx; }
};
