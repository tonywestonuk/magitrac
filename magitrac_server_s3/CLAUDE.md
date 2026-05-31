# MagiTrac — Server S3 (MIDI player, CoreS3 build)

Sibling sketch to `magitrac_server/` (Core Basic).  Same protocol, same
sequencer, same companion client `magitrac/` — different hardware.  Old
sketch is kept around as the fall-back build until this one is proven in
live use.

## Hardware

- **M5Stack CoreS3** (ESP32-S3) — 320×240 ILI9342C LCD, GT911 capacitive
  touch, AXP2101 PMIC, AW9523 GPIO expander all on the internal I²C bus
  (G11/G12).  No physical A/B/C buttons — M5Unified maps a bezel touch
  strip below the LCD into virtual BtnA/B/C.
- **M5Stack Module Audio v2.2** (M-Bus) — ES8388 codec, full-duplex on
  one I²S peripheral.  CoreS3-specific pin map: BCK=G0, MCLK=G7, WS=G6,
  DOUT=G13, DIN=G14, codec I²C on the internal bus (G12/G11) at address
  0x33.  Replaces the Core Basic's separate PDM mic + DAC2 paths — no
  I²S0 mutex needed any more.
- **M5Stack Module MIDI** (Grove Port A — only Grove port accessible
  with the Module Audio stacked) — **UART1** (NOT UART2 — Serial2 was
  dropped from Arduino-ESP32 v3.x defaults on the S3 and UART2 misbehaves
  on remapped pins).  Pin convention is **G2 = host TX → MIDI OUT DIN**
  and **G1 = host RX ← MIDI IN DIN** (verified by direct UART1 loopback
  on the physical module — the Grove cable wiring is opposite of the
  "G1=TX intuition").  Direct UART LL FIFO writes (`uart_ll_write_txfifo`
  on `UART_NUM_1`) for low-latency MIDI out.
- **SD card** (CoreS3 base) — SPI on G36 (SCK) / G35 (MISO) / G37 (MOSI)
  / G4 (CS).  `SPI.begin(36, 35, 37, 4)` in `setup()` before
  `commandsInit()` so `SD.begin(4)` picks up the right bus.

## Core innovation: performer-sync engine

This is what makes MagiTrac different from a normal sequencer.  See the
client's `CLAUDE.md` for the full spec.  Server-side highlights:

- `seqOnPerformerNote()` (in `midi_player.cpp`) is the snap path.  When
  the performer plays a MIDI note, it scans the input track (col 0) of the
  current pattern for `WAIT`/`SYNC` rows whose pitch class matches.  On a
  hit, it snaps `seqRow` to the cue row and fires `seqPlayRow()`
  immediately — *zero latency* between key-down and MIDI-out.
- BPM is **derived** from the time delta between snaps
  (`seqRecordSnap` / `seqCurrentBPM`).  Clamped to song's `minBPM`/`maxBPM`.
- `seqWaiting=true` halts playback at a `WAIT` row until the performer
  resolves it.  500 ms timeout returns to row 0 of the (possibly queued)
  block.

Don't add inline `delay()` or anything that blocks `sequencerTick()`.

## Sequencer file: `midi_player.cpp`

This is the heart of the server.  Key functions:

- `sequencerTick()` — call every loop.  Returns early if not yet time
  to play the next row.  Detects `WAIT`, advances rows, handles
  block-end navigation.
- `seqPlayRow()` — walks the linked-list nodes at `seqRow` and emits
  MIDI for output columns.  Col 0 nodes are skipped (handled by
  tick/performer logic).
- `seqSendColumnSetup()` — flushes bank / program / volume for every
  column in a pattern.  Called at start, resume, block transition,
  and WAIT timeout.
- `seqOnPerformerNote()` — performer snap + transpose + block-switch.

### Patch-change rules (important)

The sequencer does **not** send program/bank changes inline with note-ons,
because the live-performance nature of WAIT means the lead time before any
given note can be near-zero.  Instead:

1. **Block start / resume / WAIT timeout** → `seqSendColumnSetup()`
   flushes any pending bank/program/volume for every column whose cache
   doesn't match.
