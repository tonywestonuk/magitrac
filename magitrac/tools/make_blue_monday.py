#!/usr/bin/env python3
"""make_blue_monday.py — Generate Blue Monday .mgt file for MagiTrac.

Based on MIDI analysis of New_Order_-_Blue_Monday.mid.
Performer plays bass (F, C, D, G) to drive tempo via performer sync.
A note advances to next section; E note goes back.

Instrument mapping (user assigns actual sounds):
  7  = Bass Drum      (MIDI ch10, note 36)
  8  = Snare          (MIDI ch10, note 40)
  9  = Hi-Hat Closed  (MIDI ch10, note 42)
  10 = Bass Synth     (MIDI ch3, prog 38/39)
  11 = Synth Stab     (MIDI ch1, prog 28)
  12 = Guitar/Pad     (MIDI ch4, prog 27)
  13 = Strings/Lead   (MIDI ch6, prog 50)

Usage:
    python3 make_blue_monday.py [output.mgt]
"""

import struct
import sys

# ── MagiTrac constants (from TrackerData.h) ──────────────────────────────────

MAX_PATTERNS    = 32
MAX_ROWS        = 64
MAX_COLUMNS     = 8
MAX_SONG_NOTES  = 2048
NOTE_NULL       = 0xFFFF
NOTE_EMPTY      = 0
NOTE_OFF        = 0xFF
EFFECT_SYNC     = 0x0E
EFFECT_WAIT     = 0x0F
COL0_NAV_NONE   = 0
COL0_NAV_NEXT   = 1

SONG_FILE_MAGIC   = 0x4D414754  # "MAGT"
SONG_FILE_VERSION = 10

TRACKER_TO_MIDI_OFFSET = 11

# ── Helper: MIDI note → tracker note value ────────────────────────────────────

def midi_to_tracker(midi_note):
    return midi_note - TRACKER_TO_MIDI_OFFSET

def make_note(semitone, octave):
    return octave * 12 + semitone + 1

# Pitch class constants (semitone index 0-11)
PC_C  = 0;  PC_CS = 1;  PC_D  = 2;  PC_DS = 3
PC_E  = 4;  PC_F  = 5;  PC_FS = 6;  PC_G  = 7
PC_GS = 8;  PC_A  = 9;  PC_AS = 10; PC_B  = 11

# ── Instruments (indices into instrument table) ──────────────────────────────

INST_KICK    = 7
INST_SNARE   = 8
INST_HH      = 9
INST_BASS    = 10
INST_STAB    = 11
INST_GUITAR  = 12
INST_STRINGS = 13

# ── Tracker note values for common pitches ────────────────────────────────────

# Drums (MIDI ch10 GM note numbers → tracker)
TN_KICK  = midi_to_tracker(36)  # 25
TN_SNARE = midi_to_tracker(40)  # 29
TN_HH    = midi_to_tracker(42)  # 31

# Bass (octave 2)
TN_F2 = midi_to_tracker(41)  # 30
TN_C2 = midi_to_tracker(36)  # 25
TN_D2 = midi_to_tracker(38)  # 27
TN_G2 = midi_to_tracker(43)  # 32

# Synth stabs (octave 5)
TN_C5 = midi_to_tracker(72)  # 61
TN_D5 = midi_to_tracker(74)  # 63
TN_F5 = midi_to_tracker(77)  # 66
TN_G5 = midi_to_tracker(79)  # 68

# Guitar (octave 3)
TN_C3 = midi_to_tracker(48)  # 37
TN_D3 = midi_to_tracker(50)  # 39
TN_F3 = midi_to_tracker(53)  # 42
TN_G3 = midi_to_tracker(55)  # 44

# Strings (octave 5)
TN_A5 = midi_to_tracker(81)  # 70
TN_E5 = midi_to_tracker(76)  # 65

