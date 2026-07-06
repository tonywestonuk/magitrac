// gig_effects.cpp — shared helpers + the effectId -> renderer dispatch.
// Individual looks live in eff_*.cpp.

#include "gig_effects.h"

// HSV -> RGB (h,s,v all 0..255).  Standard 6-sector conversion.
void hsv2rgb(uint8_t h, uint8_t s, uint8_t v,
             uint8_t &r, uint8_t &g, uint8_t &b) {
  uint8_t region = h / 43;                  // 0..5
  uint8_t rem    = (h - region * 43) * 6;   // 0..255 within sector
  uint8_t p = (uint16_t)v * (255 - s) / 255;
  uint8_t q = (uint16_t)v * (255 - ((uint16_t)s * rem / 255)) / 255;
  uint8_t t = (uint16_t)v * (255 - ((uint16_t)s * (255 - rem) / 255)) / 255;
  switch (region) {
    case 0:  r = v; g = t; b = p; break;
    case 1:  r = q; g = v; b = p; break;
    case 2:  r = p; g = v; b = t; break;
    case 3:  r = p; g = q; b = v; break;
    case 4:  r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
}

uint8_t dmxHash8(uint32_t x) {
  x ^= x >> 13; x *= 0x9E3779B1u; x ^= x >> 15;
  return (uint8_t)x;
}

// Map a 0..255 hue onto one of the derby's coarse colour codes so the derby
// beams roughly track the wash colour.  Centres are chosen inside each band of
// the fixture's Derby-control chart (R 25-49, G 50-74, B 75-99, combos 100-199).
uint8_t hueToDerbyColor(uint8_t hue) {
  if (hue < 21 || hue >= 234) return 37;    // red
  if (hue < 64)               return 112;   // red + green (orange/yellow)
  if (hue < 106)              return 62;    // green
  if (hue < 150)              return 162;   // green + blue (cyan)
  if (hue < 192)              return 87;    // blue
  return 137;                               // red + blue (magenta)
}

// The PixelPost effectId maps onto a handful of bar behaviours.  This is a
// starting taste-map — retune freely; effect names come from pixelpost_proto.h.
// The pars are the backbone (every look drives them); derby/laser/strobe ride on
// top for the more energetic looks.
GigFrame gigRenderEffect(const EffectCtx &c) {
  switch (c.effectId) {
    case 8:                     return effStrobe(c);     // Strobe (momentary, touch-held)
    case 19:                    return effLightning(c);  // Lightning (random white strikes)
    case 13:                    return effSpectrum(c);   // Sound Spectrum (simple green)
    case 26:                    return effUv(c);         // UV (UV channels full on)
    case 27:                    return effWhite(c);      // WHITE (press-hold white swell)
    case 5:                     return effColorSel(c);   // Color Wheel (uniform colour)
    case 11:                    return effCircles(c);    // Circles 1 (2D circle, per-slice)
    case 7:                     return effSparkle(c);    // Sparkle (strobe-panel twinkle)
    case 4:                     return effTap(c);        // POW!
    case 3:                     return effFire(c);       // FIRE
    case 14:                    return effBlood(c);      // Blood (green<->red crossfade)
    case 17:                    return effHeartbeat(c);  // Heartbeat (self-animating)
    case 20: case 23: case 25:  return effBeat(c);       // Beat Test/Relay/Solid Beat
    case 0:  case 2:            return effSolid(c);      // BLOCK-1/2
    default:                    return effRainbow(c);    // rainbow et al.
  }
}
