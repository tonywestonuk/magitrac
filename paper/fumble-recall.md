# Fumble recall: act on the first key, let the last key count

**Tony Weston** — independent; author of MagiTrac
Draft 0.1, 14 November 2026. Not peer reviewed.

## Abstract

In a performer-cued sequencer, a keyboard player steers a backing arrangement
by playing cue keys: one key releases the next phrase, another jumps to a
different section. A fumbled cue — two keys landing a few tens of milliseconds
apart — forces the machine to choose. Waiting to see which key was meant adds
latency to every cue; acting on the first key and ignoring the rest locks in
the mistake whenever the slip comes first. This paper describes a third rule,
*fumble recall*. The first key is acted on at once. For half a row afterwards,
a different key *recalls* it: the sequencer is put back to the state it was in
before the first key, the new key is judged from there, and any notes the
recalled key started are silenced. The last key played is the one that counts,
and a clean cue is never delayed. The rule is implemented in the open-source
MagiTrac firmware and has been played on the instrument. In a replay experiment
that injected 1,584 two-key fumbles into a real arrangement, every fumble
resolved to exactly the state a single key would have produced, where the
earlier first-key rule left 42% of them in a state neither key alone could
reach. The rule does not make fumbles disappear: when the stray key comes
last, it is the stray key that counts. But the player knows which key they
ended on, so they know where the machine is and how to bring it back — which
they cannot know when the outcome turns on which of two keys landed a few
milliseconds sooner.

## 1. Introduction

MagiTrac is a tracker-style sequencer built to be played rather than started.
The arrangement is a set of *blocks* of *rows*. A column of the pattern holds
*cues*: rows where the machine waits for, or snaps to, a key played by the
keyboard player on a chosen range of their instrument. Each block also has a table
saying what each of the twelve pitch classes does there — stay, jump to another
block, change the transposition. The song follows the player; there is no song
position to chase.

This puts a great deal of meaning on single key presses, and hands slip. Two
adjacent keys go down together, or a finger catches a neighbour on the way to
the right one. The two note-ons arrive a few milliseconds apart and the
sequencer has to decide what was meant.

There are two familiar answers.

*Wait and decide.* Hold the first key for a short window, then act on whatever
has arrived. This is how switch debouncing and chord windows work. It is
simple and its result does not depend on which key came first, but it delays
every cue by the length of the window, including the great majority that were
played cleanly. The usual target for the delay between a gesture and its
sound is around 10 ms [4, 5]; a decision window is several times that.

*First key wins.* Act at once, then ignore further keys for a short time. No
latency, but the decision is final at the first note-on. If the slip came
first, the machine is now somewhere the player did not send it, and the
correcting key is the one that gets ignored.

MagiTrac used the second rule until November 2026. In use it had a worse
flaw than its definition suggests: the ignore window covered cue releases but
not the per-key actions, so a second key could move the sequencer to a new
block without playing that block's first row. Notes from the previous block
were left sounding until the next cue. That bug is what prompted the redesign.

The replacement rests on one premise about how players behave: **the last key
they play is their best statement of what they meant.** The model is the
"recall" some e-mail systems offer — the message goes when you press send, and
for a short while afterwards you can take it back. Here, the first key takes
effect immediately, and for a few tens of milliseconds the player can replace
it simply by playing the key they meant.

The premise has a second half, and it matters most when the player gets it
wrong. **The performer knows.** If the last key was the wrong one, they made
that choice, by accident, and they know which key it was. The machine is now
on the wrong track, but it is on the track that key leads to, and the player
can steer it back from there. Under a first-key rule the machine's position
depends on an ordering of two note-ons a few milliseconds apart, which the
player has no way to feel. They have to work out where the machine went by
listening before they can correct it. A rule should not only choose well; its
choice should be one the performer can predict from what they know they did.

## 2. The rule

### 2.1 Statement

1. A performer key that does something — releases a cue, switches block,
   changes transposition — is acted on immediately. Before acting, the
   sequencer saves its position state.
2. For half a row after that, a *different* key recalls it. The saved state is
   restored, and the new key is judged exactly as if the earlier one had never
   been played.
3. Notes that the recalled key started, and that the new key's outcome does
   not replace, are sent note-offs.
4. The window restarts from the new key, and the saved state is kept, so a
   third key is again judged from the position before the fumble.

In short: act now, and for half a row the last key counts.

### 2.2 When the earlier key stands

A later key inside the window does *not* recall the earlier one in three
cases.

