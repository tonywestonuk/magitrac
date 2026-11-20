# MagiTrac — Server S3 (MIDI player, CoreS3 build)

The live server: sequencer, MIDI out, sample player, organ synth and the
song library on SD.  Its companion is the client sketch `magitrac/`.

`magitrac_server/` (M5Core Basic) is a frozen legacy sketch — it no longer
matches the protocol.  Land every server change here.

## Hardware

- **M5Stack CoreS3** (ESP32-S3) — 320×240 ILI9342C LCD (used in portrait,
  240×320), GT911 capacitive touch, AXP2101 PMIC, AW9523 GPIO expander all
  on the internal I²C bus (G11/G12).  No physical A/B/C buttons — M5Unified
  maps a bezel touch strip below the LCD into virtual BtnA/B/C.
- **M5Stack Module Audio v2.2** (M-Bus) — ES8388 codec, full-duplex on
  one I²S peripheral.  CoreS3-specific pin map: BCK=G0, MCLK=G7, WS=G6,
  DOUT=G13, DIN=G14, codec I²C on the internal bus (G12/G11, `Wire1`) at
  address 0x33.  Replaces the Core Basic's separate PDM mic + DAC2 paths —
  no I²S0 mutex needed any more.
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
- BPM is **derived** from the time delta between `WAIT` snaps
  (`seqRecordSnap` / `seqCurrentBPM`); a `SYNC` snap realigns without
  changing tempo.  Clamped to song's `minBPM`/`maxBPM`.
- `seqWaiting=true` halts playback at a `WAIT` row until the performer
  resolves it.  On timeout — 500 ms for `WAIT`, 1 s for `WAT1`, 2 s for
  `WAT2` — it returns to row 0 of the (possibly queued) block.
- **Fumble rule — the last key counts.**  A key that takes effect opens a
  window of half a row.  A *different* key inside it is taken as what the
  performer meant: `seqOnPerformerNote()` puts the sequencer back to where it
  was before the first key (`SeqPos`) and acts on the new key from there,
  silencing anything the first key started that the new one doesn't replace.
  The first key stands only if the second is the same key again, would do
  exactly the same thing, or means nothing where the first was pressed.
  `test_seq.cpp` has the cases; don't change this without the owner.

Don't add inline `delay()` or anything that blocks `sequencerTick()`.

## Sequencer file: `midi_player.cpp`

This is the heart of the server.  It runs on a **dedicated MIDI task**
(`midiTaskFn` in `magitrac_server_s3.ino`), not in `loop()`.  That task
alone owns MIDI out, the running-status cache and the playhead.  Key
functions:

- `sequencerTick()` — called every pass of the MIDI task.  Returns early if
  not yet time to play the next row.  Detects `WAIT`, advances rows, handles
  block-end navigation.
- `seqPlayRow()` — walks the linked-list nodes at `seqRow` and emits
  MIDI for output columns.  Col 0 nodes are skipped (handled by
  tick/performer logic).
- `seqSendColumnSetup()` — flushes bank / program / volume for every
  column in a pattern.  Called at start, resume, block transition,
  WAIT timeout and (forced) on song load.
- `seqOnPerformerNote()` — performer snap + transpose + block-switch.

The public control functions (`sequencerStart/Stop/Pause/Seek/Goto/…`) are
safe to call from any task: each hands its work to the MIDI task and returns
once it has run there — see the "Cross-task entry" block at the end of the
file.  New control entry points must go through the same hand-off, never
touch sequencer state or `midiTx` directly from another task.

**Host test:** `test_seq.cpp` exercises the sequencer on the Mac — WAIT/SYNC
snaps, block ends, live note edits, and the MIDI bytes sent
(mocks in `test_mocks/`).  Run it from this directory in a terminal, not
through the Arduino IDE (it writes a `test_seq` binary here — don't commit
it):

```
g++ -std=c++11 -Wno-format -DUNIT_TEST -I test_mocks -I . \
    -I ../magitrac_lib/src test_seq.cpp -o test_seq && ./test_seq
```

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

The one thing a row *does* send inline besides notes is **pan**: output-column
effect `0A` (`EFFECT_PAN`) sends MIDI CC 10 with the param as the position
(00 left, 40 centre, 7F right) just before that row's note.  Pan is sent
ONLY where a PAN effect is written — never on song load, start or stop; the
starting pan is left to the synth and the preset the program change selects.

`seqChanBank[16]` / `seqChanProg[16]` / `seqChanVol[16]` are the
per-channel cache (init `0xFF` to force first send).

## Communications

