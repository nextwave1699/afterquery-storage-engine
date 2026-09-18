# The agent's image.
#
# /app/leveldb    the deliverable: LevelDB 1.23 (commit 99b3c03b) with its
#                 googletest/benchmark submodules vendored, unpacked from the
#                 sealed archive and committed once to git (tag `pristine`)
# /opt/pristine   a second copy of the same tree, left untouched; bench.py
#                 builds it on first use as the baseline
# /app/bench      the benchmark harness and workload (byte copies of what the
#                 verifier runs) and bench.py, which estimates the score
#
# Nothing is compiled at image build time and nothing is fetched except apt
# packages.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ \
        cmake \
        make \
        python3 \
        git \
        patch \
        gdb \
        valgrind \
        strace \
        less \
        file \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY sealed/leveldb.tar.gz /opt/sealed/leveldb.tar.gz
COPY bench /app/bench

RUN set -e; mkdir -p /app /opt/pristine \
    && tar -C /app -xzf /opt/sealed/leveldb.tar.gz \
    && tar -C /opt/pristine --strip-components=1 -xzf /opt/sealed/leveldb.tar.gz \
    && test -f /app/leveldb/db/version_set.cc \
    && find /app/bench -type f -exec sed -i 's/\r$//' {} + \
    && chmod +x /app/bench/bench.py \
    && git -C /app/leveldb init -q -b main \
    && git -C /app/leveldb -c user.name=pristine -c user.email=pristine@localhost add -A \
    && git -C /app/leveldb -c user.name=pristine -c user.email=pristine@localhost \
           commit -q -m "LevelDB 1.23 (pristine)" \
    && git -C /app/leveldb tag pristine

WORKDIR /app/leveldb
