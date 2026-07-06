# Tracker Guide (the client)

The client is the LilyGo T5 e-paper touch device — it owns the song and all
editing, while the paired [server](server-guide.md) does the playing. Edits
stream to the server continuously as you make them, so what you hear is
always what you see. The physical BOOT button toggles the frontlight.

For what the cells and cues *mean* musically, see the
[Reference](reference.md). This page is about where everything lives.

## The main tracker screen

Top to bottom:

- **Header** — "MENU" button, clock, connection status
  ("Connecting…"/"Connected"), battery.
- **Column headers** — three columns visible at a time. Column 0 is
  labelled **MIDI IN**; output columns show their name (or "COLUMN n").
  Each output column has a small speaker icon — tap it to **mute/unmute**
  (a strike-through means muted).
- **The grid** — 10 rows × 3 columns, row numbers on the left. The
  play/cursor row sits on a fixed shaded "reader" line. Empty cells read
  `--- -- -----` (note / velocity / effect).
- **Status bar** — "PLAY"/"STOP" (shown when connected), the song name,
  the edit toggles, and block navigation `< nn/nn >`.

### Gestures

| Gesture | Action |
|---|---|
| Tap a cell (while stopped) | open the **Note Editor** for that cell |
| Touch the grid (while playing) | **pause** for as long as your finger is down — release to resume |
| Drag vertically | scroll/scrub the row cursor (inertia, clamps at pattern ends); while stopped this seeks the server |
| Drag horizontally | scroll through the 21 columns |
| Tap a column header | open the **Column Editor** (cols 1–20) |
| Long-press a column header (½ s) | open the **piano-roll editor** (any column, including MIDI IN) |
| Tap `<` / `>` | previous / next block |
| Tap the `nn/nn` block indicator | open **Block Settings** |
| Tap the song name | open the **song browser** (load/save) |

### Step entry from a MIDI keyboard

While stopped, the three small status-bar toggles drive step input from the
keyboard plugged into the server:

- **`e`** — edit mode on/off. With it on, playing a key writes that note
  into the selected column at the cursor row.
- **`+N`** — step advance after each entry (cycles +1 → +2 → +3 → +4).
- **`v`** — velocity capture (record how hard you hit the key).

Two bottom-of-the-keyboard specials: **A-0 deletes** the cell,
**A♯0 writes a NOTE-OFF**.

### Saving

Edits stream to the server live; separately, 30 seconds after your last
edit a small "Saving…" appears bottom-left and the server writes the song
to its SD card. Explicit SAVE / SAVE AS lives in the song browser.

## The menu

Tap "MENU" for the main menu: **SONG** (song settings), **INSTRUMENTS**,
**SETTINGS**, **BACKUP**, **PERFORM**, **POSTS** (manual light control),
**PAIR**, and **ORGAN**. Tap outside the box to dismiss.

## Song browser (tap the song name)

Lists the songs on the server (bold = currently loaded); drag to scroll.

- **Tap a song** to load it — if you have unsaved changes you're asked
  "SAVE CHANGES TO &lt;name&gt;?" first.
- **NEW** — name a new blank song (duplicates rejected).
- **SAVE** / **SAVE AS** — write to the server under the current / a new
  name.
- **DEL** — toggles delete mode; tapping a song then asks
  "DELETE SERVER FILE?".

Unpaired, the page just says "Not connected — pair to a server to load
songs."

## Note Editor (tap a cell)

Full-screen editor for one cell, with `<` / `>` stepping through rows
(each step commits the edit).

- **NOTE** — twelve note buttons + **OCTAVE** 0–7. Tap a selected note
  again to clear it.
- Output columns — **CLR**, **OFF**, velocity −/+, and an attributes
  button showing the effect hex (opens a hex pad, `EE PP`).
- MIDI IN column — **CLR**, **WAIT** (tap again for WAT1/WAT2), **SYNC**,
  **PASS** (tap again for PASA), **AVRG**.
- **COPY** / **PASTE** move cells around; PXL-column cells get an extra
  **PXL** button that opens the live light-effect picker.

## Piano-roll editor (long-press a column header)

Edits one column of one block as a 13-pitch × 16-step grid, in segments
(`< >` to move between them, `pp.s / nn` shows where you are).

- **Tap a grid cell** — place a note (tap same pitch again to clear;
  different pitch moves it).
- **Drag vertically** — scroll the pitch axis.
- **Drag the selector strip** (above the grid) — multi-select steps, then
  **COPY** / **PASTE**.
- **PREVIEW** — loops just this column at song tempo while you edit.
- **VEL** and **ATTR** set velocity/effect for the selection; on the
  MIDI IN column the action buttons become **ANY / WAIT / SYNC**.

## Column Editor (tap a column header)

