// eff_strobe.cpp — Strobe / Lightning.
// Momentary, performer-driven — like the posts' LayerStrobe.  Fires WHILE the
// touchpad is held.  Two layers: the pars flash white on alternate ticks (a
// rock-solid, tick-locked strobe we control directly), and the dedicated strobe
// panel runs its own white-manual-strobe program for the hard xenon-style snap.
// Released pad = everything dark.
#include "gig_effects.h"

GigFrame effStrobe(const EffectCtx &c) {
  GigFrame o;
  if (c.pressed) {
    if (c.tick & 1) { o.r = o.g = o.b = o.w = 255; }   // par flash, alternate ticks
    o.strobePattern = 105;   // white manual strobe, mid speed
    o.strobeWhite   = 255;
    o.strobeSpeed   = 220;   // fast
  }
  return o;
}
