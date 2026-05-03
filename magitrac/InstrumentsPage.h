#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "TrackerData.h"
#include "UIHelpers.h"
#include "KeyboardPopup.h"
#include "HoldRepeat.h"

// ── InstrumentsPage (960×540) ─────────────────────────────────────────────────
//
// LIST VIEW:
//   y=  0  ┌─ Header "INSTRUMENTS" (50px) ──────────────────────────────────┐
//   y= 50  ├─ Toolbar: [PREV x=0,w=134] [NEXT x=150,w=134] [HOME x=800,w=160] (55px)
//   y=105  ├─ 8 instrument rows × 54px ────────────────────────────────────┤
//   y=537  └──────────────────────────────────────────────────────────────────┘
//
// EDIT VIEW:
//   y=  0  ┌─ [BACK w=130] [ID] [NAME — tap to rename] [HOME x=830,w=130] (70px)
//   y= 70  ├─ 4 field rows × 78px: BANK / PROG / VOL / TRANS ───────────────┤
//   y=538  └──────────────────────────────────────────────────────────────────┘

// ── List layout ───────────────────────────────────────────────────────────────
static const int IP_HDR_H        = 50;
static const int IP_TOOL_H       = 55;
static const int IP_BTN_Y        = IP_HDR_H + 4;           // = 54
static const int IP_BTN_H        = 47;
static const int IP_LIST_Y       = IP_HDR_H + IP_TOOL_H;   // = 105
static const int IP_ROW_H        = 54;
static const int IP_PER_PAGE     = 8;

// List title-bar HOME (right side, white-on-black) — consistent with other pages
static const int IP_LIST_HOME_X  = 830;
static const int IP_LIST_HOME_W  = 130;

// Toolbar (below title bar) — pagination only
static const int IP_PREV_X       = 0;
static const int IP_PREV_W       = 134;
static const int IP_NEXT_X       = 150;
static const int IP_NEXT_W       = 134;

// ── Edit layout ───────────────────────────────────────────────────────────────
static const int IP_EDIT_HDR_H   = 70;
static const int IP_EDIT_BACK_X  = 0;
static const int IP_EDIT_BACK_W  = 130;
static const int IP_EDIT_HOME_X  = 830;
static const int IP_EDIT_HOME_W  = 130;
static const int IP_EDIT_NAME_X  = 215;   // tappable name region
static const int IP_EDIT_NAME_W  = 610;   // ends at 825, before HOME at 830

static const int IP_FIELD_Y      = IP_EDIT_HDR_H;  // = 70
static const int IP_FIELD_H      = 78;
static const int IP_NUM_FIELDS   = 4;
static const int IP_LABEL_X      = 30;
static const int IP_VAL_X        = 590;
static const int IP_VAL_W        = 185;
static const int IP_MINUS_X      = 785;
static const int IP_PLUS_X       = 868;
static const int IP_FBTN_W       = 75;
static const int IP_FBTN_H       = 58;

// ── InstrumentsPage ───────────────────────────────────────────────────────────

class InstrumentsPage {
public:
    InstrumentsPage(EPD_PainterAdafruit& display, GT911_Lite& touch,
                    Instrument* instruments);

    void open();
    void draw();

    // Returns true when HOME is tapped (caller returns to tracker).
    bool poll();

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;
    Instrument*          _instruments;
    KeyboardPopup        _keyboard;

    enum class State : uint8_t { LIST, EDIT, NAMING, CONFIRM };
    State   _state;
    uint8_t _listPage;
    uint8_t _editIdx;
    bool    _wasDown;
    bool    _anyEditMade;  // true if anything was changed since open()
    bool    _saveOnExit;   // set by confirm dialog: true=save, false=discard

    HoldRepeat _hold;

    // ── Drawing ───────────────────────────────────────────────────────────────
    void drawList();
    void drawListRow(int idx, int y);
    void drawEdit();
    void drawEditField(int field);
    void drawEditHeader();
    void drawConfirm();

    // ── Value helpers ─────────────────────────────────────────────────────────
    void fieldLabel(int field, char* out) const;
    void fieldValue(int field, char* out) const;
    void adjustField(int field, int delta);

    // ── Hit tests — list ──────────────────────────────────────────────────────
    int  hitRow     (int sx, int sy) const;
    bool hitPrev    (int sx, int sy) const;
    bool hitNext    (int sx, int sy) const;
    bool hitListHome(int sx, int sy) const;

    // ── Hit tests — edit ──────────────────────────────────────────────────────
    bool hitBack    (int sx, int sy) const;
    bool hitEditHome(int sx, int sy) const;
    bool hitName    (int sx, int sy) const;
    int  hitMinus   (int sx, int sy) const;  // field index or -1
    int  hitPlus    (int sx, int sy) const;  // field index or -1

    void rawToScreen(int rx, int ry, int& sx, int& sy) const;

public:
    // After poll() returns true: whether to save (true) or discard (false).
    bool    saveOnExit()   const { return _saveOnExit; }
};
