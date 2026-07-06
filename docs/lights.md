# Lights (PixelPost / DMX nodes)

The server continuously broadcasts the state of the light show over
ESP-NOW — one small "PPOST" packet on every change plus a 2-second
heartbeat, carrying the current effect, an X/Y position, a slider level,
and a shared animation clock. Light nodes are pure **receive-only
sniffers**: they lock their animation clocks to the broadcast, so every
fixture renders the same show frame-for-frame with no pairing handshake,
no ACKs, and no limit on how many nodes listen.

While a sound-reactive effect is selected, the server also opens its
microphone and broadcasts a second packet stream with FFT band levels and
beat detections — the lights literally hear the room through the server.

This repo contains two DMX node sketches (the addressable-LED "pixel
posts" themselves are a companion project sharing the same protocol):

| Sketch | Hardware | Fixture |
|---|---|---|
| `magitrac_dmx/` | original M5Stick + Grove RS-485 DMX | 7-channel RGBW wash (dimmer/R/G/B/W/strobe/mode, address D001) |
| `magitrac_gigbar_dmx/` | original M5Stick + Grove RS-485 DMX | Chauvet **GigBar 2** in 23-channel mode (2× Par, 2× Derby, laser, strobe panel, address D001) |

Both emit DMX at 50 Hz on GPIO 25 and show status (effect name, RX
activity, WiFi channel) on the M5Stick's OLED.

## Binding a node to the show

There's no code or confirm step for lights — a node simply **adopts the
first show it hears**:

1. **Hold the M5Stick's big button (BtnA) for 1.5 s.** The fixture starts
   **strobing white** while it scans WiFi channels 1/6/11.
2. The first server broadcast it hears is adopted — the fixture goes
   **solid green** for a moment, saves the channel, and starts rendering.

A brand-new node auto-enters this mode on first boot. After that it
remembers its channel and re-scans by itself if the show goes quiet.

## Driving lights from a song — the PXL POST column

Any output column becomes a light column by setting its **MIDI CH** to
`PXL` in the column editor. Then the sequencer rows drive the rig in
lock-step with the music — and because the whole song follows the
performer, so do the lights:

| Cell part | Meaning on a PXL column |
|---|---|
| **Note** | selects the effect: C-0 = effect 0, C♯0 = 1, D-0 = 2… (one octave = 12 effects, so the picker matters more than the pitch) |
| **Velocity** | the intensity slider (brightness / density / rate, per effect) |
| **Effect + param hex** | X and Y "touchpad" coordinates for positional effects |
| **OFF** | "lift finger" — releases held/momentary effects |

Rather than memorising ordinals, edit PXL cells with the **PixelPost
picker** (the `PXL` button in the note editor): tap effects and drag the
slider/touchpad *live* — the rig responds in real time and the cell
records what you settled on.

## The effect catalogue

28 effects, indexed 0–27 (entries marked 🎤 are sound-reactive and switch
the server's mic on):

`0` BLOCK-1 · `1` SINE WAVE · `2` BLOCK-2 · `3` FIRE · `4` POW! ·
`5` COLOR WHEEL · `6` Rainbow · `7` Sparkle · `8` Strobe · `9` Meteor ·
`10` Springs 🎤 · `11` Circles 1 · `12` Chaser-1 · `13` Sound Spectrum 🎤 ·
`14` Blood · `15` Plasma · `16` Wave · `17` Heartbeat · `18` Ripple 🎤 ·
`19` Lightning · `20` Beat Test 🎤 · `21` Tainted 🎤 · `22` Enola Gay 🎤 ·
`23` Relay 🎤 · `24` FREED 🎤 · `25` Solid Beat 🎤 · `26` UV · `27` WHITE

Each node interprets an effect in whatever way suits its fixture. The
GigBar renders 15 distinct looks — highlights:

- **Beat** (20/23/25) — pars pulse on the detected beat with the hue
  stepping each hit, derbies strobe to the sound, laser stabs on the
  strongest beats.
- **Lightning** (19) — darkness with random full-bar white strikes; the
  slider sets the storm's intensity, a tap forces a strike.
- **POW!** (4) — dark until you tap, then a white pop that decays.
- **WHITE** (27) — press-and-hold white swell (this is the Performance
  page's WHITE button).
- **Color Wheel** (5) — the X/Y pad becomes a live colour picker.
- **UV** (26) — every UV emitter on, nothing else.

Effects a node doesn't implement fall back to a rainbow sweep, so an
unmapped ordinal never leaves a fixture dark. The RGBW wash implements a
smaller set (beat, fire, heartbeat, rainbow, solid, strobe, colour wheel,
spectrum, POW!) and rainbows the rest.

## Live control at the gig

- **Performance page light strip** — `< FX` / `FX >` step the effect
  (grabbing control from the song's PXL track), **WHITE** is momentary
  while held, **RELEASE** hands control back to the track.
- **MENU → POSTS** — full manual board: paged effect grid, brightness
  slider, X/Y touchpad, plus **PWR OFF** (blackout, confirmed) and rig
  settings (max brightness, flash smoothing).

While manual control is active the song's PXL column is ignored, so a
grabbed look holds until you RELEASE.

## Updating light-node firmware at a gig

No router or laptop-on-stage needed: drop the new `pixel_post.ino.bin`
into `/firmware/` on the server's SD card (over USB), and the server
reboots into its **FLASH** screen — it serves the image from its own WiFi
while the nodes pull it. Full steps in the
[Server Guide](server-guide.md#pixelpost-flash-screen-firmware-for-the-light-nodes).
