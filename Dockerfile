# Build + test the LSM-Tree engine in a reproducible container.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update -y && apt-get install -y --no-install-recommends \
      build-essential \
      cmake \
      ninja-build \
      pkg-config \
      libgtest-dev \
      libbenchmark-dev \
      librocksdb-dev \
      libsnappy-dev \
      zlib1g-dev \
      libzstd-dev \
      liblz4-dev \
      libbz2-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /lsmdb
COPY . .

RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j

# Default: run the full test suite. Override the command to run a benchmark,
# e.g. `docker run --rm lsmdb ./build/bench_vs_rocksdb`.
CMD ["ctest", "--test-dir", "build", "--output-on-failure"]
