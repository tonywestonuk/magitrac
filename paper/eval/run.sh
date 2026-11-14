#!/bin/sh
# Rebuild and rerun the fumble-injection experiment in paper/fumble-recall.md.
#
#   paper/eval/run.sh path/to/song.mgt
#
# Builds the replay harness twice — against the sequencer as it is in this
# checkout (last-key recall) and against the one at commit 46ab5f3 (the earlier
# first-key cooldown) — then runs fumble_eval.py.  Needs g++ and python3; no
# hardware.  The song used in the paper is from the author's live set and is not
# distributed; the key list at the top of fumble_eval.py belongs to that song,
# so change both to try another.
set -e
[ -n "$1" ] || { echo "usage: $0 song.mgt" >&2; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
SRV="$HERE/../../magitrac_server_s3"
LIB="$HERE/../../magitrac_lib/src"
B="$HERE/build"
mkdir -p "$B"
git -C "$HERE" show 46ab5f3:magitrac_server_s3/midi_player.cpp > "$B/midi_player_first.cpp"
FLAGS="-std=c++11 -w -DUNIT_TEST -I $SRV/test_mocks -I $SRV -I $LIB -I $HERE -I $HERE/mocks"
g++ $FLAGS -DMIDI_PLAYER_CPP="\"$SRV/midi_player.cpp\""        "$HERE/replay.cpp" -o "$B/sim_recall"
g++ $FLAGS -DMIDI_PLAYER_CPP="\"$B/midi_player_first.cpp\""    "$HERE/replay.cpp" -o "$B/sim_first"
python3 "$HERE/fumble_eval.py" "$B" "$1"
