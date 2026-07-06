// gig_effects.h — GigBar-effect interface for magitrac_gigbar_dmx.
//
// Same control model as magitrac_dmx (the RGBW wash node): each PixelPost effect
// is rendered by its own file (eff_*.cpp) and is a PURE function of one frame's
// control state (EffectCtx).  The difference is the OUTPUT: the GigBar 2 is a
// 4-in-1 bar (two RGB+UV Pars, two Derbies, a Laser, a dedicated Strobe panel),
// so an effect returns a GigFrame describing every section rather than a single
// RGBW.  The main sketch owns the DMX transport, master brightness and power-off.
//
// Adding an effect:
//   1. new eff_foo.cpp with `GigFrame effFoo(const EffectCtx&)`
//   2. declare effFoo() below
//   3. route an effectId to it in gigRenderEffect() (gig_effects.cpp)

#pragma once
#include <stdint.h>
#include <string.h>
#include <pixelpost_proto.h>   // PP_AUDIO_BANDS

// One frame's control snapshot, taken under the control-state spinlock and
// handed to whichever effect is live.  (Identical to the wash node — the PPOST
// control surface is the same; only the rendered output differs.)
struct EffectCtx {
  uint32_t tick;       // 20 ms show tick, locked to the sender
  uint8_t  effectId;   // PPOST effect index (pixelpost_proto.h catalogue)
  uint8_t  x, y;       // touchpad position (latched 0..255)
  bool     pressed;    // finger on the pad right now
  bool     tapped;     // a tap (touch rising-edge) happened this frame
  uint32_t beatSeq;    // mic beat counter (advances per detected beat)
  uint8_t  beatStr;    // most recent beat strength 0..255
  uint8_t  beatEnv;    // derived: 0..255 decaying since the last beat
  uint8_t  tapEnv;     // derived: 0..255 decaying since the last tap
  uint8_t  bands[PP_AUDIO_BANDS];   // mic FFT magnitudes (AGC'd 0..255)
  uint8_t  postCount;  // number of posts in the show (PPOST postCount) — spatial effects
  uint8_t  slider;     // raw slider 0..255 (also used as master brightness in main)
};

// What the whole bar should show this frame.  Defaults are full blackout, so an
// effect only sets the sections it wants lit — everything left zero stays dark
// (negative space; bright pops read against a dark bar, not a busy one).  Master
// brightness is applied to the dimmable sections by the main sketch, not here.
//
// Section value conventions follow the fixture's 23-ch chart:
//   Par  R/G/B/W/UV   — direct 0..255 dimmers (we drive the pars in manual mode).
//   Derby color       — 0=blackout, 25-49 R, 50-74 G, 75-99 B, 100-199 combos,
//                       200-224 auto single, 225-255 auto two-at-a-time.
//   Derby strobe      — 0=off, 10-239 rate, 240-255 strobe-to-sound.
//   Derby rotate      — 0=stop, 5-127 CW, 134-255 CCW (the 2nd derby is mirrored).
//   Laser color       — 0=blackout, 40-79 R, 80-119 G, 120-159 R+G, 160+ strobed.
//   Laser strobe      — 0=off, 10-239 speed, 240-255 to-sound.
//   Laser pattern     — 0=stop, 5-127 CW, 134-255 CCW.
//   Strobe panel      — pattern (0=blackout, 100-109 white manual slow→fast,
//                       230-255 white-to-sound), white dimmer, UV dimmer, speed.
struct GigFrame {
  // Par wash — applied to BOTH pars.  W is the (separate) white mix; the pars
  // here are RGB+UV, so W folds into white via equal R=G=B.  Kept in the struct
  // so the wash-node effects port across with minimal change.
  uint8_t r = 0, g = 0, b = 0, w = 0, uv = 0;
  // Derbies (both share colour/strobe; rotation is mirrored for a counter-spin).
  uint8_t derbyColor  = 0;
  uint8_t derbyStrobe = 0;
  uint8_t derbyRotate = 0;
  // Laser.
  uint8_t laserColor   = 0;
  uint8_t laserStrobe  = 0;
  uint8_t laserPattern = 0;
  // Dedicated strobe panel.
  uint8_t strobePattern = 0;
  uint8_t strobeWhite   = 0;
  uint8_t strobeUv      = 0;
  uint8_t strobeSpeed   = 0;

  // ── Per-side override (spatial effects, e.g. Circles) ──────────────────────
  // Normally both Pars (and both Derbies) show the shared colour above.  A
  // spatial effect that wants the two halves of the bar to differ sets
  // splitSides=true and fills p1*/d1Color (Par1+Derby1) and p2*/d2Color
  // (Par2+Derby2) instead; gigWrite then drives each side independently.
  bool    splitSides = false;
  uint8_t p1r = 0, p1g = 0, p1b = 0, p1uv = 0, d1Color = 0;
  uint8_t p2r = 0, p2g = 0, p2b = 0, p2uv = 0, d2Color = 0;
};

// ── Shared helpers (gig_effects.cpp) ────────────────────────────────────────
void    hsv2rgb(uint8_t h, uint8_t s, uint8_t v, uint8_t &r, uint8_t &g, uint8_t &b);
uint8_t dmxHash8(uint32_t x);          // cheap deterministic noise (fire flicker)
uint8_t hueToDerbyColor(uint8_t hue);  // map a 0..255 hue to a derby colour code

// ── Per-effect renderers (one .cpp each) ────────────────────────────────────
GigFrame effRainbow  (const EffectCtx &c);
GigFrame effSolid    (const EffectCtx &c);
GigFrame effStrobe   (const EffectCtx &c);
GigFrame effLightning(const EffectCtx &c);
GigFrame effColorSel (const EffectCtx &c);
GigFrame effCircles  (const EffectCtx &c);
GigFrame effSparkle  (const EffectCtx &c);
GigFrame effTap      (const EffectCtx &c);
GigFrame effFire     (const EffectCtx &c);
GigFrame effBlood    (const EffectCtx &c);
GigFrame effBeat     (const EffectCtx &c);
GigFrame effHeartbeat(const EffectCtx &c);
GigFrame effSpectrum (const EffectCtx &c);
GigFrame effUv       (const EffectCtx &c);
GigFrame effWhite    (const EffectCtx &c);

// effectId -> renderer (the taste-map).  gig_effects.cpp.
GigFrame gigRenderEffect(const EffectCtx &c);
