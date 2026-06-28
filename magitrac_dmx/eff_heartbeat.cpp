// eff_heartbeat.cpp — Heartbeat.
// Self-animating lub-dub, tick-driven (the posts' Heartbeat needs no mic, so no
// beat packets arrive — this must run off the clock).  Two red thumps per ~1 s.
#include "dmx_effects.h"

WashColour effHeartbeat(const EffectCtx &c) {
  WashColour o = {0, 0, 0, 0};
  uint32_t ph  = c.tick % 50;            // 1 s cycle at 50 Hz
  uint8_t  env = 0;
  if      (ph < 6)              env = 255 - (uint8_t)(ph * 40);          // lub
  else if (ph >= 10 && ph < 16) env = 255 - (uint8_t)((ph - 10) * 40);  // dub
  hsv2rgb(0, 255, env, o.r, o.g, o.b);   // red
  return o;
}