- **MagiLink** (`gMagiLink`) — one persistent TCP connection, the data
  path.  The server accepts on `MAGI_PORT`, the client connects.  Messages
  are raw structs from `MagiMsg.h`, each starting `{uint8 id, uint16
  length}` with no other framing; handlers are registered per message id in
  `setup()`.  The callback table is fixed-size and a registration past the
  end fails with only a log line — check the count when adding a handler.
- **MagiUdpLink** — loss-tolerant updates to the client: sequencer position,
  preview row, MIDI note-in.  The `MSG_SERVER_ANNOUNCE` discovery beacon
  goes out over UDP too.  No ACK, no retry.
- **ESP-NOW** (`MagiCommsEspNow`) — the pairing ceremony and the PixelPost
  light broadcasts **only**.
- **Network** — normally the server hosts its own hidden softAP at a fixed
  address (`MAGI_SERVER_IP_*`, DHCP server stopped, 11g/n only, slow
  beacon) and the client joins it.  The alternative mode stored at pairing
  has the server join an external AP as a station.  See
  `serverWifiBringUp()`; the RF settings there are deliberate fixes for
  audio hum and gig-crowd airtime — don't roll them back.

**There is no HMAC, no packet signing, no app-level ACK or retry, and no
`sendNoteSetReliable`.**  They were removed on purpose after a gig failure;
TCP is the reliability layer.  The simplified transport is load-bearing —
do not re-add any of it unless the owner asks.

## How commands are processed

- MagiLink callbacks run on the **link worker task**.  They must not do slow
  work (SD scans, streaming a song, LCD drawing): validate, queue or set a
  flag, and return.  `commandsTick()` on the loop task does the work.  (The
  restore, file-save and backup handlers are the existing
  exceptions that touch SD on the worker.)
- Anything that reads, replaces or saves the active song — load, save, new
  song, set-OFF, delete, a finished full-song push — is queued as a `SongOp`
  and run by `runSongOp()` on the loop task **in arrival order**.  Note-sets
  have their own queue, merged into the same order; a song patch is applied
  at once unless an operation is queued or running.
- The active song lives in `srvActiveBuf` (PSRAM); the sequencer reads it
  from the MIDI task.  Code that rewrites it wholesale calls
  `quiesceSongReaders()` first and `commitActiveSong()` after.
- A full-song push from the client is **staged** too: it streams into its
  own scratch buffer and replaces the active song only once complete, so a
  link drop mid-push leaves the current song playing.
- Lock order: link mutex first, then `SdLock`.  Never send on the link while
  holding `SdLock`.
- Loads are **staged**: the file is read into a scratch buffer and only
  copied over the active song once it has loaded.  A failed load leaves the
  current song active and the client is told with `MSG_SONG_LOAD_FAILED`.
- The server never volunteers the song stream.  The client pulls it
  (`MSG_ACTIVE_SONG_REQ`) on connect and after `MSG_ACTIVE_SONG_CHANGED`.
- All SD access goes through `SdLock` (`sd_mutex.h`) — the loop task, the
  link worker and the sample player all use the card.  The lock is not
  recursive.  Song writes go through `writeSongVerified()` (temp file, CRC
  read-back, rename).

## Files

