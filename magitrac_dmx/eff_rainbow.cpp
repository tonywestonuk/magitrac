// eff_rainbow.cpp — default fallback look.
// Smooth hue sweep, full wheel in ~5 s (1 hue step per 20 ms tick).
#include "dmx_effects.h"

WashColour effRainbow(const EffectCtx &c) {
  WashColour o = {0, 0, 0, 0};
  hsv2rgb((uint8_t)(c.tick & 0xFF), 255, 255, o.r, o.g, o.b);
  return o;
}
