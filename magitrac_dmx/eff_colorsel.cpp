// eff_colorsel.cpp — Color Wheel / Circles 1.
// Live XY colour picker — the posts' LayerColorSel mapping: x -> hue,
// y -> saturation (top half) / value (bottom half).
#include "dmx_effects.h"

WashColour effColorSel(const EffectCtx &c) {
  WashColour o = {0, 0, 0, 0};
  uint8_t sat, val;
  if (c.y < 128) { sat = c.y * 2; val = 255; }
  else           { sat = 255;     val = 255 - (uint8_t)((c.y - 128) * 2); }
  hsv2rgb(c.x, sat, val, o.r, o.g, o.b);
  return o;
}
