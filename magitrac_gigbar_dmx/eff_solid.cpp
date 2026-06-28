// eff_solid.cpp — BLOCK-1 / BLOCK-2.
// Fixed colour; hue derived from the effectId so the two blocks differ.  The
// derbies hold the same colour family and creep slowly so the scene has life.
#include "gig_effects.h"

GigFrame effSolid(const EffectCtx &c) {
  GigFrame o;
  uint8_t hue = (uint8_t)(c.effectId * 37);
  hsv2rgb(hue, 255, 255, o.r, o.g, o.b);
  o.derbyColor  = hueToDerbyColor(hue);
  o.derbyRotate = 20;    // very slow drift
  return o;
}
