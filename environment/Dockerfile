# Agent image.
#
#   /app/leveldb   LevelDB 1.23 (99b3c03b) with submodules vendored, in a git
#                  repo tagged `pristine`; this is what gets collected
#   /opt/pristine  untouched copy, bench.py builds it as the baseline
#   /app/bench     same harness sources the verifier uses, plus bench.py
#
# Nothing gets compiled here; the only download is apt.
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