Per-column MIDI settings, shared by every block: **MIDI CH** (this is also
where a column becomes `SFX` = sample player or `PXL` = lights — see the
[Reference](reference.md#column-kinds)), **BANK**, **PROGRAM** (a drum-kit
picker on channel 10, a sample picker on SFX), **VOLUME**, **TRANSPOSE**,
and the column **NAME** with a **PICK INSTR** / **PICK SAMPLE** shortcut.

Bottom actions: **CLEAR** (settings + notes, confirmed), **COPY TO…** /
**SWAP WITH…** (whole-column, across all blocks, confirmed), and
**IMPORT DRUMS** (see below).

## Block Settings (tap `nn/nn`)

Everything about the current block:

- **`< nn/nn >`** navigation plus **NEW**, **DUPL** (duplicate), **SPLT**
  (halve a 32/48/64-row block into two), **DEL** — the destructive ones
  ask first. Deleting a block automatically renumbers every link that
  pointed past it.
- **LEN** — block length (16/24/32/48/64 rows).
- **INPUT NOTES** — the per-key performer actions: tap one of the 12 keys,
  then set its **BLOCK** action (STAY / SAME POS / TOP + target block) and
  **TRANSP** action (KEEP / NOTE / CUSTOM ± value). Keys with settings are
  shaded with a `>02` / `^02` tag. Hold KEEP or NOTE for a second to apply
  it to all 12 keys at once.
- **KEY-CHG** — what a key *change* does positionally (SAME POS / TOP),
  plus the **Drum-Editor** button.
- **NEXT** — block-end navigation: **LOOP / FWD / BACK / ABS / RNT** with
  a target value. (Semantics in the [Reference](reference.md#block-end-navigation).)

## Drum editor & drum-track import

**Drum Editor** (from Block Settings) — an X-grid drum machine view: one
lane per drum, tap cells to toggle hits, **+ DRUM** adds a lane from a GM
drum picker (with audition), **PLAY** loops the pattern through the server
synth while you work.

**Import** (from the Drum Editor or Column Editor) — reads
drum-patterns.com-style text files from the `/drumtracks/` folder on the
**server's SD card** (put them there via the server's USB mode). Pick a
file, pick which pattern block from it, choose a kit, audition, and IMPORT
— it fills consecutive columns with the drum lanes. An optional
`gm_map.txt` on the card customises the two-letter-code → GM-note mapping.

## Performance page (MENU → PERFORM)

The gig screen. Eight big pads (one per block, named if you've named them),
tagged **IMM** or **QUE**:

- **Tap a pad** — start playback there, or switch blocks: an IMM pad
  switches immediately, a QUE pad flashes and switches at the block
  boundary (tap again to cancel the queue).
- **Hold a pad ~1 s** — STOP.
- The header buttons (**SETLIST / EDIT / HOME**) need a **½-second hold**
  — quick taps are ignored so a mid-song fumble can't throw you off the
  page. EDIT opens pad setup (names + IMMEDIATE/QUEUED per pad).

The **light strip** along the bottom controls the PixelPost rig live:
**&lt; FX / FX &gt;** step the light effect (grabbing control from the
song's PXL track), **WHITE** is a momentary full-white while held, and
**RELEASE** hands control back to the track.

## Setlists (PERFORM → hold SETLIST)

Four setlist slots (**SET 1–4**), each an ordered list of names drawn from
a **master catalog** (name + song file + performance notes per entry).

- **ADD** opens the master list — tap entries to add/remove them from the
  set; **NEW**/**EDIT** maintain the catalog itself (name, server file via
  a picker, up to 4 lines of notes).
- Tap an entry in the set for the **INFO** popup: the notes in large
  print, plus **OK - LOAD** (loads the song and returns to the pads),
  **REMOVE**, **CANCEL**. Entries can be title-only — a marker in the set
  with no file behind it.
- Each row's **Move** button re-orders: tap Move, then tap the row to drop
  it after.

## Instruments (MENU → INSTRUMENTS)

A server-owned bank of up to 256 named presets (bank/program/volume/
transpose) that the Column Editor's PICK INSTR draws from. Edit here; on
exit you're asked "SAVE INSTRUMENTS?" — YES pushes the bank to the server.

## Organ (MENU → ORGAN)

Live control of the server's drawbar organ: drag the nine drawbars, tap the
voice type to cycle (DRAWBAR / TONEWHEEL / CLAUDE / NEBULA / SAMPLE),
toggle **VIB / LESLIE / DRIVE**, or grab a preset (FULL, JAZZ, GOSPEL,
FLUTE, ROCK). Play your MIDI keyboard to sound it; every tweak streams
live. HOME exits organ mode on both devices.

## Lights, manually (MENU → POSTS)

Direct control of the PixelPost rig outside any song: a paged grid of
effect buttons, a brightness slider, and an X-Y touchpad, all live.
**PWR OFF** (confirmed) blacks out every post; **FW** triggers a light-node
firmware update; **SET** opens rig settings (max brightness, flash
smoothing). See [Lights](lights.md).

## Backup & restore (MENU → BACKUP)

**BACKUP** copies every file off the server into a dated folder under
`/backups` on the client's SD card, with a progress bar. **RESTORE** lists
those folders and puts files back — one at a time or **RESTORE ALL**
(confirmed, since it overwrites the server's copies).

## Settings (MENU → SETTINGS)

Clock and date (feeds the header clock), MIDI limits (MAX BANK /
MAX PROGRAM for the value editors), and **WIFI SETTINGS** — where you pick
**Server AP** mode (the default: the server hosts its own network; the
credentials arrive automatically when you pair) or **External AP** mode
(join an existing network: editable SSID/password with a scanner). The
channel buttons (1/6/11) choose the WiFi channel in Server-AP mode.

## Pairing (MENU → PAIR)

See [Getting Started](getting-started.md#first-pairing) — in short: put
the server in pair mode (hold its bezel 2 s), tap PAIR here, check the
4-character code matches on both screens, tap **CONFIRM** on the client.
The server reboots and the client connects automatically from then on.
