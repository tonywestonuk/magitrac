// eff_tap.cpp — POW!
// Dark until tapped, then a white pop that decays away (tapEnv, derived in main).
#include "dmx_effects.h"

WashColour effTap(const EffectCtx &c) {
  WashColour o;
  o.r = o.g = o.b = o.w = c.tapEnv;
  return o;
}
