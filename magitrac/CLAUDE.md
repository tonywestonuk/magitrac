# MagiTrac — Client (UI device)

ProTracker-style music tracker for **live performance**, running on a LilyGo
T5 S3 e-paper device with GT911 capacitive touch.  This is the *client*: it
owns the UI and all editing.  The song itself lives on the **server**, which
also runs the sequencer and the MIDI playback (see the sibling
`magitrac_server_s3/` sketch).

## Hardware

- **LilyGo T5 S3** — ESP32-S3, 960×540 4-shade e-paper, GT911 touch.  The
  panel is mounted upside down: display and touch are both rotated 180°.
  (The sketch also carries pin maps for an M5PaperS3.)
- Display painter: `EPD_Painter_Adafruit` (custom library) — `paint()` blocks
  until the panel is driven, `paintLater()` hands the flush to a background
  task.
- Touch: `gt911_lite.h` — rising/falling edge detection in poll loops.

## Core innovation: performer-sync engine

This is what makes MagiTrac different from a normal tracker.  The engine
runs on the **server** (`midi_player.cpp`); the client only edits the data
that drives it.

- **Column 0** is the **input track** — it does not produce MIDI.  Its rows
  carry `WAIT` and `SYNC` effects and a target note (or `NOTE_ANY`).
- A live performer plays a MIDI keyboard.  When a performed note matches
  a `WAIT`/`SYNC` row in col 0, the engine **snaps** the play position to
  that row.  `WAIT` halts until the performer plays a matching note;
  `SYNC` does not halt but allows re-alignment.
- The engine **derives BPM** from the timing between snaps — the song
  literally follows the performer.
- `InputNoteEntry` per pitch-class can also drive **block switches** and
  **transposition** when the performer plays.

This is *the* defining feature.  Don't break it.

## Two-device architecture

```
[Performer keyboard] --MIDI-in--> [Server: M5Stack CoreS3] --MIDI-out--> [Synth]
                                       ^
                                       | WiFi: MagiLink (TCP) + UDP updates
                                       v
                                  [Client: LilyGo T5 S3]
                                  (this sketch — UI + editing)
```

- **The server's SD card is the source of truth.**  The client is
  "server-only": it keeps no songs or instruments of its own (its SD card
  holds only backup archives), and the Songs page shows "Not connected"
  when there is no link.
- The server sends the active song when the client asks for it: the client
  pulls it on every connect, when the server says it changed
  (`MSG_ACTIVE_SONG_CHANGED`), and as the reply to a load request.
- Edits go back as small messages — `sendSongPatch` (byte-offset patches
  into the `Song` struct) and `sendNoteSet` (one cell).  A full push
  (`sendSongToServer`) is only for structural changes, because it **stops
  the server's sequencer**.
- The client's `TrackerEngine` is only a view cursor (pattern + row); while
  the server plays it follows the position the server reports.
- Saving is the server writing its own copy (`sendSaveActive`).  After 30 s
  idle on the tracker the client asks for an autosave draft.

## Shared core: `magitrac_lib`

The files shared with the server live in **`../magitrac_lib/src/`** and are
compiled in via Arduino library discovery (the `.ino` does
`#include <magitrac_lib.h>`; symlink the directory into
`~/Documents/Arduino/libraries/`).  Currently shared:

- `TrackerData.{h,cpp}` — Song / Pattern / NoteNode + helper formatters
- `NoteGrid.{h,cpp}` — sparse note-pool abstraction
- `SongMigration.h` — `.mgt` reader / writer / CRC for the current version
- `MagiMsg.h` — wire message structs + types
- `MagiLink.{h,cpp}` — the persistent TCP data link
- `MagiUdpLink.{h,cpp}` — best-effort UDP datagrams
- `MagiComms.h` + `MagiCommsEspNow.{h,cpp}` — ESP-NOW, pairing ceremony only
- `PairNVS.{h,cpp}` — NVS storage for the pairing + WiFi credentials

When you need to change a shared struct or wire format, edit it once in
`magitrac_lib/src/` and both sketches pick it up.

## Key data layout (`TrackerData.h`)

| Constant | Value | Notes |
|---|---|---|
| `MAX_PATTERNS` | 50 | aka "blocks" |
| `MAX_ROWS` | 64 | per pattern |
| `MAX_COLUMNS` | 21 | col 0 = input, cols 1–20 = output |
| `MAX_SONG_NOTES` | 4000 | sparse pool shared across patterns |
| `MAX_INSTRUMENTS` | 256 | library presets |
| `INSTRUMENT_NAME_LEN` | 12 | 11 chars + null |
| `SONG_FILE_VERSION` | 20 | bump when `Pattern`/`Song` tail/`MAX_COLUMNS` changes |

