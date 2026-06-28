// eff_tap.cpp — POW!
// Dark until tapped, then a white pop that decays away (tapEnv, derived in main).
// The pop hits the pars, the dedicated strobe panel's white AND the derbies for
// extra punch.  Derbies have no dimmer, so they snap full-RGB on while the pop's
// envelope is up and cut out as it decays — a hard on/off blink with the pop.
#include "gig_effects.h"

GigFrame effTap(const EffectCtx &c) {
  GigFrame o;
  o.r = o.g = o.b = o.w = c.tapEnv;
  o.strobeWhite   = c.tapEnv;     // dedicated strobe LEDs flash with the pop
  o.strobePattern = 100;          // white manual strobe band (lets the dimmer through)
  o.strobeSpeed   = 0;            // single pop, no auto-repeat
  o.derbyColor    = (c.tapEnv > 40) ? 187 : 0;   // RGB derby blink while the pop is bright
  return o;
}
