#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"
#include "TrackerData.h"

// ── Pages ─────────────────────────────────────────────────────────────────────

enum class Page : uint8_t {
    TRACKER     = 0,
    BLOCKS      = 1,
    INSTRUMENTS = 2,
    SETTINGS    = 3,
    WIFI        = 4,
    SONG        = 5,
    PAGE_COUNT  = 6
};

// ── PageManager ───────────────────────────────────────────────────────────────

class PageManager {
public:
    PageManager(EPD_PainterAdafruit& display, GT911_Lite& touch, Song& song);

    // Call once per loop() — handles boot button and stub page touch
    void poll();

    Page currentPage() const { return _page; }

    // True for one loop() after page changes to TRACKER (caller: clear + redraw)
    bool trackerRedrawNeeded();

    // Draw the current stub page (non-TRACKER pages)
    void drawPage();

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;
    Song&                _song;

    Page _page;

    // One-shot flag consumed by the caller
    bool _trackerRedrawNeeded;

    // Touch state for stub page [HOME] button
    bool _touchWasDown;

    void drawStubPage(const char* title, const char* body);
    bool hitHomeButton(int tx, int ty) const;

public:
    // Called by complex pages (BlockSettingsPage etc.) when HOME is tapped
    void goHome();

    // Navigate directly to the BLOCKS page (called from the main tracker UI)
    void goBlocks();

    // Navigate to any stub page — draws and paints it immediately.
    void goPage(Page p);
};
