// dmx_effects.h — wash-effect interface for magitrac_dmx.
//
// Each PixelPost effect is rendered by its own file (eff_*.cpp).  Every effect
// is a PURE function of one frame's control state (EffectCtx) and returns the
// RGBW the wash should show.  The main sketch owns the DMX transport, master
// brightness and power-off.
//
// Adding an effect:
//   1. new eff_foo.cpp with `WashColour effFoo(const EffectCtx&)`
//   2. declare effFoo() below
//   3. route an effectId to it in washRenderEffect() (dmx_effects.cpp)

#pragma once
#include <stdint.h>
#include <string.h>
#include <pixelpost_proto.h>   // PP_AUDIO_BANDS

// RGBW the wash should display this frame (master brightness applied separately).
struct WashColour { uint8_t r, g, b, w; };

// One frame's control snapshot, taken under the control-state spinlock and
// handed to whichever effect is live.
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
};

// ── Shared helpers (dmx_effects.cpp) ────────────────────────────────────────
void    hsv2rgb(uint8_t h, uint8_t s, uint8_t v, uint8_t &r, uint8_t &g, uint8_t &b);
uint8_t dmxHash8(uint32_t x);   // cheap deterministic noise (fire flicker)

// ── Per-effect renderers (one .cpp each) ────────────────────────────────────
WashColour effRainbow  (const EffectCtx &c);
WashColour effSolid    (const EffectCtx &c);
WashColour effStrobe   (const EffectCtx &c);
WashColour effColorSel (const EffectCtx &c);
WashColour effTap      (const EffectCtx &c);
WashColour effFire     (const EffectCtx &c);
WashColour effBeat     (const EffectCtx &c);
WashColour effHeartbeat(const EffectCtx &c);
WashColour effSpectrum (const EffectCtx &c);

// effectId -> renderer (the taste-map).  dmx_effects.cpp.
WashColour washRenderEffect(const EffectCtx &c);
