// eff_lightning.cpp — Lightning.
// Self-firing: most ticks are dark, then a random strike blasts the WHOLE bar
// white for a moment — pars, derbies and the strobe panel together — followed by
// a short fading tail on the pars.  Strike rarity tracks the slider (same mapping
// as the post: higher slider = more frequent), and a tap forces a strike.
//
// Note: the slider doubles as master brightness in the main render, so a low
// slider makes strikes both rarer AND dimmer; keep the slider up for bright,
// frequent lightning.
#include "gig_effects.h"
#include <Arduino.h>

// Strike-frequency divisor — higher = lightning fires more often.  Strike chance
// per tick is 1/rarity where rarity = (320 - slider) / this.
static const int LIGHTNING_FREQ = 4;

static uint8_t  s_flash    = 0;            // current strike counter (8..0)
static uint32_t s_lastTick = 0xFFFFFFFFu;

GigFrame effLightning(const EffectCtx &c) {
  // Advance the strike state once per new tick (matches the post's per-tick model).
  if (c.tick != s_lastTick) {
    s_lastTick = c.tick;
    if (s_flash == 0) {
      int rarity = (320 - (int)c.slider) / LIGHTNING_FREQ;
      if (rarity < 4) rarity = 4;
      if ((esp_random() % rarity) == 0 || c.tapped) s_flash = 8;   // new strike
    } else {
      s_flash--;                                                   // count it down
    }
  }

  GigFrame o;
  if (s_flash > 6) {
    // The bright blast — everything white.
    o.r = o.g = o.b = 255;
    o.w = 255;
    o.derbyColor   = 187;     // RGB (white-ish — the derby's brightest "all on")
    o.strobePattern = 100;    // white-manual band so the strobe LEDs light
    o.strobeWhite   = 255;
    o.strobeSpeed   = 0;
  } else if (s_flash > 0) {
    // Short fading afterglow on the pars only.
    uint8_t v = (uint8_t)((s_flash * 255) / 6);
    o.r = o.g = o.b = v;
    o.w = v;
  }
  return o;
}
