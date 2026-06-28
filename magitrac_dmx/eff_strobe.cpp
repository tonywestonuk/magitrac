// eff_strobe.cpp — Strobe / Lightning.
// Momentary, performer-driven — like the posts' LayerStrobe.  Fires WHILE the
// touchpad is held; white, every other 20 ms tick.  Released pad = dark.
#include "dmx_effects.h"

WashColour effStrobe(const EffectCtx &c) {
  WashColour o = {0, 0, 0, 0};
  if (c.pressed && (c.tick & 1)) { o.r = o.g = o.b = o.w = 255; }
  return o;
}