- **It is the same key** (same pitch class). A bounce or an octave doubling is
  not a correction.
- **It would do exactly the same thing** — same block, same cue row, same
  transposition. This is a chord played onto a cue that accepts any key.
  Recalling would only restart the row a few milliseconds late.
- **It means nothing where the first key was played** — not a cue there, no
  entry in the block's table. A key with nothing to say cannot be a correction.

In all three the first key's action and its timing are kept.

### 2.3 What is saved, and what cannot be

The saved state is everything a key can change about *where* the sequencer is
and *how fast* it is going: block and row, the time the next row is due,
whether it is waiting at a cue, transposition, current tempo and the history
the tempo is derived from, any queued block, and the cursor into the note
data. It is about a hundred bytes, copied once per acted-on key.

What has already been sent to the synthesiser cannot be taken back. Each
output column therefore carries a generation counter that increases when a
note starts. After a recall, a column whose sounding note was started by the
recalled key, and which the new key has not retriggered, is silenced. The
audible cost of a recall is that those notes sound for the gap between the two
keys. Controller messages and one-shot samples fired by the recalled row are
not undone.

### 2.4 The window

The window is half a row at the current tempo: 57 ms at the 115 ms rows of a
130 BPM arrangement. Tying it to the row keeps it shorter than the smallest
step the arrangement can express, so keys played a row apart are always two
events. It also means the window shrinks as the player speeds up.

A pending recall is dropped when the transport is stopped, restarted or moved,
and when the pattern is edited from the control surface.

### 2.5 Worked examples

Block 3 is held with G and moves to block 4 on A♭.

| Played (inside the window) | Result |
|---|---|
| A♭ then G | Block 4 starts on A♭; G recalls it. The sequencer is on block 3, as if A♭ had not been played. |
| G then A♭ | Block 3's cue is taken on G; A♭ recalls it and, judged from the same starting point, moves to block 4. Block 4's first row plays, including any note-offs it contains. |

Block 1 moves to block 2 on F and to block 3 on G; other keys stay on block 1.

| Played | Result |
|---|---|
| E, F, G | Block 3. Each key recalls the one before. |
| G, F, E (E releases a cue in block 1) | Block 1. |
| F, then a key that means nothing in block 1 | Block 2. The F stands. |
| F, F | Block 2. Same key. |

## 3. Implementation

The rule lives in `magitrac_server_s3/midi_player.cpp`. The function that
judges a key (`seqPerformerNoteCore`) reports the decision it reached — target
block, cue row, transposition — without the caller needing to know how. The
wrapper around it (`seqOnPerformerNote`) owns the window: it saves the state
(`SeqPos`) before a key is acted on, and on a second key restores it, re-runs
the judgement, compares decisions, and either keeps the new outcome and
silences leftover notes or puts the first outcome back. The judgement itself
was not rewritten; the recall is a layer around it. Host-side tests
(`test_seq.cpp`) cover the cases in section 2.5 among others.

No timer or extra task is involved. Nothing happens when the window closes;
it is only consulted when another key arrives. A clean cue costs one state
copy.

## 4. Evaluation

### 4.1 Method

The sequencer source was compiled unmodified for a desktop host with a mocked
millisecond clock and its MIDI output captured, and a scripted performance was
replayed through it. Two builds were compared: the current firmware (*recall*)
and the firmware immediately before the change (*first key*; tag `fumble-recall-baseline`, commit `46ab5f3`).

The arrangement is one from the author's live set: 16-row blocks, a cue every
four rows, 130 BPM. The performance visits four blocks.

| Block | Cue released by | Keys that do something else |
|---|---|---|
| 1 | any key | G → block 3, A♭ → block 2; every key resets the transposition |
| 2 | A♭ | C → block 1, G → block 3 |
| 3 | G | C → block 1; F, A♭ → block 4 |
| 4 | any key | every key sets the transposition from its pitch class |

The clean performance is twelve cue keys, 460 ms apart:
C C C C A♭ A♭ G G A♭ C C C. Both builds produce identical note-on and
note-off streams for it.

Each trial adds one stray key beside one cue: each of the 12 cues × the 11
other pitch classes × the stray arriving before or after the intended key × a
gap of 5, 10, 20, 30, 40 or 50 ms. That is 1,584 trials per build, all inside
the 57 ms window. The rest of the performance is played as written.

Each trial is compared, from the later key onward, with three reference
replays: the clean performance, the performance with only the earlier key of
the pair, and with only the later key. "Same" means the same note-ons at the
same millisecond and the same block between every later pair of cues.

