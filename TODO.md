# To do

Things agreed but not started.

## Next — in this order

Agreed on 15 Nov 2026 for the next session. Details for each are further down.

1. **Commit the work from 15 Nov.** Nothing from that session is in git.
2. **Fix the gap when the sample list is rebuilt** — a song sample can fail to
   sound for a moment.
3. **Back up the samples folder.**
4. **Soak test** — play a whole set on the new server build while logging.
5. **ADD SAMPLE** — record a sample through the mic.

Not in that order yet (below): per-sample transpose; per-sample velocity
sensitivity.

---

## 1. Commit the work from 15 Nov

Everything from that session is uncommitted in one working tree, on `main`:
about fifty changed or new files, and `usb_msc.ino` deleted. Branch first,
then commit — split by feature rather than as one lump:

- the samples page, the sample editor's tune and keys, per-sample tune;
- the SD card at 25 MHz and the waveform scan;
- internal RAM: USB-drive mode removed, tables moved to PSRAM;
- the performance page's organ and sample buttons, organ over a running song;
- the vocoder (now a PROC sound) and the hold-to-record mic test;
- the sample cache (polyphonic squares, eight voices, samples on the keys,
  velocity) and its host test;
- the paper's experiment script, guides, and this file.

## 2. Fix the gap when the sample list is rebuilt

When the client asks for the sample list, the server rebuilds its table of
sample numbers and names from the card (`sampleManifestSync`, called from
`sendSampleList`). The rebuild empties the table and fills it again, and it
runs once for every page of the list the client asks for. While it is empty,
a sample that has to play from the SD card cannot be found by name, so a song
sample triggered at that instant is silent. Samples already in memory are not
affected.

It was hard to hit while the list was only asked for from a stopped song.
The samples page now opens mid-song from the performance page, which asks
for the list.

Ways to fix it, to choose between: build the new table to one side and swap
it in; rescan the card once per request rather than once per page; or leave
the table alone while a song is playing.

## 3. Back up the samples folder

**Asked:** 15 Nov 2026.

The client's BACKUP covers songs, instruments, drum tracks and setlists, but
nothing in the server's `/samples/` folder. That leaves two kinds of thing
with no copy anywhere but the server's SD card:

- the sample WAVs themselves;
- the two small text files beside them: `samples.txt` (which number each
  sample has — songs refer to samples by that number) and `trim.txt` (every
  sample's start, end, loop and tune from the sample editor).

Since the USB-drive mode was removed, the only other way to copy them is to
take the card out.

**The idea:** samples are large, so when BACKUP is chosen, ask whether to
back up the samples as well.

**To settle when this is picked up:**

- Whether `samples.txt` and `trim.txt` should always be included, whatever
  the answer to the prompt. They are a few hundred bytes, and without them a
  restored set of songs points at the wrong samples and loses its trims and
  tunings. (Suggestion, not yet agreed.)
- Where the backup is kept and how much room it has: the 12 samples on the
  card today come to about 14 MB.
- How long it takes over the link. Songs are about 36 KB each; a 4.8 MB
  sample is over a hundred times that.
- Restore: putting samples back must keep each one's number in `samples.txt`.

## 4. Soak test before a gig

Several changes on 15 Nov alter what the server does while a song plays:
samples loading in the background, the SD card at 25 MHz, the organ
switching in over a running song, keys playing a sample over a song. Each
passed its own check; none has run through a whole set.

Play a full set with the server on USB while the log is captured, watching:

- internal RAM — the lowest it gets (it was 59 KB after one song; the RAM
  print used on 15 Nov is temporary and has to be put back in for this);
- any SD read errors or failed loads at the faster card speed;
- samples arriving in memory, and anything that fails to;
- backup and restore, which read the tables that moved to PSRAM and have not
  been run since.

## 5. ADD SAMPLE on the samples screen — record through the mic

**Asked:** 15 Nov 2026.

Add an "ADD SAMPLE" control to the samples screen that records a new sample
through the server's microphone.

Some of this exists already. The hold-to-record mic test
(`mic_hold_test.cpp`) records the mic into PSRAM and plays it back, and that
is what proved the mic path is clean; it is a separate boot mode, though, and
it never writes a file. The sample editor can then trim, loop and tune
whatever gets recorded.

**To settle when this is picked up:**

- How recording starts and stops from the client: hold to record, or tap to
  start and tap to stop. And whether there is a count-in or a level meter.
- How long a take may be. The mic test allows 30 seconds (about 1.9 MB at the
  codec's 32 kHz, mono); anything over 2 MB of audio would not fit the
  sample cache and would play one note at a time.
- What the new file is called, and how it gets a name. File names are limited
  to 23 characters including `.wav`. The client has an on-screen keyboard.
- Whether you hear the take and can accept or discard it before it is saved,
  and whether saving drops you into the sample editor to trim it.
- Level: the mic test showed loud speech close to the mic reaching full scale
  at the present gain, so a take can clip.
- Limits: the sample list holds 50 entries, and the server is busy with the
  mic for the vocoder and the chord and scope screens, so recording has to
  take turns with those. Recording while a song plays is probably out.

## Later: per-sample transpose

**Asked:** 15 Nov 2026.

A sample has a TUNE; it also needs a TRANSPOSE.

TUNE in the sample editor moves in cents (a tap is one cent, holding speeds
up). It can reach four octaves either way, so a transpose is possible with it
today, but only by holding the button through hundreds of cents. What is
missing is a control that moves in whole semitones.

**To settle when this is picked up:**

- Whether TRANSPOSE is a second stored value or simply semitone steps on the
  same one. (Suggestion: the same one. The stored tune is already in cents
  and already shown as semitones.cents, e.g. `+7.05`; TRANSPOSE −/+ would
  move it by 100 and TUNE −/+ by 1. Nothing new to store, and no change to
  `trim.txt` or to the messages between the two devices.)
- If they are kept separate: the range of each, and that TUNE would then
  stay within a semitone.
- Where the buttons go in the sample editor — the same shortage of room as
  the velocity switch below. The two are worth laying out together.
- Whether a sample column in a song, which has its own TRANSPOSE and TUNE,
  needs anything changed. (As it stands the sample's own value is added to
  the column's.)

## Later: per-sample velocity sensitivity on/off

**Asked:** 15 Nov 2026.

A sample needs a flag saying whether velocity sensitivity is enabled for it.

Today every sample played from the MIDI keys (a square on the keys, or the
sample editor) follows how hard the key is struck: velocity 100 is the
sample's own level, harder is louder (and can clip), softer is quieter. That
suits some samples and not others — a thunderclap or a spoken phrase probably
wants the same level however it is hit.

**To settle when this is picked up:**

- What "off" plays at. (Suggestion: the sample's own level, as velocity 100
  does now.)
- Whether a new sample starts with it on or off.
- Where the flag is kept. The natural place is with the sample's other
  settings in `/samples/trim.txt`, as a fifth field after tune, so it follows
  the sample into every song like trim and tune do.
- Where the switch goes in the sample editor. The header already holds LOOP,
  PLAY and BACK and the bottom row holds zoom, tune and SPECTRUM, so one of
  them has to give up some room.
