#!/bin/sh
# Rebuild and rerun the fumble-injection experiment in paper/fumble-recall.md.
#
#   paper/eval/run.sh path/to/song.mgt
#
# Builds the replay harness twice — against the sequencer as it is in this
# checkout (last-key recall) and against the one tagged fumble-recall-baseline
# (commit 46ab5f3, the earlier first-key cooldown) — then runs fumble_eval.py.
# Needs g++, python3 and the repository's tags (git fetch --tags); no hardware.  The song used in the paper is from the author's live set and is not
# distributed; the key list at the top of fumble_eval.py belongs to that song,
# so change both to try another.
set -e
[ -n "$1" ] || { echo "usage: $0 song.mgt" >&2; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
SRV="$HERE/../../magitrac_server_s3"
LIB="$HERE/../../magitrac_lib/src"
B="$HERE/build"
mkdir -p "$B"
# The baseline is built against ITS OWN headers, not this checkout's: the
# sequencer's neighbours (organ, sample player) have moved on since the tag.
rm -rf "$B/base" && mkdir -p "$B/base"
ROOT=$(git -C "$HERE" rev-parse --show-toplevel)
git -C "$ROOT" archive -o "$B/base.tar" fumble-recall-baseline magitrac_server_s3 magitrac_lib/src
tar -x -f "$B/base.tar" -C "$B/base"
BSRV="$B/base/magitrac_server_s3"
BLIB="$B/base/magitrac_lib/src"
OPTS="-std=c++11 -w -DUNIT_TEST -I $HERE -I $HERE/mocks"
g++ $OPTS -I "$SRV/test_mocks"  -I "$SRV"  -I "$LIB"  -DMIDI_PLAYER_CPP="\"$SRV/midi_player.cpp\""   "$HERE/replay.cpp" -o "$B/sim_recall"
# (the host mocks — Arduino.h, the MIDI-capturing UART — are harness plumbing,
#  newer than the tag: they come from this checkout, searched last.)
rm -rf "$BSRV/test_mocks"
g++ $OPTS -I "$SRV/test_mocks"  -I "$BSRV" -I "$BLIB" -I "$SRV" -DMIDI_PLAYER_CPP="\"$BSRV/midi_player.cpp\""  "$HERE/replay.cpp" -o "$B/sim_first"
python3 "$HERE/fumble_eval.py" "$B" "$1"