- `NoteNode` is the sparse note record: 8 bytes in memory (row, col, note,
  velocity, effect, param + a 16-bit `next` index).  Each pattern is a
  **circular linked list** through `Song::notePool`; free nodes form a
  singly-linked free list.  The `NoteGrid` class is the abstraction for
  read/write access.
- `ColumnSettings` is **per-song** (shared by all patterns), 20 bytes,
  in `Song::columns[MAX_COLUMNS]`.  `midiChannel` 1–16 is MIDI; the special
  values `SFX_CHANNEL` / `PIXELPOST_CHANNEL` / `ORGAN_CHANNEL` route the
  column to the server's sample player, the lights, or its organ synth.
- Block-end navigation is packed into `Pattern::blockEndNav` as `xxyyyyyy`
  — see `NAV_*` macros.

## File format

`.mgt` files use the compact serialised layout in `TrackerData.h`:

```
[ SongFileHeader ]              (8 bytes)
[ Pattern × MAX_PATTERNS ]      (noteHead ignored on load)
[ ColumnSettings × MAX_COLUMNS ]
[ Song tail ]                   (numPatterns ... flags)
[ uint16_t noteCount ]
[ SerializedNote × noteCount ]  (7 bytes each)
```

Wire transfer (the MagiLink song streams) uses the **raw `Song` struct**
behind a `SongFileHeader` — file format and wire format diverge only in
note storage.

**Only the current version loads.**  `songReadAnyVersion()` in
`SongMigration.h` accepts `SONG_FILE_VERSION` and refuses everything else;
the readers for older versions were removed on purpose (single user, every
song already converted).  When the layout changes, bump the version and
**ask the owner** whether a one-off conversion is wanted — don't write a
migration reader unasked.  Old files are converted offline on the Mac with
`tools/mgt_upgrade.py` (scans a folder, rewrites v14–v19 songs as v20, keeps
backups); extend that tool when the version is bumped.  `songCrc32()` must walk the same bytes in the
same order as `songWriteCompact()`, or every save fails its verify.

## UI pages

The tracker grid is the home screen; everything else is a full-screen page
or popup.  Most are a class with `open()` / `draw()` / `poll()`, owned and
dispatched by `magitrac.ino` through one "open" flag per page (only the
tracker and Block Settings go through `PageManager`).

- **TrackerUI + TouchHandler** — the main grid.  `TrackerUI` draws header,
  column headers, a 10-row × 3-column window of the block, and the status
  bar; `TouchHandler` turns touches into `TouchAction`s (cell tap, drag
  scroll, header tap/hold, play/stop, …).
- **NoteEditor** — full-screen editor for one cell: note, octave, velocity,
  effect/param (hex pad or effect list), COPY/PASTE; col 0 gets
  WAIT/SYNC/PASS/AVRG instead.  Opened by a cell tap while stopped.
- **ColumnEditor** — per-column settings (channel, bank, program, volume,
  transpose, name, pick-instrument; SFX columns get TUNE and a sample
  picker) plus CLEAR / COPY / PASTE / IMPORT DRUMS, with a confirm on CLEAR
  and PASTE.  Opened by **tap** on a column header (cols 1–20).
- **ColumnNoteEditorPage** — piano-roll-style note editor for one column
  of one pattern: 13 visible pitches × 16 row-cells per segment, with
  drag-scroll on the pitch axis, segment nav `[<] [>]`, PREVIEW, VEL/ATTR
  popups, selector-strip multi-select, COPY/PASTE.  Opened by **500 ms
  long-press** on any column header (incl. col 0).  Col 0 swaps the OFF
  action for ANY/WAIT/SYNC and hides ATTR (param=0 always).
- **BlockSettingsPage** — blocks: new / duplicate / split / delete, length,
  the per-pitch-class input-note actions (block switch, transpose),
  key-change mode, block-end navigation.  Opened by tapping the block label.
- **DrumEditorPage** — step grid over a block's channel-10 columns, reached
  from Block Settings.  **DrumTrackImportPage** imports a block from the
  server's `/drumtracks/*.txt` (from here or from ColumnEditor).
- **SongConfigPage** — song-level config: initial / min / max BPM, MIDI-in
  channel and note range, performer slot enables (`performerMask`), which
  MIDI channels follow performer transpose, MIDI-clock out.
- **SongPage** — the server's song list: load, NEW, SAVE, SAVE AS, swipe a
  row left to delete.
- **PerformancePage** — 8 pads for live block switching (immediate or
  queued), a light-control strip, and the way into **SetlistPage** (master
  song catalog + 4 setlists, stored on the server).
