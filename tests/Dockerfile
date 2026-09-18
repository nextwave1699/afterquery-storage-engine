# Verifier image. Pristine source, fixtures, harness and toolchain all live
# here; the only thing that comes from the agent is /app/leveldb.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ \
        cmake \
        make \
        python3 \
        python3-pytest \
        procps \
        util-linux \
    && rm -rf /var/lib/apt/lists/*

# Candidate builds and runs happen as this user. No home, can't read /tests.
RUN useradd --system --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin lsm

COPY . /tests/
# Uploads sometimes turn LF into CRLF, which breaks the shebang. Strip CRs
# from the text files (the .tar.gz archives are left alone).
RUN find /tests -type f \( -name '*.sh' -o -name '*.py' -o -name '*.cc' -o -name '*.h' \) -exec sed -i 's/\r$//' {} + \
    && chmod +x /tests/test.sh \
    && chown -R root:root /tests && chmod -R go-rwx /tests \
    && head -c 12 /tests/test.sh | grep -q '^#!/bin/bash$' \
    && test -f /tests/sealed/leveldb.tar.gz && test -f /tests/sealed/fixtures.tar.gz \
    && python3 -c "import pytest" \
    && python3 -m py_compile /tests/verify.py /tests/harness/manifest.py /tests/harness/measure.py

# measure.py runs as lsm, so it needs a copy outside /tests.
RUN mkdir -p /opt/verifier && cp /tests/harness/measure.py /opt/verifier/measure.py \
    && chmod 755 /opt/verifier && chmod 644 /opt/verifier/measure.py

RUN mkdir -p /app/leveldb /logs/verifier /logs/artifacts

WORKDIR /tests
