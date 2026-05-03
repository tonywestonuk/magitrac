#!/usr/bin/env python3
"""decode_midi.py — Decode a Standard MIDI File and print a human-readable analysis.

No external dependencies — parses raw SMF binary format.

Usage:
    python3 decode_midi.py <file.mid>
"""

import struct
import sys
from collections import defaultdict

# ── MIDI constants ────────────────────────────────────────────────────────────

NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']

GM_DRUMS = {
    35: 'Kick 2', 36: 'Kick 1', 37: 'Side Stick', 38: 'Snare 1',
    39: 'Clap', 40: 'Snare 2', 41: 'Lo Tom 2', 42: 'HH Closed',
    43: 'Lo Tom 1', 44: 'HH Pedal', 45: 'Mid Tom 2', 46: 'HH Open',
    47: 'Mid Tom 1', 48: 'Hi Tom 2', 49: 'Crash 1', 50: 'Hi Tom 1',
    51: 'Ride 1', 52: 'China', 53: 'Ride Bell', 54: 'Tambourine',
    55: 'Splash', 56: 'Cowbell', 57: 'Crash 2', 58: 'Vibraslap',
    59: 'Ride 2', 60: 'Hi Bongo', 61: 'Lo Bongo', 62: 'Mute Conga',
    63: 'Open Conga', 64: 'Lo Conga', 65: 'Hi Timbale', 66: 'Lo Timbale',
}

