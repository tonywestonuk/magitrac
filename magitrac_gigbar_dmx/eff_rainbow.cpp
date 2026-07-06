// eff_rainbow.cpp — default fallback look.
// Smooth hue sweep on the pars (full wheel in ~5 s, 1 hue step per 20 ms tick),
// with the derbies on auto two-colour, slowly counter-spinning underneath.
#include "gig_effects.h"

GigFrame effRainbow(const EffectCtx &c) {
  GigFrame o;
  hsv2rgb((uint8_t)(c.tick & 0xFF), 255, 255, o.r, o.g, o.b);
  o.derbyColor  = 230;   // auto, two colours at a time
  o.derbyRotate = 30;    // slow clockwise (derby 2 mirrors to counter-spin)
  return o;
}
