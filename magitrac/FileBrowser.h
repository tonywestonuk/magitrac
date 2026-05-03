#pragma once
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "SongStorage.h"
#include "UIHelpers.h"
#include "Constants.h"

// ── Layout constants ──────────────────────────────────────────────────────────

static const int FB_HDR_H    = 50;
static const int FB_TOOL_H   = 55;
static const int FB_BTN_Y    = FB_HDR_H + 4;           // = 54
static const int FB_BTN_H    = 47;

// Title-bar buttons (consistent across pages):
//   Left  — extended actions (here: DELETE mode toggle)
//   Right — HOME (exit page)
static const int FB_HOME_X   = 830;
static const int FB_HOME_W   = 130;
static const int FB_DEL_X    = 0;
static const int FB_DEL_W    = 130;

// Toolbar PREV/NEXT (pagination)
static const int FB_PREV_X   = 680;
static const int FB_PREV_W   = 134;
static const int FB_NEXT_X   = 818;
static const int FB_NEXT_W   = 142;

static const int FB_COLS     = 4;
static const int FB_ROWS     = 7;
static const int FB_PER_PAGE = FB_COLS * FB_ROWS;       // = 28
static const int FB_COL_W    = 960 / FB_COLS;            // = 240
static const int FB_LIST_Y   = FB_HDR_H + FB_TOOL_H;    // = 105
static const int FB_ROW_H    = 54;

static const int FB_FOOT_Y   = FB_LIST_Y + FB_ROWS * FB_ROW_H;  // = 483
static const int FB_FOOT_H   = 57;
static const int FB_FBTN_Y   = FB_FOOT_Y + 4;
static const int FB_FBTN_H   = 49;
static const int FB_NEW_X    = 0;
static const int FB_NEW_W    = 316;
static const int FB_SAVE_X   = 320;
static const int FB_SAVE_W   = 316;
static const int FB_SAVEAS_X = 640;
static const int FB_SAVEAS_W = 320;

// ── Events ────────────────────────────────────────────────────────────────────

enum class FileBrowserEvent : uint8_t {
    NONE,
    HOME,
    NEW,
    SAVE,
    SAVE_AS,
    ITEM_TAP,     // result.index = page-relative item index
    ITEM_DELETE,  // result.index = page-relative item index (delete mode)
    PREV_PAGE,
    NEXT_PAGE,
};

struct FileBrowserResult {
    FileBrowserEvent event;
    int              index;   // valid for ITEM_TAP / ITEM_DELETE
};

// ── FileBrowser ───────────────────────────────────────────────────────────────
//
// Generic 4-column × 7-row file grid with header, toolbar (BACK/PREV/NEXT),
// and footer (NEW / SAVE <name> / SAVE AS).
//
// Usage:
//   1. Call open() once when entering the browser state.
//   2. Call setTitle(), setLoadedName(), setHasPrev/Next(), then
//      clearItems() + addItem() to populate the current page.
//      For async loading, call setStatusText() and clearItems() to show a
//      placeholder message until items are ready.
//   3. Call draw() to render.
//   4. Call poll() every frame; handle the returned event.
//   5. Tap the header bar to toggle delete mode (onBootPress() also available).

class FileBrowser {
public:
    FileBrowser(EPD_PainterAdafruit& d, GT911_Lite& touch);

    // Reset state — call when first entering the browser
    void open();

    // ── Configure before draw ────────────────────────────────────────────────
    void setTitle     (const char* title);   // "SD CARD" / "SERVER" (delete mode overrides)
    void setLoadedName(const char* name);    // display name (no extension) for SAVE label + bold
    void setHasPrev   (bool v);
    void setHasNext   (bool v);
    void setStatusText(const char* text);    // shown centred in grid when no items

    // ── Item list for current page ───────────────────────────────────────────
    void clearItems();
    void addItem(const char* name);          // display name (no extension), up to FB_PER_PAGE
    int  itemCount() const { return _itemCount; }

    // ── Header tap (or legacy boot button call) toggles delete mode ──────────
    void onBootPress();
    bool altMode() const { return _altMode; }

    void             draw();
    FileBrowserResult poll();

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;

    char  _title[24];
    char  _loadedName[STORAGE_FILENAME_MAX];
    char  _statusText[32];
    bool  _hasPrev;
    bool  _hasNext;
    bool  _altMode;
    bool  _wasDown;

    char  _items[FB_PER_PAGE][STORAGE_FILENAME_MAX];
    int   _itemCount;

    void drawHeader ();
    void drawToolbar();
    void drawGrid   ();
    void drawItem   (int col, int rowY, const char* name, bool isLoaded);
    void drawFooter ();

    bool hitHome  (int sx, int sy) const;
    bool hitDel   (int sx, int sy) const;   // left-side DELETE-mode toggle
    bool hitPrev  (int sx, int sy) const;
    bool hitNext  (int sx, int sy) const;
    int  hitItem  (int sx, int sy) const;   // page-relative index or -1
    bool hitNew   (int sx, int sy) const;
    bool hitSave  (int sx, int sy) const;
    bool hitSaveAs(int sx, int sy) const;

    static void rawToScreen(int rx, int ry, int& sx, int& sy);
};
