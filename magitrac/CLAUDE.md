# MagiTrac — Client (UI device)

ProTracker-style music tracker for **live performance**, running on a LilyGo
T5 S3 e-paper device with GT911 capacitive touch.  This is the *client*: it
owns the UI, the song data, and all editing.  The actual MIDI playback runs
on a separate **server** device (see the sibling `magitrac_server/` repo).

## Hardware

- **LilyGo T5 S3** — ESP32-S3, 960×540 4-shade e-paper, GT911 touch.
- Display painter: `EPD_Painter_Adafruit` (custom, with `paintLater()` deferred
  flush — full repaint ~13 ms after a `fillRect` override, ~17 ms for the grid).
- Touch: `gt911_lite.h` — rising/falling edge detection in poll loops.

## Core innovation: performer-sync engine

This is what makes MagiTrac different from a normal tracker.

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
[Performer keyboard] --MIDI-in--> [Server: M5Stack] --MIDI-out--> [Synth]
                                       ^
                                       | ESP-NOW (encrypted, paired)
                                       v
                                  [Client: LilyGo T5 S3]
                                  (this repo — UI + song)
```

- **Client** owns the source-of-truth `Song`.
- **Server** receives a copy on pair-connect and is patched incrementally
  thereafter (`sendSongPatch` byte-offset patches; `sendNoteSet` per-cell).
- ESP-NOW transport is being abstracted behind `MagiComms` /
  `MagiCommsTransport` — see `MagiComms.h`.  Both client and server share
  the same `MagiComms.h` / `MagiCommsEspNow.h` / `MagiCommsEspNow.cpp` files.
  A future UART transport will swap in via `MagiCommsTransport` subclass.

## Key data layout (`TrackerData.h`)

| Constant | Value | Notes |
|---|---|---|
| `MAX_PATTERNS` | 50 | aka "blocks" |
| `MAX_ROWS` | 64 | per pattern |
| `MAX_COLUMNS` | 9 | col 0 = input, cols 1–8 = MIDI output |
| `MAX_SONG_NOTES` | 4000 | sparse pool shared across patterns |
| `MAX_INSTRUMENTS` | 256 | library presets |
| `INSTRUMENT_NAME_LEN` | 12 | 11 chars + null |
| `SONG_FILE_VERSION` | 17 | bump when `Pattern`/`Song` tail/`MAX_COLUMNS` changes |

- `NoteNode` is the sparse note record (7 bytes wire, 8 bytes struct +
  `next` index).  Each pattern is a **circular linked list** through
  `Song::notePool`; free nodes form a singly-linked free list.  The
  `NoteGrid` class is the abstraction for read/write access.
- `ColumnSettings` is **per-song** (shared by all patterns), 20 bytes,
  in `Song::columns[MAX_COLUMNS]`.
- Block-end navigation is packed into `Pattern::blockEndNav` as `xxyyyyyy`
  — see `NAV_*` macros.

## File format

`.mgt` files use the compact serialised layout in `TrackerData.h`:

```
[ SongFileHeader ]              (8 bytes)
[ Pattern × MAX_PATTERNS ]      (noteHead ignored on load)
[ ColumnSettings × MAX_COLUMNS ]
[ Song tail ]                   (numPatterns ... _songPad)
[ uint16_t noteCount ]
[ SerializedNote × noteCount ]  (7 bytes each)
```

Wire transfer (ESP-NOW) still uses the **raw `Song` struct** — file format
and wire format diverge only in note storage.

`SongMigration.h` carries a chain of versioned readers (v11→v13→v14→v15→
v16→v17).  When changing layout, add a new `songMigrateVNFromFile()`
function and a dispatch line in `SongStorage.cpp` — never break old files.
Note `V16_MAX_COLUMNS = 8` constant — historical structs lock the column
count at the value it had when they were written.

## UI pages (each is a class with `open()` / `draw()` / `poll()`)

- **TrackerPage** — main grid: row × col cells with note/effect editing,
  inline keyboard via `KeyboardPopup`.
- **ColumnEditor** — per-column MIDI settings (channel, bank, program, vol,
  transpose, name); also COPY / SWAP / CLEAR column actions with confirm
  dialogs.
- **SongConfigPage** — song-level config (BPM, speed, MIDI-in channel,
  performer mask, slot enables).  *Mid-rewrite — `.h` updated, `.cpp`
  pending unification of row constants.*
- **PatternListPage**, **InstrumentListPage**, **InputNoteEditor**,
  **PerfModePage**, etc.

`UIHelpers.h` provides `uiButton()`, etc.  `HoldRepeat` powers ±value
buttons.

## EPD colour rules (memorised)

- **Never** dark grey on light grey — unreadable.
- **Disabled** = white background + dark grey text (no inversion).
- 4 shades total: white, ltgrey, dkgrey, black.

## Communications

`ServerPairing` (in `ServerPairing.cpp/.h`) owns the pairing state machine
and the message handlers.  HMAC-based auto-reconnect via `PairNVS`.
A `MagiComms` abstraction is being introduced — see `joyful-sleeping-candy`
plan in `~/.claude/plans/`.  ESP-NOW peer/MAC handling has Arduino-3.x
quirks (use `ESP32_NOW.h` peer classes server-side, raw `esp_now.h` on
client; broadcast required for initial contact; recv callback signature
needs the `_recv_info_t*` form).

## Build

Arduino IDE / arduino-cli targeting ESP32-S3.  No PlatformIO config in tree.
Headers live alongside their `.cpp` files.  Files in this directory are
compiled together — `.ino` is the entry point.

## Conventions

- No comments unless the *why* is non-obvious.
- E-paper redraws are expensive — use `paintLater()` and minimise dirty
  rects.  Don't full-repaint when a region update will do.
- All position/touch handlers do rising/falling edge detection — never
  trigger on level.
