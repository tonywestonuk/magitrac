#pragma once
#include <Arduino.h>

// ── Debug logging ─────────────────────────────────────────────────────────────
// Set to 1 to enable verbose Serial output (touch events, timing, etc.)
#define MAGITRAC_DEBUG 0

#if MAGITRAC_DEBUG
  #define DBG(...)  Serial.printf(__VA_ARGS__)
  #define DBGLN(s)  Serial.println(s)
#else
  #define DBG(...)
  #define DBGLN(s)
#endif

// ── EPD 4-shade palette ───────────────────────────────────────────────────────
// The only four shades the e-paper display can render.
// Rule: never COL_DKGREY text on COL_LTGREY background.
// Disabled state: COL_WHITE background + COL_DKGREY text.
static const uint8_t COL_WHITE  = 0;
static const uint8_t COL_LTGREY = 1;
static const uint8_t COL_DKGREY = 2;
static const uint8_t COL_BLACK  = 3;

// ── Inertia tuning ────────────────────────────────────────────────────────────
// Controls the feel of momentum scrolling on the note grid.
static const float INERTIA_DECAY    = 2.0f;  // velocity decay rate per second (exponential)
static const float INERTIA_MAX_VEL  = 40.0f; // rows/second cap on launch velocity
static const float INERTIA_MIN_VEL  = 2.0f;  // below this, no inertia is started
static const float INERTIA_STOP_VEL = 0.4f;  // rows/second threshold to stop coasting

// ── ScrollViewport tuning (pixel domain) ──────────────────────────────────────
// Used by the generic ScrollViewport component, whose scroll offset is in PIXELS
// (not rows).  Tuned to match the row-based feel above at a ~60 px row height
// (e.g. SCROLL_MAX_VEL ≈ INERTIA_MAX_VEL × 60).  All ScrollViewport surfaces
// share these so they feel identical.
static const float SCROLL_DECAY      = 2.0f;    // 1/s, exponential velocity decay
static const float SCROLL_MAX_VEL    = 2400.0f; // px/s cap on launch velocity
static const float SCROLL_MIN_VEL    = 120.0f;  // px/s; below this no fling starts
static const float SCROLL_STOP_VEL   = 24.0f;   // px/s; coasting stops below this
static const int   SCROLL_DRAG_THRESH_PX = 12;  // movement past this = pan, not tap