| File | Purpose |
|---|---|
| `magitrac_server_s3.ino` | Entry point: `setup()`/`loop()`, WiFi bring-up, MIDI task, LCD screens |
| `midi_player.cpp/.h` | Sequencer + performer-sync engine, MIDI in/out |
| `commands_server.ino` | Client commands: song load/save/patch/note-set, lists, backup/restore, file server |
| `pairing.ino` | Pairing ceremony + client session state |
| `pixelpost_send.ino` | PixelPost light state broadcast over ESP-NOW |
| `field_flash.ino` | Flash PixelPost units at a gig, firmware served from the SD card |
| `SamplePlayer.cpp/.h` | WAV playback via the shared codec: one SD stream + 8 voices from the PSRAM sample cache (filled a step at a time) |
| `test_sample_cache.cpp` | Host tests for that cache (mocks in `test_mocks/cache/`); build line in the file |
| `SampleManifest.cpp/.h` | `/samples/samples.txt` — stable sample ids (a `#hi=` line stops a deleted sample's id being reused straight away); trim/loop/tune/level metadata (`trim.txt`).  The id table is rebuilt to one side and swapped in, so lookups never see it empty; `trim.txt` is read once at boot |
| `test_manifest.cpp` | Host tests for the manifest (mocks in `test_mocks/manifest/`); build line in the file |
| `drawbar_organ.cpp/.h` | Additive drawbar organ synth (own task, takes the codec while active) |
| `proc_sounds.cpp/.h` | Procedural sounds for the organ's PROC type (REED, CHOIR, VOCODER, SYNTH) |
| `synth_voice.cpp/.h` | The SYNTH PROC sound's voice: two oscillators + sub, resonant low-pass, one ADSR.  Its 14 settings live in the song's OrganPatch in the drawbars' place, and SYNTH is stored as voice 8 (7 is VOCODER's old number) — see `magitrac_lib/src/OrganSynth.h` |
| `audio_codec.cpp/.h` | Thin wrapper around the shared ES8388 (Module Audio v2.2) |
| `mic_spectrum.cpp/.h` | Module Audio mic (LEFT input) → FFT bands + beat detect → pixel_post; feeds chord + scope screens |
| `chord_detect.cpp/.h` | Chroma-based chord recognition (pure, no audio I/O) |
| `esp32s3_dsp.cpp/.h` | Third-party ESP32-S3 FFT / filter helpers |
| `sd_mutex.cpp/.h` | `SdLock` — the lock around the shared SD/SPI bus |
| `crash_log.cpp/.h` | Reset reason + breadcrumbs kept across a crash, shown on the LCD at next boot |
| `debug_log.cpp/.h` | `debugPrintf` — queued, printed by a background task |
| `Battery.cpp/.h` | Battery meter (AXP2101 via `M5.Power`) |
| `sample_record.cpp/.h` | ADD SAMPLE: armed by the client; the .ino's hold screen (`recScreenTick`) records while the glass is held (a `mic_spectrum` consumer, `micMonitor` for the level before it), writes the take to `/rec_take.wav` on release and plays it back (REVIEW: KEEP / CANCEL / RESAMPLE on the server's screen; the MIDI keys play it via `samplePlayBuffer`), finds its pitch and tunes it to middle C, and on SAVE renames it into `/samples` with that TUNE |
| `pitch_detect.cpp/.h` | Take pitch detection: averaged FFT of the loudest frames, harmonic-series candidate score + multiple check + interpolated-harmonic refinement; stepped one FFT per loop pass |
| `test_pitch.cpp` | Host tests for `pitch_detect` (synthetic tones, noise, clicks); build line in the file |
| `timestretch.cpp/.h` | KEEP LENGTH (sample editor checkbox, `keepLen` in `trim.txt`): a cached sample's other notes change pitch, not length.  WSOLA time-stretch by the pitch ratio (32 ms Hann grains, 50 % overlap, each start nudged ±8 ms to best match the grain before), read that much faster with cubic interpolation — sounds like normal pitched playback at the original length, keeps vibrato/glides, no pitch detection.  Replaced PSOLA (one period per sample → monotone, robotic).  SD-streamed samples still resample |
| `test_timestretch.cpp` | Host tests for `timestretch` (length, pitch, level, steadiness across ±2 octaves; pitch movement carried through; timbre = normal playback; attack, end, stereo, loop, matching cost); build line in the file |
| `take_fade.h` / `test_take_fade.cpp` | ADD SAMPLE's 5 ms / 25 ms fades at a take's ends (all-signed arithmetic — an unsigned fade length once scrambled every negative sample there into a click) and its host test |
| `claude_ai.cpp/.h` | AI (client MENU → AI, external-AP mode only): turns the active song into text, sends it with the typed prompt to the Claude API over HTTPS (own task on core 0, streamed SSE answer into PSRAM) and hands the reply back to `aiTick()` in `commands_server.ino`, which parses it and only then replaces the active song (unsaved; the old one kept for UNDO).  Key + optional `model=` / `effort=` in `/ai/key.txt` on the card — never sent to the client.  The system prompt that teaches the model the format lives here |
| `song_text.cpp/.h` | The song ⇄ text form the AI reads and writes (`MAGITRAC SONG 1` … `END`, one line per column / block / key / note, plus ORGAN and SYNTH lines for the organ patch).  The parser is strict about structure (END, contiguous blocks, 4000 notes) and refuses a reply with more than a few bad lines |
| `ai_certs.h` | Root CAs for api.anthropic.com (GTS Root R4/R1, GlobalSign).  TLS failing with a certificate error → re-check the chain |
| `test_synth_voice.cpp` | Host test for `synth_voice.cpp` (envelope times, pitch, filter, full-resonance stability); build line in the file |
| `test_song_text.cpp`, `test_claude_ai.cpp` | Host tests for those two (the latter against a fake network in `test_mocks/ai/`); build lines in the files |
| `mic_echo_test.cpp/.h` | Parked bench diagnostic (`MIC_ECHO_TEST`) |
| `freq_resp_test.cpp/.h` | Bench diagnostic (`FREQ_RESP_TEST`): loops stepped sines out of the codec; record and analyse with `tools/freq_resp/` |
| `test_seq.cpp`, `test_mocks/` | Sequencer host test (see above) |
| `wifi_secrets.h` | Local-only credentials, not committed (copy the `.example`) |

