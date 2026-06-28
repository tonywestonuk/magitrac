#pragma once
#include <Arduino.h>
#include "EPD_Painter_Adafruit.h"
#include "gt911_lite.h"

// ── PixelPostSettingsPage — photo-sensitivity + max-brightness ─────────────
//
// Owns the persisted gPixelPostFlashCtrl byte.  Single page, two value-bump
// controls:
//
//   Max Brightness   0..15  (upper nibble)   — 15 = full, 0 = off
//   Flash Smoothing  0..4   (lower nibble)   — 4 ≈ 1 s tau on the post side
//
// Byte 0x00 (both nibbles zero) means the post's LayerFlashSoftener is a
// passthrough — no smoothing, no brightness limit.  Any non-zero byte
// engages the layer.
//
// Persistence: NVS namespace "magitrac_pp", key "flashCtrl".  Sent over
// MagiLink via MSG_PIXELPOST_SET_FLASH_CTRL whenever it changes AND when
// the manual page opens (refreshes the server cache after server reboots).

extern uint8_t gPixelPostFlashCtrl;          // 0 = disabled, else MMMM SSSS

void loadPixelPostFlashCtrl();               // call once from setup()
void savePixelPostFlashCtrl();               // writes to NVS

class PixelPostSettingsPage {
public:
    PixelPostSettingsPage(EPD_PainterAdafruit& display, GT911_Lite& touch);

    void open(bool fingerDown = true);
    bool isOpen() const { return _open; }
    void draw();

    bool poll();   // returns true once when the user taps BACK

private:
    EPD_PainterAdafruit& _d;
    GT911_Lite&          _touch;

    bool _open;
    bool _wasDown;
    bool _swallowDown;

    void drawTitle();
    void drawRows();
    void drawBrightnessRow();
    void drawSmoothingRow();
    void redrawAndSync();   // repaint rows + push current byte to server

    void rawToScreen(int rx, int ry, int& sx, int& sy) const;

    bool hitBack(int sx, int sy) const;
    int  hitBrightness(int sx, int sy) const;  // -1, 0 = minus, 1 = plus
    int  hitSmoothing (int sx, int sy) const;  // -1, 0 = minus, 1 = plus
};
