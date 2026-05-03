#include "KeyboardPopup.h"
#include <string.h>

// ── Constants ─────────────────────────────────────────────────────────────────

// Width of the layer-toggle button in the text field
static const int KBD_SYM_BTN_W = 108;

// Alpha layer
static const char* KBD_ROW1 = "QWERTYUIOP";   // 10 chars
static const char* KBD_ROW2 = "ASDFGHJKL";    //  9 chars  (starts at x=48)
static const char* KBD_ROW3 = "ZXCVBNM";      //  7 chars  (starts at x=96)

// Symbol / number layer (boot button toggles)
static const char* KBD_ROW1_SYM = "1234567890";  // 10
static const char* KBD_ROW2_SYM = "-_.:;!?@#";   //  9
static const char* KBD_ROW3_SYM = "()[]{}+";     //  7

// ── Constructor ───────────────────────────────────────────────────────────────

KeyboardPopup::KeyboardPopup(EPD_PainterAdafruit& display, GT911_Lite& touch)
    : _d(display)
    , _touch(touch)
    , _buf(nullptr)
    , _maxLen(0)
    , _open(false)
    , _done(false)
    , _wasDown(false)
    , _symLayer(false)
{}

// ── Public ────────────────────────────────────────────────────────────────────

void KeyboardPopup::open(char* buf, uint8_t maxLen) {
    _buf      = buf;
    _maxLen   = maxLen;
    _open     = true;
    _done     = false;
    _symLayer = false;
    _wasDown  = _touch.isTouched;  // swallow any lingering touch
}

void KeyboardPopup::draw() {
    _d.fillScreen(COL_WHITE);
    drawTextField();

    const char* r1 = _symLayer ? KBD_ROW1_SYM : KBD_ROW1;
    const char* r2 = _symLayer ? KBD_ROW2_SYM : KBD_ROW2;
    const char* r3 = _symLayer ? KBD_ROW3_SYM : KBD_ROW3;

    drawKeyRow(KBD_ROW1_Y, r1, 10, 0);
    drawKeyRow(KBD_ROW2_Y, r2, 9,  KBD_SLOT_W / 2);
    drawKeyRow(KBD_ROW3_Y, r3, 7,  KBD_SLOT_W);

    // BKSP — occupies 2 slots at the right of row 3
    {
        int x = KBD_SLOT_W + 7 * KBD_SLOT_W;   // = 8 * 96 = 768
        int w = KBD_W - x - 4;                  // reaches right edge
        drawKey(x, KBD_ROW3_Y, w, "BKSP");
    }

    // Row 4: CANCEL | SPACE | DONE  (each 320px wide)
    drawKey(4,   KBD_ROW4_Y, 308, "CANCEL");
    drawKey(324, KBD_ROW4_Y, 308, "SPACE");
    drawKey(648, KBD_ROW4_Y, 308, "DONE");
}

bool KeyboardPopup::poll() {
    if (!_open || !_touch.read()) return false;

    bool down = _touch.isTouched;
    int sx, sy;
    rawToScreen(_touch.x, _touch.y, sx, sy);

    // Fire on finger lift
    if (!down && _wasDown) {
        _wasDown = false;

        // Layer toggle button — top-left of text field
        if (sy < KBD_FIELD_H && sx >= 4 && sx < 4 + KBD_SYM_BTN_W) {
            toggleSymbolLayer();
            return false;
        }

        bool bksp = false, done = false, cancel = false;
        char ch = hitKey(sx, sy, bksp, done, cancel);

        if (cancel) {
            _open = false;
            _done = false;
            return true;
        }
        if (done) {
            _open = false;
            _done = true;
            return true;
        }
        if (bksp) {
            int len = strlen(_buf);
            if (len > 0) _buf[len - 1] = '\0';
            drawTextField();
            _d.paintLater();
        } else if (ch) {
            int len = strlen(_buf);
            if (len < _maxLen - 1) {
                _buf[len]     = ch;
                _buf[len + 1] = '\0';
                drawTextField();
                _d.paintLater();
            }
        }
        return false;
    }

    if (down && !_wasDown) _wasDown = true;
    return false;
}

// ── Private ───────────────────────────────────────────────────────────────────

void KeyboardPopup::drawTextField() {
    _d.fillRect(0, 0, KBD_W, KBD_FIELD_H, COL_BLACK);

    // Layer toggle button — top left, inverted colours so it stands out
    const char* btnLabel = _symLayer ? "ABC" : "123";
    _d.fillRect(4, 4, KBD_SYM_BTN_W, KBD_FIELD_H - 8, COL_WHITE);
    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    int bw = (int)strlen(btnLabel) * 18;
    _d.setCursor(4 + (KBD_SYM_BTN_W - bw) / 2, (KBD_FIELD_H - 24) / 2);
    _d.print(btnLabel);

    // Text content (offset right to clear the button)
    _d.setTextColor(COL_WHITE);
    _d.setCursor(4 + KBD_SYM_BTN_W + 8, (KBD_FIELD_H - 24) / 2);
    _d.print(_buf ? _buf : "");
    _d.print("_");
}

void KeyboardPopup::drawKeyRow(int y, const char* keys, int count, int xOffset) {
    char label[2] = { 0, 0 };
    for (int i = 0; i < count; i++) {
        label[0] = keys[i];
        drawKey(xOffset + i * KBD_SLOT_W, y, KBD_KEY_W, label);
    }
}

void KeyboardPopup::drawKey(int x, int y, int w, const char* label) {
    int h = KBD_KEY_H;
    _d.fillRect(x, y, w, h, COL_WHITE);
    _d.drawRect(x, y, w, h, COL_BLACK);

    _d.setTextSize(3);
    _d.setTextColor(COL_BLACK);
    int charW  = 18;
    int charH  = 24;
    int labelW = strlen(label) * charW;
    _d.setCursor(x + (w - labelW) / 2, y + (h - charH) / 2);
    _d.print(label);
}

char KeyboardPopup::hitKey(int sx, int sy,
                            bool& bksp, bool& done, bool& cancel) const {
    bksp = done = cancel = false;

    const char* r1 = _symLayer ? KBD_ROW1_SYM : KBD_ROW1;
    const char* r2 = _symLayer ? KBD_ROW2_SYM : KBD_ROW2;
    const char* r3 = _symLayer ? KBD_ROW3_SYM : KBD_ROW3;

    if (sy < KBD_ROW1_Y) return 0;

    if (sy < KBD_ROW2_Y) {
        int slot = sx / KBD_SLOT_W;
        if (slot >= 0 && slot < 10) return r1[slot];

    } else if (sy < KBD_ROW3_Y) {
        int slot = (sx - KBD_SLOT_W / 2) / KBD_SLOT_W;
        if (slot >= 0 && slot < 9) return r2[slot];

    } else if (sy < KBD_ROW4_Y) {
        if (sx >= KBD_SLOT_W * 8) { bksp = true; return 0; }
        int slot = (sx - KBD_SLOT_W) / KBD_SLOT_W;
        if (slot >= 0 && slot < 7) return r3[slot];

    } else if (sy < KBD_ROW4_Y + KBD_KEY_H) {
        if (sx < 320)  { cancel = true; return 0; }
        if (sx < 640)  return ' ';
        done = true; return 0;
    }

    return 0;
}

void KeyboardPopup::rawToScreen(int rx, int ry, int& sx, int& sy) const {
    sx = ry;
    sy = KBD_H - rx;
}

void KeyboardPopup::toggleSymbolLayer() {
    _symLayer = !_symLayer;
    draw();
    _d.paintLater();
}
