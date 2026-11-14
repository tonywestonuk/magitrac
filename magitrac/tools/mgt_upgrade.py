#!/usr/bin/env python3
"""
mgt_upgrade.py — bring old MagiTrac song files (.mgt) up to the current format.

The firmware now loads only the current song-file version (v20) and refuses
anything older ("LOAD FAILED - BAD FILE").  This tool scans a folder, finds
every song file in an older version, and rewrites it as v20.

    python3 mgt_upgrade.py /Volumes/SDCARD              # convert everything
    python3 mgt_upgrade.py /Volumes/SDCARD --dry-run    # just report, change nothing
    python3 mgt_upgrade.py /Volumes/SDCARD/songs --no-backup

What it does
  * Looks at every *.mgt under the folder (sub-folders included).
  * Files that are not songs (e.g. instruments.mgt) and songs already at v20
    are left alone.
  * Each older song is converted, the original is copied into a backup folder
    (_mgt_backup_<date>_<time> inside the folder you gave), and the converted
    file replaces it.  The replacement is written to a temporary file first and
    only then swapped in, so an interrupted run cannot leave a half-written song.

Supported source versions: 14, 15, 16, 17, 18, 19.
(v11 and v13 are not supported — the firmware's own readers for those were
faulty, so there is nothing trustworthy to reproduce.)

The conversion is a byte-for-byte port of the readers that used to live in the
firmware (magitrac_lib/src/SongMigration.h, removed 2026-11; last present in
git commit 46ab5f3), followed by the firmware's own v20 writer.  It was checked
against a build of that original C++ code on real v17/v18/v19 songs and on
randomised files of every supported version.

If the song format changes again (SONG_FILE_VERSION > 20), this tool must be
extended: the layout numbers below describe v20 exactly.

Python 3.7+, standard library only.
"""

import argparse
import datetime
import os
import shutil
import struct
import sys

# ── v20 layout (magitrac_lib/src/TrackerData.h) ──────────────────────────────
# Sizes / offsets are those of the firmware's structs as the compiler lays them
# out (they are not packed: there is one alignment byte before transposeChMask
# and one at the very end of the Song).
MAGIC            = 0x4D414754          # "MAGT", little-endian uint32
CURRENT_VERSION  = 20
HEADER_SIZE      = 8                   # magic(4) version(1) pad(3)

MAX_PATTERNS     = 50
MAX_COLUMNS      = 21
MAX_SONG_NOTES   = 4000
NOTE_NULL        = 0xFFFF

PATTERN_SIZE     = 72                  # noteHead(2) length ref inputNotes[12×4] name[16] keyChange nav pad[2]
PATTERN_V15_SIZE = 232                 # the same 72 bytes + ColumnSettings[8] embedded per pattern
COLUMN_SIZE      = 20
NOTE_SIZE        = 7                   # pattern row col note velocity effect param

PATTERNS_BYTES   = MAX_PATTERNS * PATTERN_SIZE        # 3600
COLUMNS_BYTES    = MAX_COLUMNS * COLUMN_SIZE          # 420

# Song "tail": everything from numPatterns to the end of the struct.
TAIL_SIZE        = 174
TAIL_HEAD_TO_PERFORMER_MASK = 45       # numPatterns … performerMask inclusive
TAIL_OFF_TRANSPOSE_MASK     = 46       # uint16 (one alignment byte sits at 45)
TAIL_OFF_PERF_PADS          = 48
PERF_PADS_BYTES             = 104
TAIL_OFF_ORGAN              = 152      # OrganPatch (20 bytes), new in v20
ORGAN_BYTES                 = 20
DEFAULT_TRANSPOSE_MASK      = 0xFDFF   # every MIDI channel follows transpose except ch 10 (drums)

V16_COLUMNS = 8                        # column count when v15/v16 files were written
V17_COLUMNS = 9

SUPPORTED_OLD_VERSIONS = (14, 15, 16, 17, 18, 19)


class MgtError(Exception):
    """The file could not be converted; it is left untouched."""


class _Reader:
    """Sequential reader that fails loudly on a short file."""

    def __init__(self, data, pos):
        self.data = data
        self.pos = pos

    def take(self, n, what):
        if self.pos + n > len(self.data):
            raise MgtError("file is too short (ran out while reading %s)" % what)
        chunk = self.data[self.pos:self.pos + n]
        self.pos += n
        return chunk


def read_header(data):
    """Return (is_song, version).  is_song is False for non-song .mgt files."""
    if len(data) < HEADER_SIZE:
        return False, None
    magic, version = struct.unpack_from("<IB", data, 0)
    return magic == MAGIC, version


