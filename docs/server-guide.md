# Server Guide (M5Stack CoreS3)

The server is the "brain" of MagiTrac: it runs the sequencer, plays MIDI out
to your synth, plays WAV samples through its speaker, listens to the room
through its microphone, and broadcasts to the light nodes. It is designed to
be glanceable and headless-tolerant — at a gig you mostly leave it alone.

## The touch shell

The CoreS3 has no physical buttons. There are two touch surfaces:

- **The glass** (the LCD itself) — does whatever the current screen needs:
  tap a list row, drag to scroll, drag a drawbar, freeze a display.
- **The bezel strip** (the touch-sensitive band *below* the LCD):
  - **Short tap** — cycle to the next screen.
  - **Hold 2 seconds** — enter pairing mode (see below).

Screens cycle in a fixed loop:

**Songs → Samples → Chord → Scope → USB → Organ → back to Songs**

Every screen has a coloured header (title + battery icon) and a footer hint
line reminding you what touches do.

## Songs screen (default at boot)

- Header shows **"MagiTrac"**, the current tempo (e.g. "120 BPM"), and a
  small **green dot** when a client is connected over WiFi.
- The list shows every `.mgt` song in `/songs/` on the SD card. The first
  row is always **"-- OFF --"** (no song).
- The loaded song's row is highlighted — **white** when stopped, **green**
  when playing.
- **Tap a row** to load that song. **Tap the loaded song again** to toggle
  play/stop. Tap "-- OFF --" to unload.
- **Drag** to scroll the list.

### Tempo idle screen

After 10 seconds without a touch (on the list/USB screens), the display
flips to a giant BPM readout with an average of recent readings — readable
from across the stage. Any touch returns to where you were.

## Samples screen

- Lists every `.wav` in `/samples/` on the SD card (up to 64).
- **Tap a row** to audition it through the speaker; the playing row goes
  green. Tap again to stop. **Drag** to scroll.

### Putting samples on the card

Drop 16-bit PCM WAV files (mono or stereo, 8–48 kHz — they're resampled
to the codec's 32 kHz) into `/samples/`. Other formats (8-bit, 24-bit) are
skipped, not played.

On boot the server assigns each new WAV a numeric ID (1–127) and records it
in `/samples/samples.txt` (one `id=filename` line per sample). That ID is
what a song's sample column uses to trigger the sound, and it stays stable
across reboots — you never need to edit the file by hand.

## Chord screen

Listens to the built-in microphone and names the chord it hears, with the
chord tones spelled out and a 12-bar chroma display underneath (chord tones
in green). Shows **"listening..."** until it locks on.

- **Tap the glass** to freeze/unfreeze the readout (an amber **HOLD** shows
  in the header) — handy for catching a chord mid-song.

## Scope screen

A live oscilloscope of the microphone with a peak-to-peak level readout.
**Tap the glass** to freeze/run the trace.

## USB screen — getting files on and off the SD card

Tap the green **ENTER** button and the server reboots as a USB
mass-storage device: plug it into a computer over USB-C and the SD card
mounts as a drive called **"MagiTrac SD"**. Copy songs, samples, or
PixelPost firmware on or off, then **eject on the computer** (the server
reboots back to normal) or tap the screen to exit.

Things to know:

- **FAT32 cards only (≤32 GB).** Larger exFAT cards enumerate but won't
  mount reliably.
- USB mode is a full handoff — the sequencer, audio, WiFi and MIDI are all
  offline while the card is mounted.
- Builders: the sketch must be compiled with **Tools → USB Mode →
  USB-OTG (TinyUSB)** (and USB CDC On Boot disabled), or the ENTER button
  will tell you so instead of mounting.

## Organ screen

A nine-drawbar Hammond-style organ, played live from the MIDI IN socket.
Drag the drawbars (labelled with their footages: 16, 5.3, 8, 4, 2.7, 2,
1.6, 1.3, 1) to set the registration 0–8; a voice-count indicator lights in
the header while notes sound. The organ owns the speaker while this screen
is up. Additional voice models and effects (tonewheel variants, vibrato /
chorus, Leslie, drive) are selectable remotely from the client.

## Pairing (bezel hold, 2 s)

Hold the bezel strip for 2 seconds and the screen turns green:
**"CLIENT SERVER MODE" / "PAIR MODE" / "Listening"**. Put the client in
pair mode too; the server then shows a 4-digit **confirm code**. Check the
same code shows on both devices and confirm **on the client**. The server
shows "Paired!" and restarts into its WiFi role. A tap on the bezel cancels;
the window times out after 60 seconds. See
[Getting Started](getting-started.md) for the full first-run walkthrough.

## PixelPost FLASH screen (firmware for the light nodes)

If the server finds a `pixel_post.ino.bin` in `/firmware/` on the SD card
at boot, it shows the **PixelPost FW** screen instead of starting normally:
firmware size, a **"Posts: N"** counter, a green **FLASH** button and a
brown **DONE** button.

Field-updating your lights at a gig, with no router needed:

1. USB screen → ENTER → copy the new `pixel_post.ino.bin` into
   `/firmware/` → eject.
2. The server reboots into the FLASH screen. Tap **FLASH** — the light
   nodes join the server's own WiFi and pull the firmware; the Posts
   counter ticks up as each one finishes. Tap FLASH again for stragglers.
3. Tap **DONE** — the file is renamed `*.done` and the server reboots into
   normal operation.

(The FLASH button greys out with "Server not in AP mode" if the server
isn't hosting its own network.)

## Crash banner

If the previous boot ended in a crash, the next boot opens with a red
**"LAST BOOT CRASHED"** banner summarising what happened (reset reason,
what the machine was doing, uptime, boot count, and a backtrace if one was
captured). It self-clears after a few seconds and boot continues — nothing
to do, it's purely diagnostic. Normal power-ons skip it entirely.

## Physical hookup

| Port | Connection |
|---|---|
| Grove Port A | **M5Stack Module MIDI** — the only Grove port reachable with Module Audio stacked |
| MIDI IN (G1) | from the performer's keyboard MIDI OUT |
| MIDI OUT (G2) | to your synth / sound module MIDI IN |
| SD card slot | FAT32 card with `/songs`, `/samples`, `/firmware` |
| Module Audio | speaker out (samples + organ) and microphone in (chord/scope/beat detection) |

## What happens at power-on

1. Crash banner, if the last boot faulted.
2. Audio codec init (the unit still runs without Module Audio — audio is
   just disabled).
3. MIDI up, then the Module MIDI's onboard SAM2695 synth is configured:
   GS Reset + everything muted **except channel 10 (drums)**, so the module
   plays drum kits without doubling your melodic parts. This is re-sent
   every boot (the chip has no memory).
4. SD card mounted, song and sample lists scanned.
5. WiFi: in the normal (paired) setup the server creates its own network
   named **`Magitrac_XXXX`** with a stored random password; the client
   joins it automatically. Unpaired servers leave WiFi off.
6. The Songs screen appears — or the FLASH screen if new PixelPost
   firmware is waiting on the card.