## Shared core: `magitrac_lib`

The shared types and transport live in the sibling **`magitrac_lib`**
Arduino library (`../magitrac_lib/src/`), pulled in via
`#include <magitrac_lib.h>` at the top of `magitrac_server_s3.ino`.  Files
provided by the lib (don't add local copies):

- `TrackerData.{h,cpp}` — Song / Pattern / NoteNode
- `NoteGrid.{h,cpp}` — sparse note-pool abstraction
- `SongMigration.h` — `.mgt` reader / writer / CRC for the current version
- `MagiMsg.h` — wire message structs + types
- `MagiLink.{h,cpp}` — the persistent TCP data link
- `MagiUdpLink.{h,cpp}` — best-effort UDP datagrams
- `MagiComms.h` + `MagiCommsEspNow.{h,cpp}` — ESP-NOW transport
- `PairNVS.{h,cpp}` — NVS storage for the pairing + WiFi credentials

Symlink `magitrac_lib/` into `~/Documents/Arduino/libraries/` so the
Arduino IDE finds it.  Edit shared files once in
`magitrac_lib/src/`; both sketches pick the change up.

## Key data layout

Same as the client (`TrackerData.h` is shared):

- `MAX_COLUMNS = 21` (col 0 = input, cols 1–20 = output).
- `Song::columns[MAX_COLUMNS]` holds per-column config (channel,
  bank, program, vol, transpose, mute, name).  `midiChannel` 1–16 is MIDI;
  `SFX_CHANNEL` / `PIXELPOST_CHANNEL` / `ORGAN_CHANNEL` route a column to
  the sample player, the lights, or the organ synth.
- Sparse `NoteNode` pool of 4000 nodes shared across patterns.
- `SONG_FILE_VERSION = 20`.  **Only the current version loads**
  (`songReadAnyVersion()` refuses anything else; the old-format readers were
  removed on purpose).  When the layout changes, bump the version and **ask
  the owner** whether a one-off conversion is wanted — don't write a
  migration reader unasked.  `songCrc32()` must stay in step with
  `songWriteCompact()`, or every save fails its verify.

## Conventions

- Prefer direct UART LL writes for MIDI — every microsecond between
  performer-note and MIDI-out matters for feel.
- `seqWriteStatus()` implements MIDI **running-status** — only emit a
  status byte when it differs from the last one sent.  Reset
  `seqOutStatus = 0` whenever you want to force a fresh status (and after
  writing raw bytes that carry their own status).
- `seqChanBank/Prog/Vol` cache: keep in sync with what was actually sent.
  Code that bypasses the cache (e.g. forced setup) must also update it.
- The idle tempo screensaver returns early from `loop()`: anything the
  client can request remotely must be handled before that point.
- No comments unless the *why* is non-obvious.

## Build

Arduino IDE targeting the M5Stack CoreS3 (ESP32-S3).  No
PlatformIO config in tree.  `.ino` files are compiled together with
the `.cpp/.h` files.  Needs the M5Unified, M5Module-Audio,
pixelpost_proto and ArduinoJson (7.x, for the AI's streamed answer)
libraries besides `magitrac_lib`.

## Working here

- The owner builds and flashes from the Arduino IDE.  Don't compile after
  an edit, and never run `arduino-cli` in the background.
- Client and server are always flashed together.  The wire format may
  change freely, but both ends must change in the same commit.
- Internal RAM is the radios' budget on the ESP32-S3: anything over ~1 KB
  (buffers, queues, big statics) goes in PSRAM.  The deliberate exception
  is the organ synth's audio-rate buffers (reverb lines, voice scratch,
  wavetables — about 69 KB), which need fast internal RAM.  Measured Nov
  2026 with the client connected: 77 KB free at idle, 59 KB at the lowest
  point of a song load and play.  The core's mbedTLS allocates internal-only
  and a TLS session wants ~45 KB — more than is spare — so `claude_ai.cpp`
  points mbedTLS at PSRAM (`mbedtls_platform_set_calloc_free`) for the length
  of an AI request; the request itself needs 32 KB internal (12 KB stack +
  sockets) and refuses to start below that.  Don't link the core's `USB.h` classes
  (USBMSC and friends): the TinyUSB stack they pull in reserves 32 KB.
- The `.ino` files are concatenated into one unit (the main one first) and
  function prototypes are auto-generated ahead of the code, before the
  types those files define.  Never use a type defined in an `.ino` in a
  function signature — pass an index or plain types instead, or move the
  type to a header.