def _read_notes(r):
    """Read noteCount + notes.  Returns a list of 7-byte records in file order."""
    (count,) = struct.unpack("<H", r.take(2, "the note count"))
    if count > MAX_SONG_NOTES:
        raise MgtError("note count %d is more than a song can hold (%d)" % (count, MAX_SONG_NOTES))
    notes = []
    for i in range(count):
        rec = r.take(NOTE_SIZE, "note %d of %d" % (i + 1, count))
        if rec[0] >= MAX_PATTERNS:
            raise MgtError("note %d refers to block %d (only %d exist)" % (i + 1, rec[0], MAX_PATTERNS))
        notes.append(rec)
    return notes


def convert(data):
    """Convert one old song file (bytes) to v20 (bytes).

    Returns (new_bytes, info) where info is a dict with 'version', 'notes' and
    'trailing' (bytes after the notes that the firmware also ignored).
    Raises MgtError if the file cannot be converted.
    """
    is_song, version = read_header(data)
    if not is_song:
        raise MgtError("not a MagiTrac song file")
    if version == CURRENT_VERSION:
        raise MgtError("already version %d" % CURRENT_VERSION)
    if version not in SUPPORTED_OLD_VERSIONS:
        raise MgtError("version %d is not supported by this tool" % version)

    r = _Reader(data, HEADER_SIZE)
    patterns = bytearray(PATTERNS_BYTES)
    columns = bytearray(COLUMNS_BYTES)      # columns the old file did not have stay zero
    tail = bytearray(TAIL_SIZE)             # fields the old file did not have stay zero

    # 1. Patterns (+ columns, where they lived inside the pattern)
    if version in (14, 15):
        # Each pattern carried its own 8 column settings.  Column settings are
        # per song now: take block 0's and drop the rest.
        for p in range(MAX_PATTERNS):
            old = r.take(PATTERN_V15_SIZE, "block %d" % p)
            patterns[p * PATTERN_SIZE:(p + 1) * PATTERN_SIZE] = old[:PATTERN_SIZE]
            if p == 0:
                columns[:V16_COLUMNS * COLUMN_SIZE] = old[PATTERN_SIZE:PATTERN_SIZE + V16_COLUMNS * COLUMN_SIZE]
    else:
        patterns[:] = r.take(PATTERNS_BYTES, "the blocks")
        # 2. Per-song column settings: 8, then 9, then today's 21.
        ncols = {16: V16_COLUMNS, 17: V17_COLUMNS}.get(version, MAX_COLUMNS)
        columns[:ncols * COLUMN_SIZE] = r.take(ncols * COLUMN_SIZE, "the column settings")

    # 3. Song tail
    if version == 19:
        # v19 is v20 without the organ patch: everything up to it is in place,
        # followed by the old pad byte and one alignment byte.
        tail[:TAIL_OFF_ORGAN] = r.take(TAIL_OFF_ORGAN, "the song settings")
        r.take(2, "the song settings padding")
    else:
        tail[:TAIL_HEAD_TO_PERFORMER_MASK] = r.take(TAIL_HEAD_TO_PERFORMER_MASK, "the song settings")
        if version >= 15:
            tail[TAIL_OFF_PERF_PADS:TAIL_OFF_PERF_PADS + PERF_PADS_BYTES] = \
                r.take(PERF_PADS_BYTES, "the performance pads")
        r.take(3, "the song settings padding")
        struct.pack_into("<H", tail, TAIL_OFF_TRANSPOSE_MASK, DEFAULT_TRANSPOSE_MASK)

    # 4. Notes
    notes = _read_notes(r)
    trailing = len(data) - r.pos

    # 5. Write v20.  Each block's noteHead field is not used on load, but the
    # firmware wrote the index of that block's first note there (0xFFFF when
    # it has none) and so do we; notes go out block by block, keeping the
    # order they had within each block.
    by_pattern = [[] for _ in range(MAX_PATTERNS)]
    first_index = [NOTE_NULL] * MAX_PATTERNS
    for i, rec in enumerate(notes):
        p = rec[0]
        if first_index[p] == NOTE_NULL:
            first_index[p] = i
        by_pattern[p].append(rec)
    for p in range(MAX_PATTERNS):
        struct.pack_into("<H", patterns, p * PATTERN_SIZE, first_index[p])

    out = bytearray()
    out += struct.pack("<IB3x", MAGIC, CURRENT_VERSION)
    out += patterns
    out += columns
    out += tail
    out += struct.pack("<H", len(notes))
    for p in range(MAX_PATTERNS):
        for rec in by_pattern[p]:
            out += rec
    return bytes(out), {"version": version, "notes": len(notes), "trailing": trailing}


