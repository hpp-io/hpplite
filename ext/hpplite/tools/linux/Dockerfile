# Linux build/test image for HPPLite (tools/linux_test.sh).
# Everything the extension needs on a stock distribution: a C toolchain, CMake,
# libsecp256k1, libcurl, zlib (#12), and tcl to generate the SQLite amalgamation.
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake pkg-config git ca-certificates \
        libsecp256k1-dev libcurl4-openssl-dev zlib1g-dev tcl sqlite3 python3 valgrind \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
