// eff_sparkle.cpp — Sparkle (effect 7).
// The post effect scatters brief white dots over a black strip, the density set
// by touch-Y.  The bar has no addressable pixels, so the sparkle lands on the
// dedicated strobe panel instead: random single-frame (20 ms) white pops, so the
// strobe LEDs twinkle.  Pars stay dark — the twinkle reads against black.
// Density matches the post's mapping: low Y = busy, high Y = sparse.
#include "gig_effects.h"
#include <Arduino.h>

GigFrame effSparkle(const EffectCtx &c) {
  GigFrame o;
  // Same gate the post uses to decide "sparkle this frame?": with low Y the
  // window (455-Y) is wide so the random value clears 200 often; with high Y it
  // rarely does.  (Y is 0..255, so 455-Y is 200..455 — never zero.)
  if ((int)(esp_random() % (455 - c.y)) >= 200) {
    o.strobePattern = 100;   // white-manual band so ch21 (white dimmer) drives the LEDs
    o.strobeWhite   = 255;
    o.strobeSpeed   = 0;     // single pop per frame — no auto-strobe
  }
  // POW on touch: the bar flashes full white and fades.  tapEnv snaps to 255 on
  // the touch rising-edge and decays to 0 ~200 ms later (derived in main, same
  // envelope effTap uses).  Pars carry the white wash; the strobe panel joins in
  // (max with any twinkle pop this frame) so the whole bar reads white.
  if (c.tapEnv > 0) {
    o.r = o.g = o.b = o.w = c.tapEnv;
    o.strobePattern = 100;                       // keep the white dimmer live
    if (c.tapEnv > o.strobeWhite) o.strobeWhite = c.tapEnv;
    o.strobeSpeed = 0;
  }
  return o;
}
