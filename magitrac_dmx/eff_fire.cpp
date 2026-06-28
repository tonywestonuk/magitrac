// eff_fire.cpp — FIRE / Blood.
// Warm flicker: orange base, value jittered by cheap noise, white channel adds heat.
#include "dmx_effects.h"

WashColour effFire(const EffectCtx &c) {
  WashColour o = {0, 0, 0, 0};
  uint8_t flick = 160 + (dmxHash8(c.tick) >> 2);            // 160..223
  hsv2rgb(12 + (dmxHash8(c.tick * 7) >> 5), 255, flick, o.r, o.g, o.b);
  o.w = flick >> 2;
  return o;
}
