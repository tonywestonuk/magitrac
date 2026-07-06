# Getting Started

MagiTrac is a two-device system, optionally extended with wireless light
nodes. The **client** is where you write and control songs; the **server**
is the box on stage that plays them.

```
[Performer's MIDI keyboard] ──MIDI──▶ [SERVER: M5Stack CoreS3] ──MIDI──▶ [Synth]
                                            │        ┊
                                       WiFi │        ┊ ESP-NOW broadcast
                                            ▼        ┊
                                  [CLIENT: LilyGo T5]  ┊▶ [Light nodes]
                                  (editing + control)
```

The defining feature is the **performer-sync engine**: the song's input
track carries WAIT/SYNC cues, and the sequencer follows the *performer* —
snapping to cues when you play the matching note and deriving the tempo
from your timing. The band doesn't follow the click; the click follows the
band. See the [Reference](reference.md) for how the cues work.

## Hardware

### Client
- **LilyGo T5 4.7" S3** — ESP32-S3 with a 960×540 4-shade e-paper display
  and GT911 capacitive touch. (An M5PaperS3 also works — the sketch
  detects it at runtime.)
- MicroSD card (used for backups and setlists).

### Server
- **M5Stack CoreS3** (ESP32-S3).
- **M5Stack Module Audio v2.2** (ES8388 codec) — stacked on the M-Bus;
  provides the speaker (samples, organ) and microphone (chord recogniser,
  beat detection).
- **M5Stack Module MIDI** — plugs into **Grove Port A** (the only Grove
  port still reachable with Module Audio stacked). MIDI IN comes from the
  performer's keyboard; MIDI OUT goes to your synth.
- MicroSD card, **FAT32, ≤32 GB** (larger exFAT cards won't mount over
  USB).

### Light nodes (optional)
Original **M5Stick** (ESP32-PICO-D4, the one with the small OLED) plus a
Grove RS-485 DMX interface, driving either a 7-channel RGBW wash or a
Chauvet GigBar 2. See [Lights](lights.md).

## Building the firmware

Everything builds from the **Arduino IDE** (or arduino-cli) with the
ESP32 board package installed. There is no PlatformIO config. Each sketch
directory compiles as a unit — open the `.ino` and its sibling `.cpp/.h`
files come along.

### Libraries

Symlink (or copy) these into `~/Documents/Arduino/libraries/`:

- **`magitrac_lib`** — in this repo; shared song model + transport used by
  both client and server:
  `ln -s /path/to/magitrac/magitrac_lib ~/Documents/Arduino/libraries/`
- **[EPD_Painter](https://github.com/tonywestonuk/EPD_Painter)** and
  **[gt911-arduino](https://github.com/tonywestonuk/gt911-arduino)** — the
  client's e-paper painter and touch driver.
- **`pixelpost_proto`** — the light-broadcast protocol header, needed by
  the server and the light nodes (companion library, distributed
  separately).
- **`M5Unified`** and **`M5Module-Audio`** — from the Arduino library
  manager, for the server.
- **`U8g2`** (for U8x8) — for the light nodes' OLED.
- **`I2C_BM8563`** — the client's RTC.

### Board settings

| Sketch | Board | Notes |
|---|---|---|
| `magitrac/` (client) | ESP32S3 Dev Module | **PSRAM enabled (OPI)** — the song lives in PSRAM |
| `magitrac_server_s3/` (server) | M5Stack CoreS3 | **USB Mode = USB-OTG (TinyUSB)**, USB CDC On Boot = Disabled — required for the SD-over-USB feature |
| `magitrac_dmx/`, `magitrac_gigbar_dmx/` (lights) | M5Stick-C / ESP32 PICO | no PSRAM |

### Server secrets file (optional)

`magitrac_server_s3` compiles out of the box with placeholder WiFi
credentials. If you want the bench "OTA lights from your computer" route,
copy `wifi_secrets.h.example` to `wifi_secrets.h` (gitignored) and fill in
your home WiFi + firmware URL. The gig-time light-flashing route
(server-hosted, no router) needs no secrets — see the
[Server Guide](server-guide.md#pixelpost-flash-screen-firmware-for-the-light-nodes).

## Preparing the server's SD card

Format FAT32 and create these folders (or copy files over USB later —
see the Server Guide's USB screen):

| Folder | Contents |
|---|---|
| `/songs/` | song files (`.mgt`) — created by saving from the client |
| `/samples/` | 16-bit PCM WAVs for sample columns (manifest auto-managed) |
| `/drumtracks/` | drum-pattern text files for the import feature |
| `/setlists/` | setlists (managed by the client) |
| `/autosave/` | 30-second autosave drafts (auto-created) |
| `/firmware/` | `pixel_post.ino.bin` when field-flashing light nodes |

## First pairing

Out of the box the two devices don't know each other. Pair once; from then
on they find each other automatically at power-on.

1. **Power both devices.**
2. **Server**: hold the touch strip *below* the LCD for **2 seconds**. The
   screen goes green: "PAIR MODE / Listening". You have 60 seconds.
3. **Client**: tap **MENU → PAIR**. It shows "Put server in pair mode…"
   then, when it finds the server, a large **4-character code**.
4. **Check the same code shows on the server's screen**, then tap
   **CONFIRM** on the client.
5. The server shows "Paired!" and restarts. It creates its own WiFi
   network (`Magitrac_XXXX`, generated password — nothing to type); the
   client joins it automatically and the header shows **Connected**.

The default (and recommended) mode is this dedicated server-hosted
network — fixed addresses, nothing else on the air. If you'd rather both
devices join an existing network, set **External AP** mode in the client's
MENU → SETTINGS → WIFI SETTINGS *before* pairing.

## Your first song

1. On the client, tap the song name (bottom bar) → **NEW** → give it a
   name.
2. Tap a column header → set **MIDI CH** to your synth's channel, pick a
   program, name it.
3. Tap cells in the grid (or long-press the column header for the
   piano-roll) and place some notes. Everything streams to the server as
   you edit.
4. Tap **PLAY**. The server drives your synth.
5. To make it *follow you*: long-press the **MIDI IN** column header and
   put a **WAIT** on row 0 with the note you'll play. Now playback holds
   at the top until you play that note — and every WAIT you add becomes a
   sync point that pulls the tempo to your performance.

From there: [Tracker Guide](tracker-guide.md) for every page,
[Reference](reference.md) for the exact cue/effect semantics,
[Server Guide](server-guide.md) for the stage box, and
[Lights](lights.md) to add the light show.
