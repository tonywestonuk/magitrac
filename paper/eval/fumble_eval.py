#!/usr/bin/env python3
"""Fumble-injection experiment for paper/fumble-recall.md.

A clean performance of one song is a list of cue keys at fixed times.  Each trial
adds ONE stray key next to ONE cue — just before the intended key or just after
it — and replays the whole performance through two builds of the sequencer:

    sim_first    the earlier rule: first key wins (snap cooldown)
    sim_recall   the rule in the paper: the last key counts (fumble recall)

Every trial is compared with three reference performances, all replayed on the
recall build (the two builds agree whenever no two keys fall inside the window):
the clean one, the one where only the EARLIER key of the pair was pressed, and
the one where only the LATER key was pressed.

Usage: fumble_eval.py <dir with sim_first and sim_recall> <song.mgt>   (see run.sh)
"""
import collections, json, re, subprocess, sys

BUILD, SONG = sys.argv[1], sys.argv[2]
NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
def midi(name): return 36 + NAMES.index(name)          # octave 2

# The performance: one cue key every four rows (460 ms at the song's 130 BPM).
CLEAN = ["C", "C", "C", "C", "G#", "G#", "G", "G", "G#", "C", "C", "C"]
T0, STEP = 100, 460
GAPS = [5, 10, 20, 30, 40, 50]                           # ms between the two keys of a fumble
RULES = ("sim_first", "sim_recall")


def replay(sim, keys):
    args = ["%d:%d" % k for k in sorted(keys)]
    out = subprocess.run([BUILD + "/" + sim, SONG] + args, capture_output=True, text=True).stdout
    ons, offs, blocks = [], [], []
    for line in out.splitlines():
        m = re.match(r"\s+(\d+)\s+ch(\d+)\s+(ON|OFF)\s+(\S+)", line)
        if m:
            (ons if m.group(3) == "ON" else offs).append((int(m.group(1)), int(m.group(2)), m.group(4)))
            continue
        m = re.match(r"\s+(\d+)\s+-- block (\d+)", line)
        if m:
            blocks.append((int(m.group(1)), int(m.group(2))))
    return {"on": ons, "off": offs, "blocks": blocks}


def block_at(run, t):
    b = None
    for bt, blk in run["blocks"]:
        if bt <= t:
            b = blk
    return b


