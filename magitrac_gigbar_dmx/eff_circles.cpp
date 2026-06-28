// eff_circles.cpp — Circles 1 (effect 11), spatialised onto the bar.
//
// The post effect (la_Circles1) keeps a 100x100 HSV canvas: every 75 ticks it
// reseeds a random circle centre, each tick the whole canvas fades and a growing
// ring is drawn, and each post shows one VERTICAL SLICE (column xpos) of that
// canvas — xpos spread across the posts by ordinal.  We shadow two of those
// slices on the bar:
//     Par1 + Derby1  ==  top pixel of post 4   (ordinal 3)
//     Par2 + Derby2  ==  top pixel of post 3   (ordinal 2)
//
// We don't need the whole canvas — only the single "top" pixel of those two
// columns — so we track just those two cells.  The fade is per-cell and so is
// exact; the ring crossing is a plain distance test (the post fills a 32-gon
// annulus, which is the same to within ~1 px — visually identical).  The circle
// centre is reseeded with the SAME seed the post uses (the cycle-start tick), and
// our `tick` is derived identically (currentTickMs()/20), so circ_x/circ_y match
// the posts bit-for-bit.
#include "gig_effects.h"
#include <Arduino.h>

// Which post each side of the bar shadows (1-based post number -> 0-based
// ordinal = number-1).  Easy to retune if the bar sits at different posts, or to
// swap which side maps where.
static const uint8_t CIRCLES_POST_COUNT   = 5;   // assume a 5-post show (configurable on the posts)
static const uint8_t CIRCLES_PAR1_ORDINAL = 3;   // Par1/Derby1 <- post 4
static const uint8_t CIRCLES_PAR2_ORDINAL = 2;   // Par2/Derby2 <- post 3
// Canvas row sampled as the post's "top".  Post LED i maps to row i (step=1 at
// 100 LEDs); flip to 99 if the strip runs the other way and this looks inverted.
static const int     CIRCLES_TOP_ROW  = 0;
static const uint8_t CIRCLES_DERBY_ON  = 40;     // light a derby only above this brightness

struct PixState { uint8_t hue, sat, val; };

static uint32_t s_lastCycle = 0xFFFFFFFFu;
static uint32_t s_lastTick  = 0xFFFFFFFFu;
static int      s_cx = 50, s_cy = 50;
static PixState s_a = {0, 0, 0};   // Par1/Derby1  (post 4)
static PixState s_b = {0, 0, 0};   // Par2/Derby2  (post 3)

static int xposFor(uint8_t ordinal, uint8_t count) {
  int xp;
  if (count <= 1) xp = 50;
  else            xp = (int)(ordinal * (100.0 / (count - 1)) + 0.5);  // == post's round()
  if (xp > 99) xp = 99;
  if (xp < 0)  xp = 0;
  return xp;
}

GigFrame effCircles(const EffectCtx &c) {
  uint32_t tick = c.tick;

  // Reseed the circle centre once per 75-tick cycle.  la_Circles1 seeds with the
  // tick where tick%75==0, i.e. the cycle-start tick — use that directly so a
  // skipped frame on the exact boundary can't desync us.
  uint32_t cycle = tick - (tick % 75);
  if (cycle != s_lastCycle) {
    s_lastCycle = cycle;
    randomSeed(cycle);
    s_cx = random(100);
    s_cy = random(100);
  }

  int xpA = xposFor(CIRCLES_PAR1_ORDINAL, CIRCLES_POST_COUNT);
  int xpB = xposFor(CIRCLES_PAR2_ORDINAL, CIRCLES_POST_COUNT);

  // Advance the two tracked pixels once per new tick (fade, then ring hit — same
  // order as the post: fade loop first, drawCircle second).
  if (tick != s_lastTick) {
    s_lastTick = tick;
    int radius = 2 * (int)(tick % 75);
    int inner  = radius - 15; if (inner < 0) inner = 0;
    int r2 = radius * radius, i2 = inner * inner;

    PixState *px[2] = { &s_a, &s_b };
    int       col[2] = { xpA, xpB };
    for (int k = 0; k < 2; k++) {
      PixState *p = px[k];
      if (p->val > 10) { p->hue -= 10; p->val -= 10; }   // fade (uint8 wrap matches post)
      else             { p->val = 0;   p->hue = 0;   }
      int dx = col[k] - s_cx, dy = CIRCLES_TOP_ROW - s_cy;
      int d2 = dx * dx + dy * dy;
      if (d2 <= r2 && d2 >= i2) { p->hue = 255; p->sat = 255; p->val = 255; }  // ring "white"
    }
  }

  GigFrame o;
  o.splitSides = true;
  hsv2rgb(s_a.hue, s_a.sat, s_a.val, o.p1r, o.p1g, o.p1b);
  o.d1Color = (s_a.val > CIRCLES_DERBY_ON) ? hueToDerbyColor(s_a.hue) : 0;
  hsv2rgb(s_b.hue, s_b.sat, s_b.val, o.p2r, o.p2g, o.p2b);
  o.d2Color = (s_b.val > CIRCLES_DERBY_ON) ? hueToDerbyColor(s_b.hue) : 0;
  return o;
}
