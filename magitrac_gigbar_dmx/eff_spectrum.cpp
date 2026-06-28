// eff_spectrum.cpp — Sound Spectrum (effect 13).
// On the bar this is deliberately simple: solid green on both Pars and both
// Derbies (the FFT bands are ignored here — the user wants a plain green wash for
// this mode rather than a band-mapped colour scramble).
#include "gig_effects.h"

GigFrame effSpectrum(const EffectCtx &c) {
  GigFrame o;
  o.g          = 255;   // pars green
  o.derbyColor = 62;    // derby colour code = green (50-74 band)
  return o;
}