# Vocal/lead melody (octave 4)
TN_C4 = midi_to_tracker(60)  # 49
TN_D4 = midi_to_tracker(62)  # 51
TN_E4 = midi_to_tracker(64)  # 53
TN_F4 = midi_to_tracker(65)  # 54
TN_G4 = midi_to_tracker(67)  # 56
TN_A4 = midi_to_tracker(69)  # 58

# ── Note pool builder ─────────────────────────────────────────────────────────

class NotePool:
    """Builds the sparse note pool and per-pattern circular linked lists."""

    def __init__(self):
        self.nodes = []  # list of (row, col, note, instrument, effect, param)
        self.pattern_notes = {}  # pattern_idx → [node_indices]

    def add(self, pattern, row, col, note, instrument=0, effect=0, param=0):
        idx = len(self.nodes)
        self.nodes.append((row, col, note, instrument, effect, param))
        self.pattern_notes.setdefault(pattern, []).append(idx)

    def build(self):
        """Build the NoteNode array with circular linked lists.
        Returns (notePool bytes, noteFreeHead, {pattern: noteHead})."""
        # Each NoteNode: row(1) col(1) note(1) instrument(1) effect(1) param(1) next(2)
        pool = bytearray(MAX_SONG_NOTES * 8)
        heads = {}

        for pat, indices in self.pattern_notes.items():
            # Sort by (row, col) for proper playback order
            indices.sort(key=lambda i: (self.nodes[i][0], self.nodes[i][1]))

            # Build circular linked list
            heads[pat] = indices[0]
            for i in range(len(indices)):
                row, col, note, inst, eff, par = self.nodes[indices[i]]
                next_idx = indices[(i + 1) % len(indices)]  # circular
                offset = indices[i] * 8
                struct.pack_into('<BBBBBBH', pool, offset,
                                row, col, note, inst, eff, par, next_idx)

        # Build free list from unused nodes
        used = set()
        for indices in self.pattern_notes.values():
            used.update(indices)

        free_head = NOTE_NULL
        for i in range(MAX_SONG_NOTES - 1, -1, -1):
            if i not in used:
                offset = i * 8
                # Write a minimal free node — just needs 'next' field
                struct.pack_into('<H', pool, offset + 6, free_head)
                free_head = i

        return bytes(pool), free_head, heads


# ── Drum patterns ─────────────────────────────────────────────────────────────
# From MIDI analysis. Each bar = 16 rows (16th note grid at speed 6, 133 BPM).
# Bar A (odd): straight kick on every beat
# Bar B (even): kick on beats + 16th-note fill on beats 3-4

def kick_bar_a():
    """Kick drum rows for bar A (straight four-on-floor)."""
    return [0, 4, 8, 12]

def kick_bar_b():
    """Kick drum rows for bar B (with 16th fill on beats 3-4)."""
    return [0, 4, 8, 9, 10, 11, 12, 13, 14, 15]

def snare_bar():
    """Snare on beats 2 and 4."""
    return [4, 12]

def hh_bar():
    """HH: 8th notes + grace notes at x.75 positions."""
    # beats 1, 1.5, 1.75, 2, 2.5, 2.75, 3, 3.5, 3.75, 4, 4.5, 4.75
    return [0, 2, 3, 4, 6, 7, 8, 10, 11, 12, 14, 15]

# ── Bass rhythm (per 2-beat / 8-row group) ────────────────────────────────────
# From MIDI: x.xx.xx (beats 1, 1.5, 1.75, 2, 2.5, 2.75)

def bass_half_bar():
    """Bass rhythm: 6 notes per 2-beat group (8 rows)."""
    return [0, 2, 3, 4, 6, 7]

# ── Synth stab pattern (from ch1, bars 10-12 in MIDI) ─────────────────────────
# 4-bar pattern, quantised to 16th grid

