# Smart Office — multi-stage build.
#
# The image contains NO NPU driver. It talks to the host's dxrtd service; see
# third_party/dxrt/VENDORED.md and the comments in docker-compose.yml for the
# three things that makes necessary.

# ── builder ─────────────────────────────────────────────────────────────────
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git ca-certificates pkg-config curl \
        libssl-dev libpq-dev postgresql-server-dev-all \
        libcurl4-openssl-dev nlohmann-json3-dev \
        libjsoncpp-dev uuid-dev zlib1g-dev \
        libopencv-dev \
        libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    && rm -rf /var/lib/apt/lists/*

# Drogon, pinned. Built before the application sources are copied so the layer
# is cached across every subsequent code change — this is the slow part.
ARG DROGON_VERSION=v1.9.3
RUN git clone --depth 1 --branch ${DROGON_VERSION} --recurse-submodules \
        https://github.com/drogonframework/drogon /tmp/drogon \
    && cmake -S /tmp/drogon -B /tmp/drogon/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_EXAMPLES=OFF -DBUILD_CTL=OFF -DBUILD_ORM=ON \
        -DBUILD_POSTGRESQL=ON -DBUILD_MYSQL=OFF -DBUILD_SQLITE=OFF \
        -DBUILD_REDIS=OFF -DBUILD_SHARED_LIBS=OFF \
    && cmake --build /tmp/drogon/build -j"$(nproc)" --target install \
    && rm -rf /tmp/drogon

# The DEEPX runtime, vendored from the host (see third_party/dxrt/VENDORED.md).
COPY third_party/dxrt/include/dxrt /usr/local/include/dxrt
COPY third_party/dxrt/lib/ /usr/local/lib/
RUN ldconfig

WORKDIR /src
COPY CMakeLists.txt ./
COPY cmake/ ./cmake/
COPY third_party/doctest ./third_party/doctest
COPY src/ ./src/
COPY tests/ ./tests/
COPY tools/ ./tools/

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)"

# The suite is a build gate. Tests that need a real DX-M1 skip here - there is
# no /dev/dxrt0 in a build container - and everything else must pass, so an
# image cannot be produced from code whose decoders, auth or attendance maths
# are broken.
RUN cd /src && ./build/tests/so_tests

# ── frontend ────────────────────────────────────────────────────────────────
FROM node:20-bookworm-slim AS frontend
WORKDIR /app
COPY frontend/package.json frontend/package-lock.json* ./
RUN npm install --no-audit --no-fund
COPY frontend/ ./
RUN npm run build

# ── runtime ─────────────────────────────────────────────────────────────────
FROM ubuntu:22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates tzdata curl \
        libssl3 libpq5 libjsoncpp25 libuuid1 zlib1g libcurl4 \
        libopencv-core4.5d libopencv-imgproc4.5d libopencv-imgcodecs4.5d \
        libopencv-videoio4.5d \
        libgstreamer1.0-0 libgstreamer-plugins-base1.0-0 \
        gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
        gstreamer1.0-plugins-ugly gstreamer1.0-libav \
        gnupg dirmngr \
    && rm -rf /var/lib/apt/lists/*

# Rockchip's hardware video engine.
#
# This board is an RK3588: it has a dedicated H.264/H.265 decoder
# (/dev/mpp_service) and a 2D converter/scaler (/dev/rga) sitting beside the CPU.
# Without this plugin every camera is decoded in software, which is why a
# 3200x1800 H.265 stream ran at 4 fps against the 20 fps the camera was sending —
# and why the picture drifted further behind real time the longer it was open.
#
# mppvideodec can emit BGR at a chosen size directly, doing the colour conversion
# and the downscale on the RGA rather than on the CPU. That distinction is the
# whole point: an earlier attempt kept the conversion in software and came out
# SLOWER than decoding in software, because the decoder writes to uncached memory
# that is expensive for the CPU to read back.
#
# The key is the maintainer's own, fetched from the Ubuntu keyserver and pinned by
# fingerprint, so these packages are signature-checked like any other apt source
# rather than waved through with [trusted=yes].
ARG ROCKCHIP_PPA_KEY=3CC0D9D1F3F0354B50D24F51F02122ECF25FB4D7
# gpg needs its own directory and dirmngr to reach a keyserver at all, and it
# writes a keybox rather than a keyring unless the result is exported — apt's
# signed-by wants the exported form.
RUN mkdir -p /root/.gnupg && chmod 700 /root/.gnupg \
    && gpg --batch --keyserver keyserver.ubuntu.com --recv-keys "${ROCKCHIP_PPA_KEY}" \
    && gpg --batch --export "${ROCKCHIP_PPA_KEY}" \
        > /usr/share/keyrings/rockchip-multimedia.gpg \
    && printf '%s %s\n' \
        "deb [signed-by=/usr/share/keyrings/rockchip-multimedia.gpg]" \
        "http://ppa.launchpad.net/jjriek/rockchip-multimedia/ubuntu jammy main" \
        > /etc/apt/sources.list.d/rockchip-multimedia.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends gstreamer1.0-rockchip1 \
    && rm -rf /var/lib/apt/lists/*

COPY third_party/dxrt/lib/ /usr/local/lib/
RUN ldconfig

COPY --from=builder /src/build/smart_office /usr/local/bin/smart_office
COPY --from=frontend /app/dist /app/frontend/dist
COPY migrations/ /app/migrations/

WORKDIR /app
ENV MODELS_DIR=/app/models \
    SNAPSHOT_DIR=/data/snapshots \
    FRONTEND_DIR=/app/frontend/dist \
    MIGRATIONS_DIR=/app/migrations \
    HTTP_PORT=8080 \
    DXRT_DYNAMIC_IPC_ENDPOINT=/tmp/dxrt_dynamic_ipc.sock

EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=5s --start-period=40s --retries=3 \
    CMD curl -fsS http://localhost:8080/healthz || exit 1

ENTRYPOINT ["/usr/local/bin/smart_office"]