- **InstrumentsPage** (the instrument bank), **OrganPage** (drawbar organ
  on the server), **SampleEditPage** (sample trim/loop),
  **PixelPostManualPage** / **PixelPostSettingsPage** (lights),
  **SettingsPage** / **WiFiSettingsPage** (clock, MIDI limits, WiFi,
  pairing), **BackupRestorePage**, and **BootMenu** (the MENU popup that
  leads to most of these).

`UIHelpers.h` provides `uiButton()`.  `HoldRepeat` powers ±value
buttons.  `ConfirmDialog`, `KeyboardPopup`, `NumpadPopup` and
`ScrollViewport` (inertia drag-scroll lists) are the other shared pieces.
`HexpadPopup` is a full-screen hex byte editor —
`open(hi, lo, numDigits, title, fingerDown=true, showChoose=false)`;
`numDigits=2` for a
single-byte editor (returned via `effect()`), `numDigits=4` for
effect+param.  Pass `fingerDown=false` when opening on a falling edge
(finger already lifted) so the first button tap inside the popup isn't
swallowed.

## EPD colour rules (memorised)

- **Never** dark grey on light grey — unreadable.
- **Disabled** = white background + dark grey text (no inversion).
- 4 shades total: white, ltgrey, dkgrey, black.

## Communications

`ServerPairing` (in `ServerPairing.cpp/.h`) owns the pairing ceremony, the
session state, every outgoing message and the handlers for incoming ones.

- **MagiLink** (`gMagiLink`) — one persistent TCP connection, the data
  path.  The server accepts on `MAGI_PORT`, the client connects.  Messages
  are raw structs from `MagiMsg.h`, each starting `{uint8 id, uint16
  length}` with no other framing.  Incoming messages are dispatched to
  callbacks registered per message id, which run on the link's worker task.
  The callback table is fixed-size and a registration past the end fails
  with only a log line — check the count when adding a handler.
- **MagiUdpLink** — loss-tolerant updates from the server on the same port
  number: sequencer position, preview row, MIDI note-in, and the server's
  announce beacon.  No ACK, no retry — the next update supersedes a lost
  one.
- **ESP-NOW** (`MagiCommsEspNow`) — the pairing ceremony **only**
  (Settings → PAIR SERVER; a PIN to confirm, then WiFi credentials are
  exchanged and stored in NVS by `PairNVS`).
- **Network** — normally the server hosts its own hidden softAP at a fixed
  address and the client joins it as a station with a fixed address
  (`MAGI_SERVER_IP_*` / `MAGI_CLIENT_IP_*`).  The alternative mode has both
  join an external AP by DHCP, the client finding the server from its
  announce beacon.

**There is no HMAC, no packet signing, no app-level ACK or retry, and no
`sendNoteSetReliable`.**  They were removed on purpose after a gig failure;
TCP is the reliability layer.  The simplified transport is load-bearing —
do not re-add any of it unless the owner asks.

## Build

Arduino IDE targeting ESP32-S3.  No PlatformIO config in tree.
Headers live alongside their `.cpp` files.  Files in this directory are
compiled together — `.ino` is the entry point.

## Working here

- The owner builds and flashes from the Arduino IDE.  Don't compile after
  an edit, and never run `arduino-cli` in the background.
- Client and server are always flashed together.  The wire format may
  change freely, but both ends must change in the same commit.
- Internal RAM is the radios' budget on the ESP32-S3: anything over ~1 KB
  (buffers, caches, big statics) goes in PSRAM.

## Conventions

- No comments unless the *why* is non-obvious.
- E-paper redraws are expensive — use `paintLater()` and minimise dirty
  rects. paint() and paintLater() the full screen, with paintLater() doing this in the background.
- Changing pages: `display.clear()` (a hard anti-ghost clear of the panel —
  it does not wipe the framebuffer), draw the new page, then paint.
  `magitrac.ino` opens most pages with `open()`, `clear()`, `draw()`,
  `paint()`, and returns to the tracker with `clear()`, `ui.drawAll()`,
  `paintLater()`.
- All position/touch handlers do rising/falling edge detection — never
  trigger on level.
- **Long-press gestures** (e.g. column-header hold → ColumnNoteEditorPage)
  fire mid-press from `TouchHandler::poll()` *before* the early-return on
  no-new-event, since `_touch.read()` returns false on idle frames where
  the finger is still down.  The fire path also resets `_state` to `IDLE`
  and `_wasDown` to `false` because the page that opens does its own
  touch polling — without this, `TouchHandler` keeps stale state and
  swallows the next genuine tap after the page closes.
- **Drag-scroll inertia** on the tracker grid always **clamps**
  at the pattern edges — no wrap-around, regardless of play state.
  Tuning: `INERTIA_DECAY` (s⁻¹) controls deceleration rate; bump for
  more "resistance".
