// eff_white.cpp — WHITE (mirrors the posts' la_whitefill / WhiteFillLayer).
// Press-and-hold white swell: while the touchpad is held the par wash ramps up
// to full white over RAMP_MS; on release it ramps back to black over RAMP_MS,
// eased linearly from the *current* level so a quick press-release is a soft
// pulse, not a snap.  Same RAMP_MS=250 / TICK_MS=20 as the posts, and the bar
// ticks at the same 20 ms, so the bar swells in lock-step with the LED posts.
//
// Pars only (clean RGB white via gigWrite's white fold); derbies/laser/strobe
// stay dark for a pure white wash.  Overall brightness is the slider (master),
// applied downstream in gigWrite — same as every other effect.
#include "gig_effects.h"

GigFrame effWhite(const EffectCtx &c) {
  static const float RAMP_MS = 250.0f;
  static const float TICK_MS = 20.0f;
  static float    level    = 0.0f;
  static uint32_t lastTick = 0;
  static bool     haveTick = false;

  // Ticks since the previous frame; clamp so a tick-clock re-lock (jump when the
  // sender changes) can't snap the ramp to an end — same guard as the post.
  uint32_t dt = 0;
  if (haveTick && c.tick >= lastTick) dt = c.tick - lastTick;
  if (dt > 5) dt = 5;                 // 5 ticks = 100 ms cap
  lastTick = c.tick;
  haveTick = true;

  float step = 255.0f * (float)dt * TICK_MS / RAMP_MS;
  if (c.pressed) { level += step; if (level > 255.0f) level = 255.0f; }
  else           { level -= step; if (level < 0.0f)   level = 0.0f;   }

  uint8_t v = (uint8_t)(level + 0.5f);
  GigFrame o;
  o.r = o.g = o.b = v;
  return o;
}
