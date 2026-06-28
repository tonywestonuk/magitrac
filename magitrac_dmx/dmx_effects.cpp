// dmx_effects.cpp — shared helpers + the effectId -> renderer dispatch.
// Individual looks live in eff_*.cpp.

#include "dmx_effects.h"

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

// A single RGBW wash can't render strip effects literally, so the show's
// effectId maps onto a handful of wash behaviours.  This is a starting
// taste-map — retune freely; effect names come from pixelpost_proto.h.
WashColour washRenderEffect(const EffectCtx &c) {
  switch (c.effectId) {
    case 8:  case 19:           return effStrobe(c);     // Strobe, Lightning
    case 13:                    return effSpectrum(c);   // Sound Spectrum (mic FFT)
    case 5:  case 11:           return effColorSel(c);   // Color Wheel, Circles 1
    case 4:                     return effTap(c);        // POW!
    case 3:  case 14:           return effFire(c);       // FIRE, Blood
    case 17:                    return effHeartbeat(c);  // Heartbeat (self-animating)
    case 20: case 23: case 25:  return effBeat(c);       // Beat Test/Relay/Solid Beat
    case 0:  case 2:            return effSolid(c);      // BLOCK-1/2
    default:                    return effRainbow(c);    // rainbow et al.
  }
}
