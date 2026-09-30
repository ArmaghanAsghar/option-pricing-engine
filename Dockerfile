# ---------------------------------------------------------------- build stage
FROM gcc:13-bookworm AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
      cmake ninja-build git ca-certificates libssl-dev libsqlite3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Copy the build files first so a source-only change reuses the cached
# dependency downloads.
COPY CMakeLists.txt ./
COPY src/market/CMakeLists.txt src/market/
COPY src/api/CMakeLists.txt src/api/
COPY tests/CMakeLists.txt tests/

COPY include/ include/
COPY src/ src/
COPY tests/ tests/
COPY bench/ bench/

RUN cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DPRICER_BUILD_TESTS=ON \
      -DPRICER_BUILD_API=ON \
      -DPRICER_BUILD_UI=OFF \
      -DPRICER_BUILD_BENCH=ON \
    && cmake --build build --parallel

# A failing test fails the image, so a built container is always a tested one.
RUN ctest --test-dir build --output-on-failure --parallel 4

# -------------------------------------------------------------- runtime stage
FROM debian:bookworm-slim AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates libssl3 libsqlite3-0 libstdc++6 curl \
    && rm -rf /var/lib/apt/lists/*

# Unprivileged user; /data is the SQLite cache volume.
RUN useradd --system --create-home --uid 10001 pricer \
    && mkdir -p /data && chown pricer:pricer /data

COPY --from=builder /src/build/pricer-api  /usr/local/bin/pricer-api
COPY --from=builder /src/build/pricer-cli  /usr/local/bin/pricer-cli
COPY --from=builder /src/build/pricer-bench /usr/local/bin/pricer-bench

USER pricer
WORKDIR /home/pricer
VOLUME ["/data"]

ENV PRICER_HOST=0.0.0.0 \
    PRICER_PORT=8080 \
    PRICER_CACHE_PATH=/data/market.db

EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
  CMD curl -fsS http://localhost:8080/health > /dev/null || exit 1

ENTRYPOINT ["/usr/local/bin/pricer-api"]
