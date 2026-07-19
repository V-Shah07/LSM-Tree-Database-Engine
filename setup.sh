#!/usr/bin/env bash
# ==============================================================================
# LSM-Tree Storage Engine  —  Claude Code cloud setup script
# ------------------------------------------------------------------------------
# Goes in the repo root as setup.sh (or wherever the cloud env config points).
# Purpose: install the C++17 toolchain, CMake, GoogleTest, Google Benchmark,
# and RocksDB (the benchmark baseline) ONCE so it's cached for every session.
#
# Network allow-list this repo needs (set in the cloud env network config):
#   archive.ubuntu.com, security.ubuntu.com   (apt packages)
#   github.com, codeload.github.com, *.githubusercontent.com  (source clones)
# That's it — this project barely touches the web.
# ==============================================================================
set -euo pipefail

echo ">>> [LSM] Updating apt and installing C++ toolchain + build deps..."
sudo apt-get update -y
sudo apt-get install -y --no-install-recommends \
  build-essential \
  cmake \
  ninja-build \
  git \
  pkg-config \
  libgtest-dev \
  libbenchmark-dev \
  librocksdb-dev \
  libsnappy-dev \
  zlib1g-dev \
  libzstd-dev \
  liblz4-dev \
  libbz2-dev

# On some images libgtest-dev ships sources only and must be compiled once.
if [ -d /usr/src/googletest ]; then
  echo ">>> [LSM] Building bundled GoogleTest..."
  ( cd /usr/src/googletest && sudo cmake -S . -B build && sudo cmake --build build -j"$(nproc)" \
      && sudo cp build/lib/*.a /usr/lib/ 2>/dev/null || true )
fi

echo ">>> [LSM] Toolchain versions:"
g++ --version | head -1
cmake --version | head -1
echo -n "rocksdb header present: "; [ -f /usr/include/rocksdb/db.h ] && echo yes || echo "NO (check librocksdb-dev)"

echo ">>> [LSM] Setup complete. Baseline (RocksDB), GoogleTest, and Google Benchmark are ready."
