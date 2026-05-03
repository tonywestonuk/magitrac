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
static const float INERTIA_DECAY    = 1.0f;  // velocity decay rate per second (exponential)
static const float INERTIA_MAX_VEL  = 40.0f; // rows/second cap on launch velocity
static const float INERTIA_MIN_VEL  = 2.0f;  // below this, no inertia is started
static const float INERTIA_STOP_VEL = 0.4f;  // rows/second threshold to stop coasting
