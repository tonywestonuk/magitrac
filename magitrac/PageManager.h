#pragma once
#include <Arduino.h>

// ── Pages ─────────────────────────────────────────────────────────────────────
// Only the tracker grid and Block Settings are routed through PageManager;
// every other page is tracked by its own "open" flag in magitrac.ino.

enum class Page : uint8_t {
    TRACKER     = 0,
    BLOCKS      = 1,
};

// ── PageManager ───────────────────────────────────────────────────────────────

class PageManager {
public:
    PageManager();

    Page currentPage() const { return _page; }

    // True for one loop() after goHome() (caller: redraw the tracker)
    bool trackerRedrawNeeded();

    // Called when HOME is tapped on the Block Settings page
    void goHome();

    // Navigate to the BLOCKS page (called from the main tracker UI)
    void goBlocks();

private:
    Page _page;

    // One-shot flag consumed by the caller
    bool _trackerRedrawNeeded;
};
