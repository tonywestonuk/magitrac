// eff_fire.cpp — FIRE.
// The noisy per-pixel flicker doesn't read on a wash, so on the bar FIRE is a
// slow warm pulse: pars crossfade between deep red and orange, and the derbies
// track it (red at the red end, amber at the orange end).  Slow and gentle — a
// glow, not a strobe.
#include "gig_effects.h"

GigFrame effFire(const EffectCtx &c) {
  GigFrame o;

  // Triangle 0..255..0 over a ~2.4 s cycle (120 ticks at 50 Hz).
  const uint32_t period = 120;
  uint32_t ph  = c.tick % period;
  uint8_t  tri = (ph < period / 2)
                   ? (uint8_t)(ph * 2 * 255 / period)
                   : (uint8_t)(255 - (ph - period / 2) * 2 * 255 / period);

  // Pars: red (tri=0) -> orange (tri=255).  R pinned full, green rises to ~80.
  o.r = 255;
  o.g = (uint8_t)(tri * 80 / 255);
  o.b = 0;

  // Derbies: red at the red end, amber (R+G) at the orange end.
  o.derbyColor = (tri < 128) ? 37 : 112;   // 37 = red band, 112 = red+green (amber)

  return o;
}
