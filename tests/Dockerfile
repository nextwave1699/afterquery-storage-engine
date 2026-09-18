# Verifier image.  Everything the verifier trusts is baked in here and never
# comes from the agent's container: the sealed pristine engine source, the
# compatibility fixtures, the harness sources, the driver and the toolchain.
# The agent's tree arrives as the collected artifact at /app/leveldb and is
# the only outside input.  test.sh installs and fetches nothing.
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

# Candidate code (its build and every process that links it) runs as this
# user: it cannot read /tests, has no home directory and nowhere to keep
# state between runs.
RUN useradd --system --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin lsm

COPY . /tests/
# Some upload paths deliver text files with CRLF line endings, which turns
# "#!/bin/bash" into an interpreter that does not exist.  Strip carriage
# returns from the scripts (the sealed archives are binary and left alone).
RUN find /tests -type f \( -name '*.sh' -o -name '*.py' -o -name '*.cc' -o -name '*.h' \) -exec sed -i 's/\r$//' {} + \
    && chmod +x /tests/test.sh \
    && chown -R root:root /tests && chmod -R go-rwx /tests \
    && head -c 12 /tests/test.sh | grep -q '^#!/bin/bash$' \
    && test -f /tests/sealed/leveldb.tar.gz && test -f /tests/sealed/fixtures.tar.gz \
    && python3 -c "import pytest" \
    && python3 -m py_compile /tests/verify.py /tests/harness/manifest.py /tests/harness/measure.py

# The measurement helper runs as the unprivileged user, so it lives outside
# /tests, root-owned and world-readable.
RUN mkdir -p /opt/verifier && cp /tests/harness/measure.py /opt/verifier/measure.py \
    && chmod 755 /opt/verifier && chmod 644 /opt/verifier/measure.py

# Parent directories of the declared artifact and of the reward.
RUN mkdir -p /app/leveldb /logs/verifier /logs/artifacts

WORKDIR /tests
