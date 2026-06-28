// eff_blood.cpp — Blood.
// A slow crossfade between green and red, with the derbies showing the OPPOSITE
// colour to the pars: when the pars are green the derbies are red, and as the
// pars cross to red the derbies flip to green.  The par fade is a smooth R/G
// blend (a triangle wave); the derbies can't fade colour, so they snap at the
// half-way point.
#include "gig_effects.h"

GigFrame effBlood(const EffectCtx &c) {
  GigFrame o;

  // Triangle 0..255..0 over a ~2 s cycle (100 ticks at 50 Hz): 0 = green, 255 = red.
  const uint32_t period = 100;
  uint32_t ph  = c.tick % period;
  uint8_t  tri = (ph < period / 2)
                   ? (uint8_t)(ph * 2 * 255 / period)
                   : (uint8_t)(255 - (ph - period / 2) * 2 * 255 / period);

  // Pars: blend red up as green falls.
  o.r = tri;
  o.g = (uint8_t)(255 - tri);
  o.b = 0;

  // Derbies: the complement — red while the pars are mostly green, green once the
  // pars have crossed to red.
  o.derbyColor = (tri < 128) ? 37 : 62;   // 37 = red band, 62 = green band

  return o;
}
