# Reference — Notes, Effects & Navigation

The precise rules the sequencer follows. For the page-by-page editing UI see
the [Tracker Guide](tracker-guide.md); for the big picture start with
[Getting Started](getting-started.md).

## The grid model

A song is up to **50 blocks** (patterns). Each block has up to **64 rows**
(active length: 16/24/32/48/64) and **21 columns**: column 0 is the
**input track** (the performer's cue lane — it never produces MIDI), and
columns 1–20 are **output columns**. Notes are stored sparsely — a song can
hold **4000 note cells** in total across all blocks.

Every cell has four parts:

| Part | Values | Display |
|---|---|---|
| Note | `C-0` … `B-7` (96 pitches), `OFF`, or `ANY` (col 0 only) | `---` when empty |
| Velocity | 0–127, or default (plays at 100) | `--` when default |
| Effect + param | two hex bytes | `----` when empty |

Row duration is `speed × 2500 / BPM` milliseconds (speed is the classic
tracker "ticks per row", fixed at 6), so at 120 BPM a row is 125 ms — 64
rows ≈ 4 bars of 16ths.

## Column kinds

What a column *is* depends entirely on its **MIDI CH** setting in the
column editor:

| MIDI CH | Kind | Behaviour |
|---|---|---|
| `OFF` (0) | muted | produces nothing |
| 1–16 | **MIDI out** | notes go out the MIDI DIN on that channel. Channel 10 is GM drums — the PROGRAM field becomes a drum-kit picker |
| `SFX` (17) | **sample player** | notes trigger WAV samples on the server's speaker. PROGRAM = sample ID from `/samples/samples.txt`. The sample plays at native pitch on **C-4** and is repitched by semitones either side. `OFF` stops it |
| `PXL` (18) | **PixelPost lights** | notes drive the light rig — see [Lights](lights.md). Note picks the effect (C-0 = effect 0, C#0 = 1, …), velocity is the brightness/intensity slider, and the effect+param hex bytes act as X/Y pad coordinates. `OFF` = "lift finger" |

Other per-column settings (shared by every block in the song): bank,
program, volume, transpose (−24..+24), mute, and an 11-character name.

## Column 0 — the input track

This is MagiTrac's defining feature. Column 0 rows are **cues** that the
performer's live playing interacts with. A cue has a target pitch (a
specific note, or `ANY` for any pitch class — octaves never matter) and a
type:

| Cue | What it does |
|---|---|
| **WAIT** | When the playhead reaches this row it **halts**. The moment the performer plays the matching note, the row fires instantly and playback continues. If no note comes within **500 ms**, the block restarts from row 0 (or jumps to a queued block). `WAT1` and `WAT2` are the same with 1 s and 2 s timeouts |
| **SYNC** | Gentle re-alignment, never halts. If the performer plays the matching note within ±1 row of a SYNC, the engine snaps to it (or absorbs a slightly-late hit without drifting the tempo). Outside that window it's ignored. SYNC never changes the BPM |
| **PASS** (a plain note with no cue type) | "Expected noise": absorbs **one** matching performed note so it doesn't falsely trigger the next WAIT. A passing run through a WAIT note won't derail the song |
| **PASA** | Pass-All — absorbs **every** matching note until the playhead moves past the row |
| **AVRG** | When the playhead reaches this row, the tempo is set to the average of the last 4 performer-derived BPMs — lets a new block start at the tempo you were actually playing |

### How the tempo follows the performer

Every WAIT snap timestamps the performance. From the time and row-distance
between two snaps the engine derives the BPM — **the song follows you**,
not the other way round. The derived tempo is clamped between the song's
**min BPM** and **max BPM** (fumble protection: a missed cue can't send the
song to 400 BPM). Notes softer than velocity 20 are ignored as brushes.

### Per-key actions (block switch & transpose)

Each block also has a per-pitch-class table (set in Block Settings): *when
the performer plays a C (any octave), do X*. Two independent actions per
key:

- **Block switch** — `STAY` (nothing), `>N` (jump to block N keeping the
  current row), or `^N` (jump to block N from the top).
- **Transpose** — `KEEP` (unchanged), `NOTE` (the played key becomes the
  new root — hit an E♭ and the whole song transposes to E♭), or `CUSTOM`
  (a fixed ± semitone offset).

Transpose applies to all output channels enabled in the song's
transpose mask (by default: everything except channel 10 drums). Each
block declares a **reference note** — the key that means "no
transposition".

A block-level **key-change mode** additionally decides what happens
positionally when the performer's pitch class changes: `SAME` keeps the
row; `TOP` follows the block's end-navigation and restarts from row 0.

## Effects (output columns)

Effects are entered as two hex bytes (effect, then param):

| Hex | Name | What it does |
|---|---|---|
| `1B` `xx` | **Retrigger** (ratchet) | replays the note evenly within its own row; param = total hits (0/1 mean 2; clamped 2–8). `1B04` = four hits per row. The next note or OFF cancels a roll in flight. MIDI columns only |
| `A0` `xx` | **Set tempo** | param = BPM (clamped to the song's min/max). Holds until the next WAIT snap re-derives the tempo. MIDI columns only |
| `FF` `FF` | **STOP** | halts the sequencer and sends note-offs on every active column — a hard end-of-song |

Anything else is inert on output columns. (On PXL columns the same two
bytes are X/Y pad coordinates, not effects; SFX columns ignore them.)

## Block-end navigation

Each block declares what happens when the playhead runs off its last row
(set in Block Settings):

| Mode | Meaning |
|---|---|
| **LOOP** | play this block again |
| **FWD n** | jump ahead n blocks (wraps to block 1 past the end) |
| **BACK n** | jump back n blocks (wraps to the last block below 1) |
| **ABS n** | jump to block n |
| **RNT** | *return* — go back to whichever block last handed control here, subroutine-style. Great for a shared chorus that returns to wherever it was called from. With no caller recorded it behaves like LOOP |

A **queued block** (from the Performance page pads) overrides the block's
own navigation once, then clears. If you queue while the song is still
WAITing on row 0, the switch happens immediately instead.

## Patch changes — the OFF trick

The sequencer **never** sends bank/program changes together with a note-on
(a WAIT can fire with zero lead time, and a synth mid-patch-load would
glitch). Patches flush at three moments:

1. **Block start / resume / block change / WAIT timeout** — every column
   whose bank/program/volume changed gets flushed.
2. **A NOTE_OFF cell** — placing an `OFF` on a MIDI column silences it
   *and* flushes that column's pending patch. **This is how you change
   sounds mid-block**: set the new program in the column editor won't take
   effect until you give it an OFF with a few rows of breathing room before
   the next note.
3. Note-ons themselves carry **only the note** — never patch data.

## Song settings

| Setting | Default | Notes |
|---|---|---|
| BPM | 120 | starting tempo; live tempo then follows the performer |
| Min / Max BPM | 60 / 240 | clamp on the derived tempo |
| MIDI-in channel | ANY | which channel the performer's keyboard is heard on |
| MIDI-in note range | full | keys outside the window are ignored while playing |
| Performer mask | off | channels 1–4 can be marked "performer-owned" (broadcast to the synth rig as CC115 on ch 16) |
| Transpose mask | all but ch 10 | which channels follow live transposition |
| Performance pads | — | 8 named pads, each IMMEDIATE or QUEUED |

## Limits at a glance

50 blocks · 64 rows · 20 output columns + 1 input column · 4000 notes per
song · pitch range C-0..B-7 · velocities 0–127 · retrigger 2–8 hits ·
sample IDs 1–127 · song files are `.mgt` (format version 19, older
versions load and migrate automatically).