def check_current(data):
    """Sanity-check a v20 file's structure.  Returns the note count or raises MgtError."""
    is_song, version = read_header(data)
    if not is_song or version != CURRENT_VERSION:
        raise MgtError("not a version %d song" % CURRENT_VERSION)
    r = _Reader(data, HEADER_SIZE)
    r.take(PATTERNS_BYTES, "the blocks")
    r.take(COLUMNS_BYTES, "the column settings")
    r.take(TAIL_SIZE, "the song settings")
    return len(_read_notes(r))


# ── Folder scan ──────────────────────────────────────────────────────────────

def find_mgt_files(root, backup_prefix="_mgt_backup_"):
    for dirpath, dirnames, filenames in os.walk(root):
        # never descend into our own backup folders, or hidden folders
        dirnames[:] = sorted(d for d in dirnames
                             if not d.startswith(backup_prefix) and not d.startswith("."))
        for name in sorted(filenames):
            if name.startswith("."):                 # macOS "._Name.mgt" sidecars etc.
                continue
            if name.lower().endswith(".mgt"):
                yield os.path.join(dirpath, name)


def upgrade_folder(root, dry_run=False, backup=True, out=sys.stdout):
    root = os.path.abspath(root)
    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    backup_root = os.path.join(root, "_mgt_backup_" + stamp)
    counts = {"converted": 0, "current": 0, "not_song": 0, "failed": 0}

    def say(status, path, detail=""):
        rel = os.path.relpath(path, root)
        out.write("  %-12s %s%s\n" % (status, rel, ("  — " + detail) if detail else ""))

    out.write("Scanning %s%s\n" % (root, "  (dry run — nothing will be changed)" if dry_run else ""))
    for path in find_mgt_files(root):
        try:
            with open(path, "rb") as f:
                data = f.read()
        except OSError as e:
            counts["failed"] += 1
            say("FAILED", path, "could not read: %s" % e)
            continue

        is_song, version = read_header(data)
        if not is_song:
            counts["not_song"] += 1
            say("skipped", path, "not a song file")
            continue
        if version == CURRENT_VERSION:
            counts["current"] += 1
            try:
                check_current(data)
                say("ok (v%d)" % version, path)
            except MgtError as e:
                say("ok (v%d)" % version, path, "WARNING: looks damaged: %s" % e)
            continue

        try:
            new_data, info = convert(data)
            check_current(new_data)                  # the result must itself be a valid v20 file
        except MgtError as e:
            counts["failed"] += 1
            say("FAILED", path, "v%s: %s" % (version, e))
            continue

        detail = "v%d -> v%d, %d notes" % (version, CURRENT_VERSION, info["notes"])
        if info["trailing"]:
            detail += ", %d stray bytes at the end ignored" % info["trailing"]
        if dry_run:
            counts["converted"] += 1
            say("would convert", path, detail)
            continue

        try:
            if backup:
                dest = os.path.join(backup_root, os.path.relpath(path, root))
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                shutil.copy2(path, dest)
            tmp = path + ".upgrading"
            with open(tmp, "wb") as f:
                f.write(new_data)
                f.flush()
                os.fsync(f.fileno())
            os.replace(tmp, path)
            with open(path, "rb") as f:              # read back: is it what we meant to write?
                if f.read() != new_data:
                    raise OSError("read-back does not match what was written")
        except OSError as e:
            counts["failed"] += 1
            say("FAILED", path, "could not write: %s" % e)
            continue
        counts["converted"] += 1
        say("converted", path, detail)

    out.write("\n%s %d, already current %d, not songs %d, failed %d\n" % (
        "Would convert" if dry_run else "Converted",
        counts["converted"], counts["current"], counts["not_song"], counts["failed"]))
    if backup and not dry_run and counts["converted"]:
        out.write("Originals saved in %s\n" % backup_root)
    return counts


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Convert old MagiTrac song files (.mgt) to the current format (v%d)." % CURRENT_VERSION)
    ap.add_argument("folder", help="folder to scan (sub-folders included), e.g. the SD card or its songs folder")
    ap.add_argument("--dry-run", action="store_true", help="report what would be done; change nothing")
    ap.add_argument("--no-backup", action="store_true",
                    help="do not keep a copy of each original (by default they go into a _mgt_backup_<time> folder)")
    args = ap.parse_args(argv)

    if not os.path.isdir(args.folder):
        ap.error("not a folder: %s" % args.folder)
    counts = upgrade_folder(args.folder, dry_run=args.dry_run, backup=not args.no_backup)
    return 1 if counts["failed"] else 0


if __name__ == "__main__":
    sys.exit(main())
