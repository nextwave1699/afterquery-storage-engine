#!/bin/bash
# Produces the on-disk compatibility fixtures with the pristine engine.
# Runs inside the toolchain image (see seal.sh).
#
# usage: gen_fixtures.sh <pristine-tree> <harness-dir> <out-dir>
#
# Every fixture is a database directory plus, in fixtures.json, the
# parameters that let `lsmbench check` reconstruct its expected contents:
#   clean         phases A..E at FIXTURE_SCALE, closed cleanly (the last
#                 memtable is still in the WAL, as always with LevelDB)
#   live-wal      killed between two batches: recovery has to replay the WAL
#   torn-wal      like live-wal with the last 5 bytes of the WAL cut off: the
#                 last record must be dropped without complaint
#   reuse-logs    reopened twice with Options::reuse_logs, which appends to
#                 the existing MANIFEST and WAL instead of starting new ones
#   plain-tables  written without a filter policy, 16 KB blocks, restart
#                 interval 64
set -euo pipefail
TREE=$1; HS=$2; OUT=$3
SCALE=0.05
B=/tmp/fxbuild
mkdir -p $B && cd $B
cmake -DCMAKE_BUILD_TYPE=Release -DLEVELDB_BUILD_TESTS=OFF -DLEVELDB_BUILD_BENCHMARKS=OFF -DLEVELDB_INSTALL=OFF "$TREE" > cmake.log
make -j4 leveldb > make.log
g++ -std=c++17 -O2 -fno-rtti -I"$TREE/include" -I"$HS" "$HS/lsmbench.cc" -o $B/lsmbench libleveldb.a -lpthread
L=$B/lsmbench

batches() { $L describe --seed "$1" --scale "$SCALE" | python3 -c 'import json,sys; print(json.load(sys.stdin)["batches"])'; }

entries=()
add() { entries+=("{\"name\": \"$1\", \"dir\": \"$1\", \"seed\": $2, \"scale\": $SCALE, \"batches\": $3, \"extra\": []}"); }

# clean
seed=730001; $L run --db "$OUT/clean" --seed $seed --scale $SCALE --quiet 2>/dev/null
add clean $seed "$(batches $seed)"

# live-wal: SIGKILL before batch N+1 => exactly N acknowledged
seed=730002; n=$(( $(batches $seed) * 8 / 10 ))
set +e; $L run --db "$OUT/live-wal" --seed $seed --scale $SCALE --quiet --arm-at-batch $n --crash op:1 --ack "$OUT/live-wal.ack" 2>/dev/null; set -e
acked=$(cat "$OUT/live-wal.ack"); rm -f "$OUT/live-wal.ack"
[ "$acked" = "$n" ] || { echo "live-wal acked $acked != $n" >&2; exit 1; }
add live-wal $seed "$acked"

# torn-wal
seed=730003; n=$(( $(batches $seed) * 7 / 10 ))
set +e; $L run --db "$OUT/torn-wal" --seed $seed --scale $SCALE --quiet --arm-at-batch $n --crash op:1 --ack "$OUT/torn-wal.ack" 2>/dev/null; set -e
acked=$(cat "$OUT/torn-wal.ack"); rm -f "$OUT/torn-wal.ack"
log=$(ls "$OUT/torn-wal"/*.log | sort -t/ -k2 -n | tail -1)
python3 - "$log" <<'EOF'
import os, sys
p = sys.argv[1]; n = os.path.getsize(p)
with open(p, "r+b") as f: f.truncate(n - 5)
EOF
add torn-wal $seed "$((acked - 1))"

# reuse-logs: run, then two reopen+extend cycles with reuse_logs=true
seed=730004; $L run --db "$OUT/reuse-logs" --seed $seed --scale $SCALE --quiet 2>/dev/null
b=$(batches $seed)
f1=$($L check --db "$OUT/reuse-logs" --seed $seed --scale $SCALE --batches $b --continue 300 --reuse-logs 2>/dev/null | awk '/^FINAL_BATCHES/{print $2}')
f2=$($L check --db "$OUT/reuse-logs" --seed $seed --scale $SCALE --batches $f1 --continue 300 --reuse-logs 2>/dev/null | awk '/^FINAL_BATCHES/{print $2}')
add reuse-logs $seed "$f2"

# plain-tables
seed=730005; $L run --db "$OUT/plain-tables" --seed $seed --scale $SCALE --quiet --plain-tables 2>/dev/null
add plain-tables $seed "$(batches $seed)"

# Every fixture must be readable by the engine that wrote it.
python3 - "$OUT" "${entries[@]}" <<'EOF'
import json, sys
out = sys.argv[1]
entries = [json.loads(e) for e in sys.argv[2:]]
with open(out + "/fixtures.json", "w") as f:
    json.dump(entries, f, indent=1)
EOF
for e in "${entries[@]}"; do
  name=$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['name'])" "$e")
  seed=$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['seed'])" "$e")
  bt=$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['batches'])" "$e")
  cp -r "$OUT/$name" /tmp/fxcheck
  $L check --db /tmp/fxcheck --seed $seed --scale $SCALE --batches $bt --paranoid 2>/dev/null | grep -q '^APPLIED' || { echo "fixture $name does not verify" >&2; exit 1; }
  rm -rf /tmp/fxcheck
  echo "fixture $name: $(du -sh "$OUT/$name" | cut -f1)"
done
rm -f "$OUT"/*/LOCK