def stab_pattern():
    """Returns list of (row, tracker_note) for the synth stab melody."""
    # Bar 1: F..F C.C ..D
    # Bar 2: D .D.D .D D
    # Bar 3: F.F .G C ..D.D
    # Bar 4: .D D .D.D .D
    return [
        # Bar 1 (rows 0-15)
        (2,  TN_F5), (4,  TN_F5),
        (8,  TN_C5), (10, TN_C5),
        (14, TN_D5),
        # Bar 2 (rows 16-31)
        (16, TN_D5),
        (20, TN_D5), (22, TN_D5),
        (26, TN_D5), (28, TN_D5),
        # Bar 3 (rows 32-47)
        (32, TN_F5), (34, TN_F5),
        (38, TN_G5), (40, TN_C5),
        (44, TN_D5), (46, TN_D5),
        # Bar 4 (rows 48-63)
        (50, TN_D5), (52, TN_D5),
        (56, TN_D5), (58, TN_D5),
        (62, TN_D5),
    ]

# ── Guitar riff (from ch4, bars 44-47 in MIDI) ───────────────────────────────

def guitar_pattern():
    """Returns list of (row, tracker_note) for the guitar riff."""
    return [
        # Bar 1: D eighth notes
        (0,  TN_D3), (2, TN_D3), (4, TN_D3), (6, TN_D3),
        (8,  TN_D3), (10, TN_D3), (12, TN_D3), (14, TN_D3),
        # Bar 2: D..C..D (half notes)
        (16, TN_D3), (20, TN_C3), (24, TN_D3),
        # Bar 3: F eighth notes
        (32, TN_F3), (34, TN_F3), (36, TN_F3), (38, TN_F3),
        (40, TN_F3), (42, TN_F3), (44, TN_F3), (46, TN_F3),
        # Bar 4: F..G..D (half notes)
        (48, TN_F3), (52, TN_G3), (56, TN_D3),
    ]

# ── Strings melody (from ch6, 2-bar phrase) ──────────────────────────────────

def strings_pattern():
    """Returns list of (row, tracker_note) for the strings."""
    # From MIDI: A5(beat 4) → E5(beat 2 next bar) → F5(beat 1) → repeats
    # Spread across 4 bars following the chord cycle
    return [
        # Bar 1
        (12, TN_A5),
        # Bar 2
        (20, TN_E5),
        (24, TN_F5),
        # Bar 3
        (44, TN_A5),
        # Bar 4
        (52, TN_E5),
        (56, TN_D5),
    ]

# ── Vocal melody (from ch8, bars 72-80 in MIDI) ──────────────────────────────

def vocal_pattern():
    """Returns list of (row, tracker_note) for the vocal/lead melody."""
    # Quantised from MIDI — "How does it feel" melody
    return [
        # Bar 1: F..E.E..D
        (0,  TN_F4),
        (8,  TN_E4), (10, TN_E4),
        (14, TN_D4),
        # Bar 2: ......D.D
        (28, TN_D4), (30, TN_D4),
        # Bar 3: F.F.E.E..D
        (32, TN_F4), (36, TN_F4),
        (40, TN_E4), (44, TN_E4), (46, TN_E4),
        (50, TN_D4),
        # Bar 4: ......D.D
        (60, TN_D4), (62, TN_D4),
    ]


# ── Build song ────────────────────────────────────────────────────────────────

