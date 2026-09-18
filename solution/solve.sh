[ -n "${LSM_SOLVE_DIR:-}" ] || exec bash -c 'd=$(cd "$(dirname "$1")" && pwd) && t=$(mktemp) && tr -d "\r" < "$1" > "$t" && LSM_SOLVE_DIR=$d exec bash "$t" "${@:2}"' _ "$0" "$@" #
# Reference solution: apply compaction.patch, rebuild, and run bench.py at
# scale 0.5 as a smoke test.
#
# No shebang on purpose. Some uploads arrive with CRLF endings, and then
# "#!/bin/bash\r" isn't a real interpreter. Line 1 instead re-runs a copy
# with the CRs stripped, under bash, with LSM_SOLVE_DIR set.
#
# The patch (db/version_set.{h,cc}, db/db_impl.cc, db/version_edit.h; no
# format changes):
#
#  1. Gets and scans no longer trigger seek compactions. On this workload
#     they were the biggest waste: ~15x WA with them, ~6x without.
#  2. Size compactions at level >= 1 pick the file with the least next-level
#     overlap per byte (PickCheapestFile) instead of round-robin.
#  3. Each compaction output remembers what fraction of it is "fresh", i.e.
#     written since the last compaction into that level. Hot files will get
#     rewritten by the next merge anyway, so PickCheapestFile prefers cold
#     ones. This lives only in memory.
#  4. ShouldStopBefore cuts an output at a grandparent file boundary once it
#     is at least a quarter of max_file_size, so later it overlaps whole
#     files instead of dragging half-overlapped neighbours along.
#
# On the verifier's seed this measures WA 4.93 against the pristine 14.10
# (write_amp_ratio 0.350), L0 depth 4, peak space 1.15x, phase-F reads 1.00x.
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