def note_name(midi_note):
    """Convert MIDI note number to name like 'C4', 'F#2'."""
    octave = (midi_note // 12) - 1
    return f"{NOTE_NAMES[midi_note % 12]}{octave}"

# ── Variable-length quantity ──────────────────────────────────────────────────

def read_vlq(data, pos):
    """Read a MIDI variable-length quantity. Returns (value, new_pos)."""
    value = 0
    while True:
        b = data[pos]
        pos += 1
        value = (value << 7) | (b & 0x7F)
        if not (b & 0x80):
            break
    return value, pos

# ── Track parser ──────────────────────────────────────────────────────────────

def parse_track(data, offset):
    """Parse one MTrk chunk. Returns (events, next_offset)."""
    magic = data[offset:offset+4]
    if magic != b'MTrk':
        raise ValueError(f"Expected MTrk at offset {offset}, got {magic!r}")
    length = struct.unpack('>I', data[offset+4:offset+8])[0]
    track_data = data[offset+8:offset+8+length]

    events = []
    pos = 0
    abs_tick = 0
    running_status = 0

    while pos < len(track_data):
        delta, pos = read_vlq(track_data, pos)
        abs_tick += delta

        byte = track_data[pos]

        # Meta event
        if byte == 0xFF:
            pos += 1
            meta_type = track_data[pos]; pos += 1
            meta_len, pos = read_vlq(track_data, pos)
            meta_data = track_data[pos:pos+meta_len]; pos += meta_len
            events.append({
                'tick': abs_tick, 'type': 'meta',
                'meta_type': meta_type, 'data': meta_data
            })
            continue

        # SysEx
        if byte in (0xF0, 0xF7):
            pos += 1
            sysex_len, pos = read_vlq(track_data, pos)
            pos += sysex_len
            continue

        # Channel message
        if byte & 0x80:
            status = byte
            pos += 1
            running_status = status
        else:
            status = running_status

        msg_type = status & 0xF0
        channel = status & 0x0F

        if msg_type in (0x80, 0x90, 0xA0, 0xB0, 0xE0):
            # Two data bytes
            d1 = track_data[pos]; pos += 1
            d2 = track_data[pos]; pos += 1
            events.append({
                'tick': abs_tick, 'type': 'channel', 'status': msg_type,
                'channel': channel, 'data1': d1, 'data2': d2
            })
        elif msg_type in (0xC0, 0xD0):
            # One data byte
            d1 = track_data[pos]; pos += 1
            events.append({
                'tick': abs_tick, 'type': 'channel', 'status': msg_type,
                'channel': channel, 'data1': d1, 'data2': 0
            })

    return events, offset + 8 + length

# ── Main ──────────────────────────────────────────────────────────────────────

def decode_midi(filepath):
    with open(filepath, 'rb') as f:
        data = f.read()

    # Header
    if data[:4] != b'MThd':
        print("ERROR: not a MIDI file"); return
    hdr_len = struct.unpack('>I', data[4:8])[0]
    fmt, num_tracks, division = struct.unpack('>HHH', data[8:14])

    ticks_per_beat = division if not (division & 0x8000) else None
    print(f"Format: {fmt}  Tracks: {num_tracks}  Division: {division}", end='')
    if ticks_per_beat:
        print(f" ({ticks_per_beat} ticks/beat)")
    else:
        print(f" (SMPTE)")

    # Parse all tracks
    offset = 8 + hdr_len
    all_tracks = []
    for i in range(num_tracks):
        events, offset = parse_track(data, offset)
        all_tracks.append(events)

    # ── Tempo map ─────────────────────────────────────────────────────────────
    tempos = []
    for track in all_tracks:
        for ev in track:
            if ev['type'] == 'meta' and ev['meta_type'] == 0x51:
                us_per_beat = int.from_bytes(ev['data'], 'big')
                bpm = 60_000_000 / us_per_beat
                tempos.append((ev['tick'], bpm))
    if tempos:
        print(f"\nTempo changes:")
        for tick, bpm in tempos:
            print(f"  tick {tick:>6}: {bpm:.1f} BPM")
    else:
        print("\nNo tempo events (default 120 BPM)")

    # ── Time signatures ───────────────────────────────────────────────────────
    for track in all_tracks:
        for ev in track:
            if ev['type'] == 'meta' and ev['meta_type'] == 0x58:
                nn, dd, cc, bb = ev['data'][0], ev['data'][1], ev['data'][2], ev['data'][3]
                print(f"  Time sig: {nn}/{2**dd} at tick {ev['tick']}")

    # ── Track names ───────────────────────────────────────────────────────────
    print(f"\n{'='*70}")
    for i, track in enumerate(all_tracks):
        name = ''
        for ev in track:
            if ev['type'] == 'meta' and ev['meta_type'] == 0x03:
                name = ev['data'].decode('latin-1', errors='replace')
                break

        # Collect notes per channel
        ch_notes = defaultdict(list)
        ch_programs = defaultdict(set)
        ch_banks = defaultdict(lambda: [0, 0])  # [MSB, LSB]
        ch_ccs = defaultdict(lambda: defaultdict(set))

        for ev in track:
            if ev['type'] != 'channel': continue
            ch = ev['channel']
            if ev['status'] == 0x90 and ev['data2'] > 0:
                ch_notes[ch].append((ev['tick'], ev['data1'], ev['data2']))
            elif ev['status'] == 0xC0:
                ch_programs[ch].add(ev['data1'])
            elif ev['status'] == 0xB0:
                cc_num = ev['data1']
                cc_val = ev['data2']
                ch_ccs[ch][cc_num].add(cc_val)
                if cc_num == 0:
                    ch_banks[ch][0] = cc_val
                elif cc_num == 32:
                    ch_banks[ch][1] = cc_val

        if not ch_notes and not name:
            continue

        print(f"\nTrack {i}: {name or '(unnamed)'}")

        for ch in sorted(ch_notes.keys()):
            notes = ch_notes[ch]
            pitches = [n[1] for n in notes]
            lo, hi = min(pitches), max(pitches)
            progs = ch_programs.get(ch, set())
            bank = ch_banks.get(ch, [0, 0])

            if ch == 9:
                # Drum channel
                drum_hits = defaultdict(int)
                for _, note, _ in notes:
                    drum_hits[note] += 1
                print(f"  Ch {ch+1:>2} (DRUMS): {len(notes)} notes")
                for note in sorted(drum_hits.keys()):
                    dname = GM_DRUMS.get(note, f'Note {note}')
                    print(f"         {note:>3} {dname:<16} x{drum_hits[note]}")
            else:
                print(f"  Ch {ch+1:>2}: {len(notes)} notes  "
                      f"range {note_name(lo)}-{note_name(hi)}  "
                      f"bank={bank[0]}/{bank[1]}  "
                      f"prog={progs or 'none'}")

                # Show pitch class distribution
                pc_count = defaultdict(int)
                for _, note, _ in notes:
                    pc_count[note % 12] += 1
                pcs = ', '.join(f"{NOTE_NAMES[pc]}:{cnt}"
                                for pc, cnt in sorted(pc_count.items(), key=lambda x: -x[1]))
                print(f"         pitch classes: {pcs}")

    # ── Detailed note dump (first 4 bars) ─────────────────────────────────────
    if ticks_per_beat:
        print(f"\n{'='*70}")
        print(f"First 4 bars (tick 0–{ticks_per_beat * 16}):")
        print(f"{'='*70}")
        bar_ticks = ticks_per_beat * 4
        limit = ticks_per_beat * 16  # 4 bars

        for i, track in enumerate(all_tracks):
            name = ''
            for ev in track:
                if ev['type'] == 'meta' and ev['meta_type'] == 0x03:
                    name = ev['data'].decode('latin-1', errors='replace')
                    break

            notes_in_range = []
            for ev in track:
                if (ev['type'] == 'channel' and ev['status'] == 0x90
                        and ev['data2'] > 0 and ev['tick'] < limit):
                    notes_in_range.append(ev)

            if not notes_in_range: continue

            print(f"\n  Track {i} ({name or 'unnamed'}):")
            for ev in notes_in_range:
                ch = ev['channel']
                tick = ev['tick']
                bar = tick // bar_ticks + 1
                beat_in_bar = (tick % bar_ticks) / ticks_per_beat + 1
                nn = ev['data1']
                if ch == 9:
                    nstr = GM_DRUMS.get(nn, f'#{nn}')
                else:
                    nstr = note_name(nn)
                print(f"    bar {bar} beat {beat_in_bar:5.2f}  ch{ch+1:>2}  "
                      f"{nstr:<12} vel={ev['data2']}")

    # ── Full structure: bars and what's playing ───────────────────────────────
    if ticks_per_beat:
        print(f"\n{'='*70}")
        print("Song structure (which channels are active per 4-bar section):")
        print(f"{'='*70}")

        # Merge all note-on events with track info
        all_notes = []
        for i, track in enumerate(all_tracks):
            for ev in track:
                if ev['type'] == 'channel' and ev['status'] == 0x90 and ev['data2'] > 0:
                    all_notes.append((ev['tick'], ev['channel'], i))

        if all_notes:
            max_tick = max(n[0] for n in all_notes)
            section_ticks = ticks_per_beat * 16  # 4 bars per section
            num_sections = (max_tick // section_ticks) + 1

            for s in range(num_sections):
                t0 = s * section_ticks
                t1 = t0 + section_ticks
                active = defaultdict(int)
                for tick, ch, trk in all_notes:
                    if t0 <= tick < t1:
                        active[ch] += 1

                bar_start = s * 4 + 1
                bar_end = bar_start + 3
                chans = ' '.join(f"ch{ch+1}({cnt})" for ch, cnt in sorted(active.items()))
                print(f"  Bars {bar_start:>3}-{bar_end:>3}: {chans}")


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <file.mid>")
        sys.exit(1)
    decode_midi(sys.argv[1])
