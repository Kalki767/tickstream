# syntax=docker/dockerfile:1
#
# Multi-stage build: compile with the full toolchain, ship only the binary
# and its runtime libraries.
#
#   docker build -t tickstream .
#   docker run --rm -e TICKSTREAM_DSN="host=... dbname=... user=... password=..." tickstream

# ---- build stage ----------------------------------------------------------
FROM ubuntu:24.04 AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential cmake git ca-certificates \
      libboost-dev libssl-dev libpq-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY include include
COPY src src
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DTICKSTREAM_BUILD_TESTS=OFF -DTICKSTREAM_BUILD_BENCH=OFF \
 && cmake --build build -j"$(nproc)" --target tickstream \
 && strip build/tickstream

# ---- runtime stage --------------------------------------------------------
FROM ubuntu:24.04 AS runtime
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates libssl3t64 libpq5 \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --no-create-home tickstream
COPY --from=build /src/build/tickstream /usr/local/bin/tickstream
USER tickstream
# All configuration via the environment and arguments; no credentials baked in.
ENV TICKSTREAM_DSN=""
ENTRYPOINT ["/usr/local/bin/tickstream"]
