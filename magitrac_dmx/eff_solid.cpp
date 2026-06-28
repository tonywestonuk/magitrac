// eff_solid.cpp — BLOCK-1 / BLOCK-2.
// Fixed colour; hue derived from the effectId so the two blocks differ.
#include "dmx_effects.h"

WashColour effSolid(const EffectCtx &c) {
  WashColour o = {0, 0, 0, 0};
  hsv2rgb((uint8_t)(c.effectId * 37), 255, 255, o.r, o.g, o.b);
  return o;
}