2. **Mid-block** → a `NOTE_OFF` cell flushes bank/program/volume for that
   column.  The composer places an OFF where they want a patch change to
   happen, with enough room afterwards for the synth to load before the
   next note-on.
3. **NOTE_ON** → only the note-on is sent.  Never bank/program.

`seqChanBank[16]` / `seqChanProg[16]` / `seqChanVol[16]` are the
per-channel cache (init `0xFF` to force first send).

## Communications

- ESP-NOW transport — abstracted behind `MagiComms` /
  `MagiCommsTransport` (see `MagiComms.h`).  Both client and server use the
  legacy `esp_now.h` C API in `MagiCommsEspNow.cpp` (an Arduino-3.x
  `ESP32_NOW` class variant once existed behind `#ifdef
  MAGICOMMS_ESPNOW_ARDUINO3X` but the sketch-level define never propagated to
  library compilation so the branch never ran — removed).
- HMAC-based auto-reconnect via `PairNVS`.
- Reliable transfer (song save, note-set bulk) uses ACK + retry with 200ms
  timeout — see `commands_server.ino` for the receive side, and the client's
  `sendSongToServer` / `sendNoteSetReliable` for the send side.

## Files

| File | Purpose |
|---|---|
| `magitrac_server_s3.ino` | Entry point, `setup()`/`loop()`, panel UI |
| `midi_player.cpp/.h` | Sequencer + performer-sync engine |
| `commands_server.ino` | MagiLink message dispatch (load/save/patch/note-set) |
| `pairing.ino` | Pairing state machine + auto-reconnect |
| `debug_log.cpp/.h` | `debugPrintf` (server-only) |
| `audio_codec.cpp/.h` | Thin wrapper around the shared ES8388 (Module Audio v2.2) |
| `SamplePlayer.cpp/.h` | WAV playback via the shared codec (mono → stereo, resampled) |
| `mic_spectrum.cpp/.h` | On-board MEMS mic → FFT bands + beat detect → pixel_post |

## Shared core: `magitrac_lib`

The shared types and transport now live in the sibling **`magitrac_lib`**
Arduino library (`../magitrac_lib/src/`), pulled in via
`#include <magitrac_lib.h>` at the top of `magitrac_server.ino`.  Files
provided by the lib (don't add local copies):

- `TrackerData.{h,cpp}` — Song / Pattern / NoteNode
- `NoteGrid.{h,cpp}` — sparse note-pool abstraction
- `SongMigration.h` — versioned file readers v11..v18
- `MagiMsg.h` — wire message structs + types
- `MagiComms.h` + `MagiCommsEspNow.{h,cpp}` — transport abstraction
- `PairNVS.{h,cpp}` — NVS pairing storage + HMAC helpers

Symlink `magitrac_lib/` into `~/Documents/Arduino/libraries/` so the
Arduino IDE / arduino-cli finds it.  Edit shared files once in
`magitrac_lib/src/`; both sketches pick the change up.

## Key data layout

Same as the client (`TrackerData.h` is identical):

- `MAX_COLUMNS = 21` (col 0 = input, cols 1–20 = MIDI output).
- `Song::columns[MAX_COLUMNS]` holds per-column MIDI config (channel,
  bank, program, vol, transpose, mute, name).
- Sparse `NoteNode` pool of 4000 nodes shared across patterns.
- `SONG_FILE_VERSION = 18`.

## Conventions

- Prefer direct UART LL writes for MIDI — every microsecond between
  performer-note and MIDI-out matters for feel.
- `seqWriteStatus()` implements MIDI **running-status** — only emit a
  status byte when it differs from the last one sent.  Reset
  `seqOutStatus = 0` whenever you want to force a fresh status.
- `seqChanBank/Prog/Vol` cache: keep in sync with what was actually sent.
  Code that bypasses the cache (e.g. forced setup) must also update it.
- No comments unless the *why* is non-obvious.

## Build

Arduino IDE / arduino-cli targeting ESP32 (M5Stack board).  No
PlatformIO config in tree.  `.ino` files are compiled together with
the `.cpp/.h` files.
