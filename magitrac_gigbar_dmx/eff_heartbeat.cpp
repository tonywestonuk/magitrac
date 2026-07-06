// eff_heartbeat.cpp — Heartbeat.
// Self-animating lub-dub, tick-driven (the posts' Heartbeat needs no mic, so no
// beat packets arrive — this must run off the clock).  Two thumps per ~1 s, but
// split across the bar: the FIRST beat (lub) fires the DERBIES, the SECOND beat
// (dub) pulses the PARS red.  Derbies have no dimmer so the lub is a hard on/off
// blink; the dub fades the pars with the envelope.
#include "gig_effects.h"

GigFrame effHeartbeat(const EffectCtx &c) {
  GigFrame o;
  uint32_t ph = c.tick % 50;             // 1 s cycle at 50 Hz

  bool    lub    = (ph < 6);             // first beat  -> derbies
  uint8_t dubEnv = 0;                    // second beat -> pars
  if (ph >= 10 && ph < 16) dubEnv = 255 - (uint8_t)((ph - 10) * 40);

  o.derbyColor = lub ? 37 : 0;           // red derby blink on the first beat
  hsv2rgb(0, 255, dubEnv, o.r, o.g, o.b);// red par pulse on the second beat
  return o;
}