### 4.2 Results

| Outcome | First key | Recall |
|---|---|---|
| As if only the later key had been played | 204 (12.9%) | 948 (59.8%) |
| As if only the earlier key had been played | 720 (45.5%) | 636 (40.2%) |
| **Like neither key alone** | **660 (41.7%)** | **0** |
| A pitched note sounding over 10 ms longer than any clean one | 316 (19.9%), up to 510 ms | 62 (3.9%), up to 128 ms |
| A note never released | 0 | 0 |

(The longest pitched note in the clean performance is 115 ms, one row.)

Split by which key came first:

| | First key | Recall |
|---|---|---|
| **Stray first, intended key last** (792 trials) | | |
| Identical to the clean performance from the intended key on | 204 (25.8%) | 576 (72.7%) |
| On the intended route of blocks | 762 (96.2%) | 792 (100%) |
| **Intended key first, stray last** (792 trials) | | |
| Identical to the clean performance | 486 (61.4%) | 420 (53.0%) |
| On the intended route of blocks | 684 (86.4%) | 624 (78.8%) |

Under recall, in 612 of the 948 recalls the recalled key had started notes the
later key does not play. Each sounded for exactly the gap between the keys
(5–50 ms); there were never more than two.

### 4.3 Reading the results

*Recall is predictable.* Every fumble ended in a state a single key would
have produced: the later key's, or — in the three cases of section 2.2 — the
earlier key's. The first-key rule left two fumbles in five in a hybrid state,
which is where its long-sounding notes come from.

*When the player ends on the right key, recall gets them there.* With the
stray first, every trial stayed on the intended route, and 72.7% were
indistinguishable from a clean performance from the intended key onward. The
remainder are strays that do the same thing as the intended key; there the
earlier key stands, so the cue is taken up to 50 ms early.

*When the stray comes last, recall follows the stray.* This is the rule doing
what it says, and it costs something: with the stray last, 21.2% of trials
left the intended route, against 13.6% under the first-key rule. Counting both
orders equally, recall left the route slightly *more* often (10.6% against
8.7%). It does not reduce the number of wrong turns in this experiment; it
moves all of them to the case where the last key the player touched was the
wrong one, and removes them completely from the case where it was the right
one.

*The two kinds of wrong turn are not the same.* Counting them equally, as the
table does, hides the difference the rule is built on. Under recall, a wrong
turn is always the one the player's last key asked for. They know which key
that was, so they know where the machine is and which key will bring it back.
Under the first-key rule a wrong turn follows from which of two keys landed
first, which the player cannot perceive, and in two fumbles out of five the
machine is somewhere neither key alone leads. The first row of the results
table is the measurement of this: under recall the state after a fumble is
always one the player can name from the keys they played.

This experiment does not measure what follows from that — how quickly a
player recovers from each kind of wrong turn — and it weights both orders and
all eleven stray keys equally, which real fumbles will not. The premise, that
a player knows the key they ended on and usually meant it, is the author's
experience of his own playing. The rule has behaved well in his hands on the
instrument, but none of this has been tested on other players.

### 4.4 Limits

One arrangement, one performance, synthetic fumbles, simulated time. Hardware
latency and jitter are not modelled. No listening test was run on the 5–50 ms
of a recalled note, and no other players have tried it. The comparison is with
the first-key rule as it was actually shipped, including its flaw; a first-key
rule that also ignored per-key actions inside the window would be expected to
score better on hybrid states and long notes, and the same on which key wins.

A wait-and-decide rule was not built. By construction it would reach the same
final states as recall wherever it judges the last key, and would add its
window — here 57 ms — to every cue.

## 5. Related work

**Score following and computer accompaniment** [1, 2, 3, 6, 7] track a
performer through a written score and tolerate wrong, extra and missing notes
by matching the performance against the score. The setting differs: there the
whole part is known and position is estimated; here a handful of keys make
branching decisions, and there is no score to say which of two keys was the
slip.

**Ignoring a suspect key.** A Yamaha patent on a performance control apparatus
[10] derives accompaniment tempo from the time between key presses and
excludes a press that follows the previous one too closely, to keep accidental
adjacent-key presses out of the calculation. This is the first-key family: the
later press is discarded, and nothing already done is reversed. (Based on the
published description; the claims were not studied in full.)

**Waiting.** Chord and combo detection on programmable keyboards uses a fixed
term within which keys count as one event — 50 ms by default in QMK [9].
Launch quantisation in clip-based performance software defers a launch to the
next grid point [11]. Both trade response time for certainty.

