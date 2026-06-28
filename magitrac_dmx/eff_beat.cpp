// eff_beat.cpp — Beat Test / Relay / Solid Beat.
// Brightness pulses on the mic-detected beat (beatEnv); hue steps once per beat.
// beat_seq only advances while the server runs a needsMic effect, so this
// genuinely follows the music for these three.
#include "dmx_effects.h"

WashColour effBeat(const EffectCtx &c) {
  WashColour o = {0, 0, 0, 0};
  hsv2rgb((uint8_t)(c.beatSeq * 40), 255, c.beatEnv ? c.beatEnv : 24, o.r, o.g, o.b);
  return o;
}
