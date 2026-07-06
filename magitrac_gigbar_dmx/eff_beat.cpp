// eff_beat.cpp — Beat Test / Relay / Solid Beat.
// The big-energy look.  Pars pulse on the mic-detected beat (beatEnv) with the
// hue stepping once per beat; the derbies run strobe-to-sound and counter-spin;
// the laser kicks in red+green on the strongest beats.  beat_seq only advances
// while the server runs a needsMic effect, so this genuinely follows the music.
#include "gig_effects.h"

GigFrame effBeat(const EffectCtx &c) {
  GigFrame o;
  hsv2rgb((uint8_t)(c.beatSeq * 40), 255, c.beatEnv ? c.beatEnv : 24, o.r, o.g, o.b);

  // Derbies: auto two-colour, strobing to the music, slowly rotating.
  o.derbyColor  = 230;   // auto two-at-a-time
  o.derbyStrobe = 250;   // strobe-to-sound
  o.derbyRotate = 70;    // medium clockwise (derby 2 counter-spins)

  // Laser only on a strong beat, so it punctuates rather than runs constantly.
  if (c.beatEnv > 180) {
    o.laserColor   = 120;  // red + green on
    o.laserPattern = 90;   // rotating clockwise, fast-ish
  }
  return o;
}