def build_song():
    pool = NotePool()

    # ── Pattern 0: INTRO (32 rows = 2 bars, drums only, auto-advance) ────────
    pat = 0
    for r in kick_bar_a():
        pool.add(pat, r, 1, TN_KICK, INST_KICK)
    for r in kick_bar_b():
        pool.add(pat, r + 16, 1, TN_KICK, INST_KICK)
    # BLK+ on last row col 0 to auto-advance
    pool.add(pat, 31, 0, NOTE_EMPTY, COL0_NAV_NEXT)

    # ── Pattern 1: KICK+STAB (64 rows, performer sync, loops) ────────────────
    pat = 1
    # Input track (col 0) — performer SYNC/WAIT on chord changes
    # 4-bar bass cycle: F(2 beats) C(2 beats) D(4 beats) G(2 beats) C(2 beats) D(4 beats)
    pool.add(pat, 0,  0, TN_F2, 0, EFFECT_WAIT, 0)   # WAIT for F to start
    pool.add(pat, 8,  0, TN_C2, 0, EFFECT_SYNC, 0)   # SYNC on C
    pool.add(pat, 16, 0, TN_D2, 0, EFFECT_SYNC, 0)   # SYNC on D
    pool.add(pat, 32, 0, TN_G2, 0, EFFECT_SYNC, 0)   # SYNC on G
    pool.add(pat, 40, 0, TN_C2, 0, EFFECT_SYNC, 0)   # SYNC on C
    pool.add(pat, 48, 0, TN_D2, 0, EFFECT_SYNC, 0)   # SYNC on D
    # Kick (2 x barA-barB)
    for r in kick_bar_a():
        pool.add(pat, r, 1, TN_KICK, INST_KICK)
    for r in kick_bar_b():
        pool.add(pat, r + 16, 1, TN_KICK, INST_KICK)
    for r in kick_bar_a():
        pool.add(pat, r + 32, 1, TN_KICK, INST_KICK)
    for r in kick_bar_b():
        pool.add(pat, r + 48, 1, TN_KICK, INST_KICK)
    # Synth stabs (col 5)
    for row, note in stab_pattern():
        pool.add(pat, row, 5, note, INST_STAB)

    # ── Pattern 2: VERSE (64 rows, full drums + bass + stabs, loops) ──────────
    pat = 2
    # Input track
    pool.add(pat, 0,  0, TN_F2, 0, EFFECT_WAIT, 0)
    pool.add(pat, 8,  0, TN_C2, 0, EFFECT_SYNC, 0)
    pool.add(pat, 16, 0, TN_D2, 0, EFFECT_SYNC, 0)
    pool.add(pat, 32, 0, TN_G2, 0, EFFECT_SYNC, 0)
    pool.add(pat, 40, 0, TN_C2, 0, EFFECT_SYNC, 0)
    pool.add(pat, 48, 0, TN_D2, 0, EFFECT_SYNC, 0)
    # Kick
    for r in kick_bar_a():
        pool.add(pat, r, 1, TN_KICK, INST_KICK)
    for r in kick_bar_b():
        pool.add(pat, r + 16, 1, TN_KICK, INST_KICK)
    for r in kick_bar_a():
        pool.add(pat, r + 32, 1, TN_KICK, INST_KICK)
    for r in kick_bar_b():
        pool.add(pat, r + 48, 1, TN_KICK, INST_KICK)
    # Snare
    for bar_offset in [0, 16, 32, 48]:
        for r in snare_bar():
            pool.add(pat, r + bar_offset, 2, TN_SNARE, INST_SNARE)
    # Hi-hat
    for bar_offset in [0, 16, 32, 48]:
        for r in hh_bar():
            pool.add(pat, r + bar_offset, 3, TN_HH, INST_HH)
    # Bass output (col 4): F(8 rows) C(8) D(16) G(8) C(8) D(16)
    bass_notes_in_pattern = (
        [(r, TN_F2) for r in bass_half_bar()] +                        # rows 0-7: F
        [(r + 8, TN_C2) for r in bass_half_bar()] +                    # rows 8-15: C
        [(r + 16, TN_D2) for r in bass_half_bar()] +                   # rows 16-23: D
        [(r + 24, TN_D2) for r in bass_half_bar()] +                   # rows 24-31: D
        [(r + 32, TN_G2) for r in bass_half_bar()] +                   # rows 32-39: G
        [(r + 40, TN_C2) for r in bass_half_bar()] +                   # rows 40-47: C
        [(r + 48, TN_D2) for r in bass_half_bar()] +                   # rows 48-55: D
        [(r + 56, TN_D2) for r in bass_half_bar()]                     # rows 56-63: D
    )
    for row, note in bass_notes_in_pattern:
        pool.add(pat, row, 4, note, INST_BASS)
    # Synth stabs
    for row, note in stab_pattern():
        pool.add(pat, row, 5, note, INST_STAB)

    # ── Pattern 3: GUITAR (same as VERSE but adds guitar on col 6) ───────────
    pat = 3
    # Input track
    pool.add(pat, 0,  0, TN_F2, 0, EFFECT_WAIT, 0)
    pool.add(pat, 8,  0, TN_C2, 0, EFFECT_SYNC, 0)
    pool.add(pat, 16, 0, TN_D2, 0, EFFECT_SYNC, 0)
    pool.add(pat, 32, 0, TN_G2, 0, EFFECT_SYNC, 0)
    pool.add(pat, 40, 0, TN_C2, 0, EFFECT_SYNC, 0)
    pool.add(pat, 48, 0, TN_D2, 0, EFFECT_SYNC, 0)
    # Kick
    for r in kick_bar_a():
        pool.add(pat, r, 1, TN_KICK, INST_KICK)
    for r in kick_bar_b():
        pool.add(pat, r + 16, 1, TN_KICK, INST_KICK)
    for r in kick_bar_a():
        pool.add(pat, r + 32, 1, TN_KICK, INST_KICK)
    for r in kick_bar_b():
        pool.add(pat, r + 48, 1, TN_KICK, INST_KICK)
    # Snare
    for bar_offset in [0, 16, 32, 48]:
        for r in snare_bar():
            pool.add(pat, r + bar_offset, 2, TN_SNARE, INST_SNARE)
    # Hi-hat
    for bar_offset in [0, 16, 32, 48]:
        for r in hh_bar():
            pool.add(pat, r + bar_offset, 3, TN_HH, INST_HH)
    # Bass
    for row, note in bass_notes_in_pattern:
        pool.add(pat, row, 4, note, INST_BASS)
    # Synth stabs
    for row, note in stab_pattern():
        pool.add(pat, row, 5, note, INST_STAB)
    # Guitar riff
    for row, note in guitar_pattern():
        pool.add(pat, row, 6, note, INST_GUITAR)

    # ── Pattern 4: STRINGS (verse + strings on col 7) ────────────────────────
    pat = 4
    # Input track
    pool.add(pat, 0,  0, TN_F2, 0, EFFECT_WAIT, 0)
    pool.add(pat, 8,  0, TN_C2, 0, EFFECT_SYNC, 0)
    pool.add(pat, 16, 0, TN_D2, 0, EFFECT_SYNC, 0)
    pool.add(pat, 32, 0, TN_G2, 0, EFFECT_SYNC, 0)
    pool.add(pat, 40, 0, TN_C2, 0, EFFECT_SYNC, 0)
    pool.add(pat, 48, 0, TN_D2, 0, EFFECT_SYNC, 0)
    # Kick
    for r in kick_bar_a():
        pool.add(pat, r, 1, TN_KICK, INST_KICK)
    for r in kick_bar_b():
        pool.add(pat, r + 16, 1, TN_KICK, INST_KICK)
    for r in kick_bar_a():
        pool.add(pat, r + 32, 1, TN_KICK, INST_KICK)
    for r in kick_bar_b():
        pool.add(pat, r + 48, 1, TN_KICK, INST_KICK)
    # Snare + HH
    for bar_offset in [0, 16, 32, 48]:
        for r in snare_bar():
            pool.add(pat, r + bar_offset, 2, TN_SNARE, INST_SNARE)
        for r in hh_bar():
            pool.add(pat, r + bar_offset, 3, TN_HH, INST_HH)
    # Bass
    for row, note in bass_notes_in_pattern:
        pool.add(pat, row, 4, note, INST_BASS)
    # Stabs
    for row, note in stab_pattern():
        pool.add(pat, row, 5, note, INST_STAB)
    # Strings
    for row, note in strings_pattern():
        pool.add(pat, row, 7, note, INST_STRINGS)

    # ── Pattern 5: VOCAL (verse + vocal melody on col 7) ─────────────────────
    pat = 5
    # Input track
    pool.add(pat, 0,  0, TN_F2, 0, EFFECT_WAIT, 0)
    pool.add(pat, 8,  0, TN_C2, 0, EFFECT_SYNC, 0)
    pool.add(pat, 16, 0, TN_D2, 0, EFFECT_SYNC, 0)
    pool.add(pat, 32, 0, TN_G2, 0, EFFECT_SYNC, 0)
    pool.add(pat, 40, 0, TN_C2, 0, EFFECT_SYNC, 0)
    pool.add(pat, 48, 0, TN_D2, 0, EFFECT_SYNC, 0)
    # Kick
    for r in kick_bar_a():
        pool.add(pat, r, 1, TN_KICK, INST_KICK)
    for r in kick_bar_b():
        pool.add(pat, r + 16, 1, TN_KICK, INST_KICK)
    for r in kick_bar_a():
        pool.add(pat, r + 32, 1, TN_KICK, INST_KICK)
    for r in kick_bar_b():
        pool.add(pat, r + 48, 1, TN_KICK, INST_KICK)
    # Snare + HH
    for bar_offset in [0, 16, 32, 48]:
        for r in snare_bar():
            pool.add(pat, r + bar_offset, 2, TN_SNARE, INST_SNARE)
        for r in hh_bar():
            pool.add(pat, r + bar_offset, 3, TN_HH, INST_HH)
    # Bass
    for row, note in bass_notes_in_pattern:
        pool.add(pat, row, 4, note, INST_BASS)
    # Guitar on col 6
    for row, note in guitar_pattern():
        pool.add(pat, row, 6, note, INST_GUITAR)
    # Vocal melody on col 7
    for row, note in vocal_pattern():
        pool.add(pat, row, 7, note, INST_STRINGS)  # reuse strings instrument slot

    # ── Pattern 6: FULL (everything) ──────────────────────────────────────────
    pat = 6
    # Input track
    pool.add(pat, 0,  0, TN_F2, 0, EFFECT_WAIT, 0)
    pool.add(pat, 8,  0, TN_C2, 0, EFFECT_SYNC, 0)
    pool.add(pat, 16, 0, TN_D2, 0, EFFECT_SYNC, 0)
    pool.add(pat, 32, 0, TN_G2, 0, EFFECT_SYNC, 0)
    pool.add(pat, 40, 0, TN_C2, 0, EFFECT_SYNC, 0)
    pool.add(pat, 48, 0, TN_D2, 0, EFFECT_SYNC, 0)
    # Kick
    for r in kick_bar_a():
        pool.add(pat, r, 1, TN_KICK, INST_KICK)
    for r in kick_bar_b():
        pool.add(pat, r + 16, 1, TN_KICK, INST_KICK)
    for r in kick_bar_a():
        pool.add(pat, r + 32, 1, TN_KICK, INST_KICK)
    for r in kick_bar_b():
        pool.add(pat, r + 48, 1, TN_KICK, INST_KICK)
    # Snare + HH
    for bar_offset in [0, 16, 32, 48]:
        for r in snare_bar():
            pool.add(pat, r + bar_offset, 2, TN_SNARE, INST_SNARE)
        for r in hh_bar():
            pool.add(pat, r + bar_offset, 3, TN_HH, INST_HH)
    # Bass
    for row, note in bass_notes_in_pattern:
        pool.add(pat, row, 4, note, INST_BASS)
    # Stabs
    for row, note in stab_pattern():
        pool.add(pat, row, 5, note, INST_STAB)
    # Guitar
    for row, note in guitar_pattern():
        pool.add(pat, row, 6, note, INST_GUITAR)
    # Strings
    for row, note in strings_pattern():
        pool.add(pat, row, 7, note, INST_STRINGS)

    NUM_PATTERNS = 7

    # ── Build note pool binary ────────────────────────────────────────────────
    pool_bytes, free_head, heads = pool.build()
    total_notes = len(pool.nodes)
    print(f"Total notes in pool: {total_notes} / {MAX_SONG_NOTES}")

    # ── Build pattern structs ─────────────────────────────────────────────────
    # Pattern: noteHead(2) length(1) referenceNote(1) inputNotes[12](48) name(16)
    #          keyChangeMode(1) _pad(3) = 72 bytes

    pattern_data = bytearray(MAX_PATTERNS * 72)

    pattern_names = [
        "INTRO",        # 0
        "KICK+STAB",    # 1
        "VERSE",        # 2
        "GUITAR",       # 3
        "STRINGS",      # 4
        "VOCAL",        # 5
        "FULL",         # 6
    ]

    pattern_lengths = [32, 64, 64, 64, 64, 64, 64]

    for p in range(NUM_PATTERNS):
        off = p * 72

        # noteHead
        head = heads.get(p, NOTE_NULL)
        struct.pack_into('<H', pattern_data, off, head)

        # length
        pattern_data[off + 2] = pattern_lengths[p]

        # referenceNote: D (semitone 2) — zero transposition when performer plays D
        pattern_data[off + 3] = PC_D

        # inputNotes[12] — 4 bytes each (switchMode, switchTarget, transposeAction, transposeValue)
        inp_off = off + 4
        for pc in range(12):
            entry_off = inp_off + pc * 4
            if p == 0:
                # Intro: no performer sync, all default
                pass
            elif pc == PC_F:
                # F: STAY, no transpose
                pattern_data[entry_off] = 0  # STAY
                pattern_data[entry_off + 1] = 0
                pattern_data[entry_off + 2] = 1  # SET transpose
                pattern_data[entry_off + 3] = struct.pack('b', 3)[0]  # F is 3 semitones above D
            elif pc == PC_C:
                # C: STAY
                pattern_data[entry_off] = 0
                pattern_data[entry_off + 1] = 0
                pattern_data[entry_off + 2] = 1  # SET
                pattern_data[entry_off + 3] = struct.pack('b', -2)[0]  # C is 2 below D
            elif pc == PC_D:
                # D: STAY, reference note = 0 transpose
                pattern_data[entry_off] = 0
                pattern_data[entry_off + 1] = 0
                pattern_data[entry_off + 2] = 1  # SET
                pattern_data[entry_off + 3] = 0  # zero
            elif pc == PC_G:
                # G: STAY
                pattern_data[entry_off] = 0
                pattern_data[entry_off + 1] = 0
                pattern_data[entry_off + 2] = 1  # SET
                pattern_data[entry_off + 3] = struct.pack('b', 5)[0]  # G is 5 above D
            elif pc == PC_A and p > 0:
                # A: advance to next pattern
                next_pat = min(p + 1, NUM_PATTERNS - 1)
                pattern_data[entry_off] = 2  # TOP (jump to top of target)
                pattern_data[entry_off + 1] = next_pat
                pattern_data[entry_off + 2] = 0  # KEEP
                pattern_data[entry_off + 3] = 0
            elif pc == PC_E and p > 0:
                # E: go back to previous pattern
                prev_pat = max(p - 1, 0)
                pattern_data[entry_off] = 2  # TOP
                pattern_data[entry_off + 1] = prev_pat
                pattern_data[entry_off + 2] = 0  # KEEP
                pattern_data[entry_off + 3] = 0

        # name (16 bytes, null-terminated)
        name = pattern_names[p] if p < len(pattern_names) else ""
        name_bytes = name.encode('ascii')[:15] + b'\x00'
        name_off = off + 52
        pattern_data[name_off:name_off + len(name_bytes)] = name_bytes

        # keyChangeMode (0 = SAME_POS)
        pattern_data[off + 68] = 0
        # _pad[3] = 0 (already zeroed)

    # ── Build Song struct ─────────────────────────────────────────────────────
    song = bytearray()

    # notePool (16384 bytes)
    song.extend(pool_bytes)

    # noteFreeHead (uint16)
    song.extend(struct.pack('<H', free_head))

    # patterns (32 * 72 = 2304 bytes)
    song.extend(pattern_data)

    # numPatterns (uint8)
    song.append(NUM_PATTERNS)

    # startPattern (uint8)
    song.append(0)

    # bpm (uint16) — 133 BPM
    song.extend(struct.pack('<H', 133))

    # minBPM (uint16)
    song.extend(struct.pack('<H', 100))

    # maxBPM (uint16)
    song.extend(struct.pack('<H', 170))

    # speed (uint8) — 6 ticks/row (4 rows per beat at this BPM)
    song.append(6)

    # name (32 bytes)
    name = b'Blue Monday\x00'
    song.extend(name.ljust(32, b'\x00'))

    # initInstrument[16] — instrument to pre-load on each MIDI channel
    # Channel indices 0-15 (0-based); 0 = skip
    init_inst = [0] * 16
    # Drums are on ch10 (index 9) — but drums don't need program change typically
    # Set up the melodic channels:
    # The user will fix these, but let's set sensible defaults
    init_inst[0] = INST_STAB     # ch1: synth stab
    init_inst[1] = INST_STAB     # ch2: synth stab 2
    init_inst[2] = INST_BASS     # ch3: bass
    init_inst[3] = INST_GUITAR   # ch4: guitar
    init_inst[4] = INST_STRINGS  # ch5: strings
    init_inst[9] = INST_KICK     # ch10: drums (kick, but whole kit)
    song.extend(bytes(init_inst))

    # midiInChannel (0 = ANY)
    song.append(0)

    # midiInNoteMin — accept bass range
    song.append(24)  # C1

    # midiInNoteMax
    song.append(72)  # C5

    # performerMask — bits 0-3 for MIDI channels 1-4
    song.append(0x00)

    # _songPad[3]
    song.extend(b'\x00\x00\x00')

    print(f"Song struct size: {len(song)} bytes (expected 18754)")

    # ── File header ───────────────────────────────────────────────────────────
    header = struct.pack('<IB3s', SONG_FILE_MAGIC, SONG_FILE_VERSION, b'\x00\x00\x00')

    return header + bytes(song)


