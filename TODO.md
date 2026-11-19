# To do

Things agreed but not started.

## Next — in this order

Agreed on 15 Nov 2026 for the next session. Details for each are further down.

1. ~~Commit the work from 15 Nov.~~ Done 16 Nov (PR #4).
2. ~~Fix the gap when the sample list is rebuilt.~~ Done 16 Nov: the table
   is built to one side and swapped in, rescanned once per list, and
   `trim.txt` is read only at boot. Host test: `test_manifest.cpp`.
3. ~~Back up the samples folder.~~ Done 16 Nov: BACKUP asks whether to
   include the WAVs; `samples.txt` and `trim.txt` always go in. Restore
   streams files of any size, puts them back in `/samples/`, then the server
   re-reads the list and settings (numbers kept) and empties its sample
   cache. A backup folder now lists up to 256 files for restore (was 33).
4. **Soak test** — play a whole set on the new server build while logging.
5. ~~ADD SAMPLE.~~ Done 17 Nov: ADD on the samples screen arms the server,
   which shows PRESS AND HOLD TO RECORD with a live mic level; hold its glass
   to record (30 s limit), let go and it plays straight back with KEEP /
   CANCEL / RESAMPLE (hold) on the server (a bezel tap cancels). KEEP hands
   it to the client, which names it on the keyboard
   (default `recNN`) and saves it into `/samples`, then opens the sample
   editor on it. Refused while a song plays or the organ is open.

Also done 16 Nov: per-sample transpose (±ST buttons) and velocity switch
(VEL ON/OFF) in the sample editor; the REED organ PROC sound, with its
CHAMBER resonance fitted to an oboe recording.

---

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
