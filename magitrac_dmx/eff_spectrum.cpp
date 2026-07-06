// eff_spectrum.cpp — Sound Spectrum.
// Live mic FFT (PPOSB bands[5], server-AGC'd 0..255) folded onto the wash:
// bass -> red, mids -> green, highs -> blue.  Bands only arrive while the server
// runs a needsMic effect, which Sound Spectrum is.
#include "dmx_effects.h"

WashColour effSpectrum(const EffectCtx &c) {
  WashColour o;
  o.r = c.bands[0];                                            // 65-130 Hz  (bass)
  o.g = c.bands[1] > c.bands[2] ? c.bands[1] : c.bands[2];     // 130-520 Hz (mids)
  o.b = c.bands[3] > c.bands[4] ? c.bands[3] : c.bands[4];     // 0.5-4 kHz  (highs)
  o.w = 0;
  return o;
}
