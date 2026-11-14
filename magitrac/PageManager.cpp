#include "PageManager.h"

// ── Constructor ───────────────────────────────────────────────────────────────

PageManager::PageManager()
    : _page(Page::TRACKER)
    , _trackerRedrawNeeded(false)
{}

// ── Public accessors (consume one-shot flags) ─────────────────────────────────

bool PageManager::trackerRedrawNeeded() {
    bool v = _trackerRedrawNeeded;
    _trackerRedrawNeeded = false;
    return v;
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
