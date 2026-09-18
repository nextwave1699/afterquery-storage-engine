#!/bin/bash
# Rebuilds the sealed archives from the pinned upstream source:
#
#   environment/sealed/leveldb.tar.gz   pristine LevelDB 1.23 tree (+ vendored
#   tests/sealed/leveldb.tar.gz         googletest/benchmark submodules), no .git
#   tests/sealed/fixtures.tar.gz        databases written by the pristine engine
#                                       through the benchmark harness, with the
#                                       parameters needed to check them
#
# and mirrors the harness sources into environment/bench.  Needs docker (the
# fixtures are produced by the pristine build inside the toolchain image so
# that they do not depend on the host).
#
# usage: scripts/seal.sh [path-to-leveldb-clone]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${1:-}"
UPSTREAM=https://github.com/google/leveldb.git
TAG=1.23
COMMIT=99b3c03b3284f5886f9ef9a4ef703d57373e61be

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if [ -z "$SRC" ]; then
  git clone -q --branch "$TAG" --recurse-submodules "$UPSTREAM" "$TMP/clone"
  SRC="$TMP/clone"
fi
got="$(git -C "$SRC" rev-parse HEAD)"
if [ "$got" != "$COMMIT" ]; then
  echo "expected commit $COMMIT, got $got" >&2
  exit 1
fi

# 1. pristine tree
mkdir -p "$TMP/tree"
rsync -a --exclude '.git' --exclude '.git*' "$SRC/" "$TMP/tree/leveldb/"
test -f "$TMP/tree/leveldb/third_party/googletest/CMakeLists.txt"
test -f "$TMP/tree/leveldb/third_party/benchmark/CMakeLists.txt"
mkdir -p "$ROOT/environment/sealed" "$ROOT/tests/sealed"
tar -C "$TMP/tree" --owner=0 --group=0 --numeric-owner --mtime='2021-02-23 00:00Z' --sort=name \
    -czf "$TMP/leveldb.tar.gz" leveldb
cp "$TMP/leveldb.tar.gz" "$ROOT/environment/sealed/leveldb.tar.gz"
cp "$TMP/leveldb.tar.gz" "$ROOT/tests/sealed/leveldb.tar.gz"
echo "sealed tree: $(du -h "$ROOT/tests/sealed/leveldb.tar.gz" | cut -f1)"

# 2. mirror the harness into the agent's tooling directory
mkdir -p "$ROOT/environment/bench"
for f in lsmbench.cc bench_env.h workload.h manifest.py measure.py; do
  cp "$ROOT/tests/harness/$f" "$ROOT/environment/bench/$f"
done

# 3. fixtures, produced by the pristine engine inside the toolchain image
docker build -q -t lsm-seal - <<'EOF'
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends g++ cmake make python3 && rm -rf /var/lib/apt/lists/*
EOF
mkdir -p "$TMP/fx"
docker run --rm -u "$(id -u):$(id -g)" -v "$TMP/tree/leveldb:/src:ro" -v "$ROOT/tests/harness:/hs:ro" \
    -v "$ROOT/scripts:/scripts:ro" -v "$TMP/fx:/fx" lsm-seal bash /scripts/gen_fixtures.sh /src /hs /fx
tar -C "$TMP/fx" --owner=0 --group=0 --numeric-owner --sort=name -czf "$ROOT/tests/sealed/fixtures.tar.gz" .
echo "fixtures: $(du -h "$ROOT/tests/sealed/fixtures.tar.gz" | cut -f1)"
sha256sum "$ROOT/tests/sealed/"*.tar.gz "$ROOT/environment/sealed/"*.tar.gz
