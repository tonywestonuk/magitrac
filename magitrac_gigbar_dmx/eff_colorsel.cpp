// eff_colorsel.cpp — Color Wheel / Circles 1.
// Live XY colour picker — the posts' LayerColorSel mapping: x -> hue,
// y -> saturation (top half) / value (bottom half).  Pars take the picked colour;
// the derbies follow the chosen hue family with a slow spin.
#include "gig_effects.h"

GigFrame effColorSel(const EffectCtx &c) {
  GigFrame o;
  uint8_t sat, val;
  if (c.y < 128) { sat = c.y * 2; val = 255; }
  else           { sat = 255;     val = 255 - (uint8_t)((c.y - 128) * 2); }
  hsv2rgb(c.x, sat, val, o.r, o.g, o.b);
  o.derbyColor  = hueToDerbyColor(c.x);
  o.derbyRotate = 40;
  return o;
}