def route(run, t):
    """The block the sequencer is in half-way between each pair of cues after t."""
    mids = [T0 + j * STEP + STEP // 2 for j in range(len(CLEAN))]
    return [block_at(run, x) for x in mids if x > t]


def after(run, t):
    """Everything the sequencer does from t on: each note-on, and its route."""
    return (sorted(e for e in run["on"] if e[0] >= t), route(run, t))


def notes(run):
    """(duration or None, channel, note, start) for each note on the pitched channels."""
    live, res = {}, []
    evs = [(t, 1, ch, nt) for t, ch, nt in run["on"]] + [(t, 0, ch, nt) for t, ch, nt in run["off"]]
    for t, is_on, ch, nt in sorted(evs):
        if ch == 10:                                   # drums: one-shots, length means nothing
            continue
        if is_on:
            if (ch, nt) in live:
                res.append((t - live[(ch, nt)], ch, nt, live[(ch, nt)]))
            live[(ch, nt)] = t
        elif (ch, nt) in live:
            t0 = live.pop((ch, nt))
            res.append((t - t0, ch, nt, t0))
    res += [(None, ch, nt, t0) for (ch, nt), t0 in live.items()]
    return res


clean_keys = [(T0 + i * STEP, midi(k)) for i, k in enumerate(CLEAN)]
clean = {rule: replay(rule, clean_keys) for rule in RULES}
agree = all(clean["sim_first"][k] == clean["sim_recall"][k] for k in ("on", "off"))
clean_longest = max(d for d, *_ in notes(clean["sim_recall"]) if d is not None)
print("clean performance: %d note-ons; identical under both rules: %s; longest pitched note %d ms"
      % (len(clean["sim_recall"]["on"]), agree, clean_longest))

rows = []
for i, key in enumerate(CLEAN):
    Ti = T0 + i * STEP
    block = block_at(clean["sim_recall"], Ti - 1)
    rest = clean_keys[:i] + clean_keys[i + 1:]           # the other cues are always played as written
    for stray in NAMES:
        if stray == key:
            continue
        for order in ("stray first", "stray last"):
            for gap in GAPS:
                if order == "stray first":
                    first, last = (Ti - gap, midi(stray)), (Ti, midi(key))
                else:
                    first, last = (Ti, midi(key)), (Ti + gap, midi(stray))
                only_first = replay("sim_recall", rest + [first])
                only_last = replay("sim_recall", rest + [last])
                for rule in RULES:
                    run = replay(rule, rest + [first, last])
                    t = last[0]
                    got = after(run, t)
                    as_last, as_first = got == after(only_last, t), got == after(only_first, t)
                    ns = notes(run)
                    too_long = [d for d, *_ in ns if d is not None and d > clean_longest + 10]
                    in_last = set(only_last["on"])
                    taken_back = [d for d, ch, nt, t0 in ns
                                  if first[0] <= t0 < t and (t0, ch, nt) not in in_last]
                    rows.append(dict(
                        rule=rule, cue=i, block=block, key=key, stray=stray, order=order, gap=gap,
                        as_last=as_last, as_first=as_first,
                        as_clean=got == after(clean[rule], t),
                        on_route=route(run, t) == route(clean[rule], t),
                        never_released=sum(1 for d, *_ in ns if d is None),
                        too_long=len(too_long), longest=max(too_long, default=0),
                        taken_back=len(taken_back),
                        taken_back_longest=max([d if d is not None else 10 ** 6 for d in taken_back], default=0)))

json.dump(rows, open(BUILD + "/results.json", "w"), indent=0)


def frac(a, b):
    return "%4d / %d (%5.1f%%)" % (a, b, 100.0 * a / b)


def summary(R, label):
    n = len(R)
    print("  %s" % label)
    print("    as if only the LAST key had been pressed   %s" % frac(sum(r["as_last"] and not r["as_first"] for r in R), n))
    print("    as if only the FIRST key had been pressed  %s" % frac(sum(r["as_first"] and not r["as_last"] for r in R), n))
    print("    like neither single key                    %s" % frac(sum(not r["as_first"] and not r["as_last"] for r in R), n))
    print("    identical to the clean performance         %s" % frac(sum(r["as_clean"] for r in R), n))
    print("    off the intended route of blocks           %s" % frac(sum(not r["on_route"] for r in R), n))
    print("    a pitched note sounding too long           %s   longest %d ms"
          % (frac(sum(r["too_long"] > 0 for r in R), n), max(r["longest"] for r in R)))
    print("    a note never released                      %s" % frac(sum(r["never_released"] > 0 for r in R), n))


for rule in RULES:
    R = [r for r in rows if r["rule"] == rule]
    print("\n%s" % rule)
    summary(R, "all trials")
    for order in ("stray first", "stray last"):
        summary([r for r in R if r["order"] == order], order)

R = [r for r in rows if r["rule"] == "sim_recall" and r["as_last"] and not r["as_first"]]
T = [r for r in R if r["taken_back"]]
print("\nrecalls: %d; in %d of them the recalled key had started notes the later key does not play"
      % (len(R), len(T)))
by_gap = collections.defaultdict(set)
for r in T:
    by_gap[r["gap"]].add(r["taken_back_longest"])
print("  how long such a note sounded, by gap between the keys:",
      ", ".join("%d ms -> %s ms" % (g, "/".join(map(str, sorted(v)))) for g, v in sorted(by_gap.items())))
print("  most such notes in one trial:", max(r["taken_back"] for r in T))
