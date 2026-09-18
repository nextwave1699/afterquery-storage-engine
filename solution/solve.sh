[ -n "${LSM_SOLVE_DIR:-}" ] || exec bash -c 'd=$(cd "$(dirname "$1")" && pwd) && t=$(mktemp) && tr -d "\r" < "$1" > "$t" && LSM_SOLVE_DIR=$d exec bash "$t" "${@:2}"' _ "$0" "$@" #
# Reference solution: apply compaction.patch to /app/leveldb, rebuild, and
# run the bundled benchmark at a small scale as a smoke test.
#
# The first line replaces the usual "#!/bin/bash": some upload paths deliver
# this file with CRLF line endings, and "#!/bin/bash<CR>" names an
# interpreter that does not exist.  Without a shebang the shell interprets
# the file itself, line 1 re-executes a carriage-return-free copy under bash
# with LSM_SOLVE_DIR set to this directory, and everything below runs there.
#
# What the patch changes (db/version_set.{h,cc}, db/db_impl.cc,
# db/version_edit.h; the on-disk format is untouched):
#
#   1. Reads no longer schedule compactions.  LevelDB charges every Get and
#      every megabyte scanned to the first table consulted and compacts a
#      table after ~file_size/16KB such "seeks".  That rewrites the table and
#      everything it overlaps one level down without reducing the data, and
#      on this workload it was the largest source of bytes written (the
#      untouched engine writes ~15x the logical bytes; ~6x without it).
#
#   2. Cheapest-file selection for size-triggered compactions at level >= 1
#      (VersionSet::PickCheapestFile) instead of the round-robin pointer:
#      the file whose overlap with the next level, relative to its own size,
#      is smallest rewrites the fewest bytes per byte moved.
#
#   3. Hotness-aware selection.  While compacting, every entry is classified
#      as fresh (sequence number above the one at the start of the previous
#      compaction into the same level) or old, and each output file records
#      the fresh fraction of its bytes (FileMetaData::hotness, memory only).
#      A file that is still receiving updates will be rewritten by the next
#      merge into its level anyway; pushing it down now only moves versions
#      that are about to become garbage, so cold files are preferred (the
#      rewrite ratio is scaled by kHotnessWeight + hotness).
#
#   4. Output files aligned with the grandparent level
#      (Compaction::ShouldStopBefore): once an output has reached a quarter
#      of the target file size it is cut when the next key falls past the
#      end of a grandparent file.  When that output is later compacted down
#      it overlaps whole files only, so no partially overlapped neighbour is
#      rewritten along with it.
#
# With the fixed workload at the verifier's scale the reference measures a
# write amplification of about 5.0 (bar 5.4, untouched engine 15-16.5),
# with level-0 depth 4, peak space ~1.13x and phase-F read bytes ~0.95x of
# the baseline.
set -euo pipefail
HERE="${LSM_SOLVE_DIR}"
TREE="${LSM_TREE:-/app/leveldb}"

cd "${TREE}"
tr -d '\r' < "${HERE}/compaction.patch" | patch -p1 --forward
git -c user.name=solver -c user.email=solver@localhost commit -q -a -m "compaction: no seek compactions, cheapest cold file first, grandparent-aligned outputs" || true

mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DLEVELDB_BUILD_TESTS=OFF -DLEVELDB_BUILD_BENCHMARKS=OFF .. > /dev/null
make -j4 leveldb > /dev/null
cd ..
echo
echo "smoke test (scale 0.5) against the pristine copy:"
python3 /app/bench/bench.py --scale 0.5
