// eff_uv.cpp — UV (effect 26).
// The posts go solid deep blue; the bar just turns its UV channels full on — both
// Par UV emitters and the strobe panel's UV bank.  No RGB, no derbies (the
// derbies have no UV element).  Slider still scales it via master in gigWrite.
#include "gig_effects.h"

GigFrame effUv(const EffectCtx &c) {
  GigFrame o;
  o.uv            = 255;   // both Pars' UV (ch4 / ch9)
  o.strobeUv      = 255;   // strobe panel UV bank (ch22)
  o.strobePattern = 0;     // static — no auto-strobe
  return o;
}
