#pragma once
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "SongStorage.h"
#include "MagiMsg.h"      // SRV_MAX_SONGS
#include "UIHelpers.h"
#include "Constants.h"

// ── Layout constants ──────────────────────────────────────────────────────────

static const int FB_HDR_H    = 50;
static const int FB_TOOL_H   = 55;

// Title-bar button, right side — HOME (exit page).  Deleting is a per-row
// swipe, so there is no header DEL button.
static const int FB_HOME_X   = 830;
static const int FB_HOME_W   = 130;

static const int FB_ROWS     = 7;
static const int FB_LIST_Y   = FB_HDR_H + FB_TOOL_H;    // = 105
static const int FB_ROW_H    = 54;

static const int FB_FOOT_Y   = FB_LIST_Y + FB_ROWS * FB_ROW_H;  // = 483
static const int FB_FBTN_Y   = FB_FOOT_Y + 4;
static const int FB_FBTN_H   = 49;
static const int FB_NEW_X    = 0;
static const int FB_NEW_W    = 316;
static const int FB_SAVE_X   = 320;
static const int FB_SAVE_W   = 316;
static const int FB_SAVEAS_X = 640;
static const int FB_SAVEAS_W = 320;

// Max items the browser can hold at once.  The list accumulates the whole
// server song list, so this caps it — sized to what the server can list, so no
// song is ever left off.  The storage is allocated from PSRAM in open() (3 KB
// is too much to sit in internal RAM, which the radios need).
static const int FB_MAX_ITEMS = SRV_MAX_SONGS;

// Drag-scroll list: single full-width rows between the toolbar and footer.
static const int FB_LROW_H           = FB_ROW_H;                        // 54
static const int FB_LIST_ROWS        = (FB_FOOT_Y - FB_LIST_Y) / FB_LROW_H; // 7
static const int FB_LIST_DRAG_THRESH = 12;
// Swipe-left on a list row reveals a per-row DEL button (iOS-style).
static const int FB_SWIPE_THRESH     = 55;    // px leftward to reveal DEL
static const int FB_ROWDEL_W         = 150;   // DEL button width on a revealed row

// ── Events ────────────────────────────────────────────────────────────────────

enum class FileBrowserEvent : uint8_t {
    NONE,
    HOME,
    NEW,
    SAVE,
    SAVE_AS,
    ITEM_TAP,     // result.index = index into the item list (itemName())
    ITEM_DELETE,  // result.index = same; the swiped row's DEL was tapped
};

struct FileBrowserResult {
    FileBrowserEvent event;
    int              index;   // valid for ITEM_TAP / ITEM_DELETE
};

// ── FileBrowser ───────────────────────────────────────────────────────────────
//
// Song browser: header (title + HOME), a toolbar strip, the item area and a
// footer (NEW / SAVE <name> / SAVE AS).  The item area is seven full-width
// rows over the WHOLE item list, drag-scroll with inertia; the toolbar shows
// "a-b of N".  Swipe a row left to reveal its DEL button.
//
// Usage:
//   1. Call open() once when entering the browser state (resets scroll).
//   2. Call setTitle(), setLoadedName(), then clearItems() + addItem() to
//      populate.
//      For async loading, call setStatusText() and clearItems() to show a
//      placeholder message until items are ready.
//   3. Call draw() to render.
//   4. Call poll() every frame; handle the returned event.

class FileBrowser {
public:
    FileBrowser(EPD_PainterAdafruit& d, GT911_Lite& touch);

    // Reset state — call when first entering the browser
    void open();

    // ── Configure before draw ────────────────────────────────────────────────
    void setTitle     (const char* title);   // header text, e.g. "SERVER"
    void setLoadedName(const char* name);    // display name (no extension) for SAVE label + bold
    void setStatusText(const char* text);    // shown in the item area while there are no items

    // ── Item list ────────────────────────────────────────────────────────────
    void clearItems();
    void addItem(const char* name);          // display name (no extension)
    int  itemCount() const { return _itemCount; }
    const char* itemName(int i) const {      // valid 0..itemCount()-1
        return (i >= 0 && i < _itemCount) ? _items[i] : "";
    }
    // Blank the item area (the status text shows instead) without losing the
    // list; restoreItems() brings it back as it was, scroll position included.
    // While hidden itemCount() is 0.  clearItems() / addItem() drop the hidden
    // list, after which restoreItems() does nothing.
    void hideItems();
    void restoreItems();

    void             draw();
    FileBrowserResult poll();

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;

    char  _title[24];
    char  _loadedName[STORAGE_FILENAME_MAX];
    char  _statusText[32];
    bool  _wasDown;

    char (*_items)[STORAGE_FILENAME_MAX] = nullptr;   // [FB_MAX_ITEMS], see open()
    int   _itemCount;
    int   _hiddenCount;       // items set aside by hideItems() (0 = none)
    int   _hiddenScroll;      // _scroll at hideItems()

    // Drag-scroll state.
    int   _scroll;            // index of the first visible row
    int   _dragStartY;
    int   _dragStartScroll;

    // Inertia (fling) — same model as the tracker grid: fractional-row position
    // + rows/sec velocity, exp-decayed (INERTIA_DECAY / INERTIA_MIN_VEL).
    float    _scrollF;        // fractional _scroll (coast truth; round → _scroll)
    float    _inertiaVel;     // rows/sec, signed like a _scroll delta
    bool     _inertiaActive;
    uint32_t _inertiaMs;      // last coast tick
    int      _velSampleY;     // drag velocity sampling: last sy
    uint32_t _velSampleMs;    //                         last time
    bool     _suppressTap;    // finger-down cancelled a coast → don't select
    void coastInertia();      // advance the fling one tick (finger-up frames)

    // Swipe-to-delete: a horizontal left-swipe on a row reveals its DEL button;
    // vertical scroll (or a tap elsewhere) hides it.
    int   _dragStartX;        // press x (horizontal-swipe detection)
    int   _gestureDir;        // 0 undecided, 1 vertical scroll, 2 horizontal
    int   _pressRow;          // abs item index under the press (-1 = none)
    int   _swipeItem;         // abs item index showing DEL, -1 = none
    bool  hitRowDel(int sx, int sy) const;   // tap on the revealed row's DEL

    void drawHeader ();
    void drawFooter ();
    void drawList     ();                 // item rows
    void drawListToolbar();               // "a-b of N" scroll hint
    int  hitListItem(int sx, int sy) const;  // absolute item index or -1
    int  maxScroll() const;

    bool hitHome  (int sx, int sy) const;
    bool hitNew   (int sx, int sy) const;
    bool hitSave  (int sx, int sy) const;
    bool hitSaveAs(int sx, int sy) const;

    static void rawToScreen(int rx, int ry, int& sx, int& sy);
};