# ── Main ──────────────────────────────────────────────────────────────────────

if __name__ == '__main__':
    outfile = sys.argv[1] if len(sys.argv) > 1 else 'Blue_Monday.mgt'
    data = build_song()
    with open(outfile, 'wb') as f:
        f.write(data)
    print(f"Wrote {len(data)} bytes to {outfile}")
    print()
    print("Instrument mapping (assign your sounds to these slots):")
    print(f"  {INST_KICK:>2} = Bass Drum")
    print(f"  {INST_SNARE:>2} = Snare / Clap")
    print(f"  {INST_HH:>2} = Hi-Hat Closed")
    print(f"  {INST_BASS:>2} = Bass Synth (the sequenced bass line)")
    print(f"  {INST_STAB:>2} = Synth Stab (the iconic melody)")
    print(f"  {INST_GUITAR:>2} = Guitar / Synth Pad")
    print(f"  {INST_STRINGS:>2} = Strings / Lead / Vocal melody")
    print()
    print("Performer plays bass: F, C, D, G to drive tempo")
    print("Play A to advance to next section, E to go back")
    print()
    print("Patterns:")
    print("  0: INTRO     — kick only, auto-advances")
    print("  1: KICK+STAB — kick + synth melody, performer sync starts")
    print("  2: VERSE     — full drums + bass + stabs")
    print("  3: GUITAR    — adds guitar riff")
    print("  4: STRINGS   — adds strings melody")
    print("  5: VOCAL     — guitar + vocal melody")
    print("  6: FULL      — everything")