**Optimistic execution with rollback.** The closest mechanism is outside
music. Time Warp [8] lets a distributed simulation run ahead and rolls back
when a message arrives in its past. Rollback netcode for fighting games [12]
advances the game on a guess at the remote player's input and re-simulates
from a saved state when the real input arrives. Fumble recall has the same
shape — save, proceed, restore, redo — with two differences. What is corrected
is not a prediction about missing input but the performer's own earlier input,
on the strength of their later one. And the side effect, sound, cannot be
hidden or re-rendered, so it is compensated rather than undone.

The author has not found this rule described for a musical instrument or
sequencer. The search — patent databases and the computer-music literature —
was limited, and he would be glad to be shown earlier work.

## 6. Discussion

The rule asks nothing of the player. There is no cancel gesture; the
correction is the thing they would do anyway, which is play the right key.

It also keeps the machine legible when the correction does not come. A
sequencer the performer steers is only playable if they can tell where it is.
After a fumble under this rule, the answer is given by something the player
already knows: the key they ended on. The cost of a mistake is then one
wrong turn they can see and undo, not a search for where the machine went.

A key that means nothing does not recall (section 2.2). The alternative —
any later key cancels the first — would let a player back out of a wrong block
switch by touching a neutral key, at the price of making every brushed
neighbour destructive. The current choice favours the common accident over the
deliberate cancel. It is the least settled part of the design.

Work that would strengthen this paper: recordings of real fumbles from several
players, to learn which key tends to come last; a test of whether players can
tell which key they ended on when the two are only a few milliseconds apart,
and of how quickly they recover from a wrong turn under each rule; a listening
test on the audibility of recalled notes; and a built wait-and-decide baseline
for a direct comparison of feel.

## 7. Availability and rights

The implementation is part of MagiTrac, published under the GNU GPL v3 at
<https://github.com/tonywestonuk/magitrac>. The rule first appears in commit
`b895970` (tag `fumble-recall-v1`, 14 November 2026) and reached the `main`
branch the same day as commit `68dc087`. The experiment in section 4 is in
`paper/eval/`.

The author does not intend to seek a patent on the rule described here and
publishes this description so that anyone may use it. The licence above
applies to the code, not to the idea.

## Acknowledgements and use of AI

The rule — the last key counts, the examples of section 2.5, and the e-mail
recall model — is the author's, and he tested the result on the instrument.
The firmware implementation, the handling of the three cases in section 2.2,
the experiment and the drafting of this paper were done with an AI coding
assistant (Claude, Anthropic), working to the author's specification.

## References

1. R. B. Dannenberg. An on-line algorithm for real-time accompaniment. *Proc.
   International Computer Music Conference*, Paris, 1984, pp. 193–198.
2. B. Vercoe. The synthetic performer in the context of live performance.
   *Proc. International Computer Music Conference*, Paris, 1984, pp. 199–200.
3. J. J. Bloch and R. B. Dannenberg. Real-time computer accompaniment of
   keyboard performances. *Proc. International Computer Music Conference*,
   Burnaby, 1985, pp. 279–290.
4. D. Wessel and M. Wright. Problems and prospects for intimate musical control
   of computers. *Computer Music Journal* 26(3), 2002, pp. 11–22.
5. A. McPherson, R. Jack and G. Moro. Action-sound latency: are our tools fast
   enough? *Proc. New Interfaces for Musical Expression*, Brisbane, 2016,
   pp. 20–25.
6. R. B. Dannenberg and C. Raphael. Music score alignment and computer
   accompaniment. *Communications of the ACM* 49(8), 2006, pp. 38–43.
7. A. Cont. Antescofo: anticipatory synchronization and control of interactive
   parameters in computer music. *Proc. International Computer Music
   Conference*, Belfast, 2008, pp. 33–40.
8. D. R. Jefferson. Virtual time. *ACM Transactions on Programming Languages
   and Systems* 7(3), 1985, pp. 404–425.
9. QMK Firmware documentation: Combos. <https://docs.qmk.fm/features/combo>
10. S. Usa and T. Urai (Yamaha Corp.). Performance control apparatus and
    program therefor. US Patent 7,633,003, granted 15 December 2009.
11. Ableton. *Live Reference Manual*, "Launching Clips".
    <https://www.ableton.com/en/manual/launching-clips/>
12. T. Cannon. GGPO: rollback networking SDK. <https://www.ggpo.net>
