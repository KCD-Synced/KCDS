# KCDS standalone voice server image (KCDS-DOCS/voice.md).
#
# Build from the repository root:
#   docker build -f Dev/docker/voice/Dockerfile -t kcds-voice .
# Run:
#   docker run -d --name kcds-voice -p 7777:7777/udp -p 7778:7778/tcp \
#     -e KCDS_VOICE_TENANTS="main=<a long random secret>" kcds-voice
# Settings: the KCDS_VOICE_* variables, or a kcds-voice.json mounted at /opt/kcds-voice.

FROM gcc:14-bookworm AS build
RUN apt-get update \
    && apt-get install -y --no-install-recommends cmake libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B /build -DCMAKE_BUILD_TYPE=Release -DKCD2S_BUILD_TESTS=OFF \
        -DKCD2S_BUILD_GAME_CLIENT=OFF \
        "-DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++ -static-libgcc" \
    && cmake --build /build --target kcds-voice -j "$(( $(nproc) * 3 / 4 ))"

FROM debian:bookworm-slim
WORKDIR /opt/kcds-voice
COPY --from=build /build/Backend/voice/kcds-voice ./kcds-voice
ENV KCDS_VOICE_BIND=0.0.0.0
EXPOSE 7777/udp 7778/tcp
USER nobody
ENTRYPOINT ["/opt/kcds-voice/kcds-voice"]
