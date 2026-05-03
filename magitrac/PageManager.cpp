#include "PageManager.h"
#include <string.h>

// Home button at bottom of stub pages
static const int  HOME_BTN_H      = 80;
static const int  HOME_BTN_Y      = 540 - HOME_BTN_H;  // = 460
static const int  HOME_BTN_X      = 0;
static const int  HOME_BTN_W      = 960;


// ── Constructor ───────────────────────────────────────────────────────────────

PageManager::PageManager(EPD_PainterAdafruit& display, GT911_Lite& touch, Song& song)
    : _d(display)
    , _touch(touch)
    , _song(song)
    , _page(Page::TRACKER)
    , _trackerRedrawNeeded(false)
    , _touchWasDown(false)
{}

// ── Public accessors (consume one-shot flags) ─────────────────────────────────

bool PageManager::trackerRedrawNeeded() {
    bool v = _trackerRedrawNeeded;
    _trackerRedrawNeeded = false;
    return v;
}

// ── poll() ────────────────────────────────────────────────────────────────────

void PageManager::poll() {
    // ── Touch — only consume events on simple stub pages ─────────────────────
    // Complex pages (BLOCKS etc.) handle their own touch including HOME.
    if (_page == Page::TRACKER || _page == Page::BLOCKS) return;

    if (!_touch.read()) return;

    bool down = _touch.isTouched;
    int tx = _touch.y;
    int ty = 540 - _touch.x;

    bool rising  = (down  && !_touchWasDown);
    bool falling = (!down && _touchWasDown);
    _touchWasDown = down;

    if (!rising && !falling) return;

    // ── Stub page [HOME] button ───────────────────────────────────────────────
    if (falling && hitHomeButton(tx, ty)) {
        Serial.println("[PAGE] HOME tapped");
        _page = Page::TRACKER;
        _d.clear();
        _trackerRedrawNeeded = true;
    }
}

// ── goHome() ──────────────────────────────────────────────────────────────────

void PageManager::goHome() {
    _page = Page::TRACKER;
    _trackerRedrawNeeded = true;
    Serial.println("[PAGE] goHome");
}

void PageManager::goBlocks() {
    _page = Page::BLOCKS;
    Serial.println("[PAGE] goBlocks");
}

void PageManager::goPage(Page p) {
    _page = p;
    Serial.printf("[PAGE] goPage %d\n", (int)p);
    _d.clear();
    drawPage();
    _d.paint();
}

// ── drawPage() ────────────────────────────────────────────────────────────────

void PageManager::drawPage() {
    switch (_page) {
        case Page::INSTRUMENTS:
            drawStubPage("INSTRUMENTS",
                "Define up to 16 instruments.\nEach instrument maps to a\nMIDI channel and patch number.");
            break;
        case Page::SETTINGS:
            drawStubPage("SETTINGS",
                "Global settings — tempo,\nMIDI routing, display\nbrightness and calibration.");
            break;
        case Page::WIFI:
            drawStubPage("WIFI",
                "Configure WiFi SSID and\npassword. MIDI packets are\nsent via UDP on port 5004.");
            break;
        case Page::SONG:
            drawStubPage("SONG",
                "Arrange patterns into a song\nsequence. Drag blocks to\nreorder the play order.");
            break;
        default:
            break;
    }
}

// ── drawStubPage() ────────────────────────────────────────────────────────────

void PageManager::drawStubPage(const char* title, const char* body) {
    _d.fillScreen(0);  // white

    // Title bar
    _d.fillRect(0, 0, 960, 60, 3);  // black
    _d.setTextSize(3);
    _d.setTextColor(0);  // white
    int tw = strlen(title) * 18;
    _d.setCursor((960 - tw) / 2, (60 - 24) / 2);
    _d.print(title);

    // Body text
    _d.setTextSize(2);
    _d.setTextColor(3);  // black
    int y = 100;
    // Print body line by line (split on \n)
    char buf[128];
    strncpy(buf, body, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char* line = strtok(buf, "\n");
    while (line) {
        _d.setCursor(40, y);
        _d.print(line);
        y += 28;
        line = strtok(nullptr, "\n");
    }

    // [HOME] button
    _d.fillRect(HOME_BTN_X, HOME_BTN_Y, HOME_BTN_W, HOME_BTN_H, 3);  // black
    _d.setTextSize(3);
    _d.setTextColor(0);  // white
    int lw = 4 * 18;  // "HOME"
    _d.setCursor((960 - lw) / 2, HOME_BTN_Y + (HOME_BTN_H - 24) / 2);
    _d.print("HOME");
}


// ── hitHomeButton() ───────────────────────────────────────────────────────────

bool PageManager::hitHomeButton(int tx, int ty) const {
    return tx >= HOME_BTN_X && tx < HOME_BTN_X + HOME_BTN_W
        && ty >= HOME_BTN_Y && ty < HOME_BTN_Y + HOME_BTN_H;
}
