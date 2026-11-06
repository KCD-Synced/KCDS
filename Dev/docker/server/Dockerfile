# KCDS dedicated server image: the Linux server and its Node.js resource host, with an
# empty resources folder for the owner's resources.
#
# Build from the repository root, with the release build secret (a file holding it):
#   docker build -f Dev/docker/server/Dockerfile --secret id=build_secret,src=<file> -t kcds-server .
# Run (UDP 7777):
#   docker run -d --name kcds -p 7777:7777/udp -p 7777:7777/tcp -e KCDS_SERVER_NAME="My server" kcds-server
# Own resources: mount a folder over /opt/kcds/resources. Settings: see start-server.sh and the
# KCDS_SERVER_* variables in Backend/integration/local/config/host_settings.hpp.

FROM gcc:14-bookworm AS build
RUN apt-get update     && apt-get install -y --no-install-recommends cmake libcurl4-openssl-dev     && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
# The build secret is a BuildKit secret: CMake derives the handshake key from it, and it never
# lands in a layer. A hotfix that keeps the previous release's wire label passes it as
#   --build-arg KCD2S_BUILD_SALT=kcds-<previous version>
# (the Windows release build takes the same -DKCD2S_BUILD_SALT); empty: the version's own.
ARG KCD2S_BUILD_SALT=
# How many compile jobs at once (--build-arg BUILD_JOBS=22); empty: one per core.
ARG BUILD_JOBS=""
RUN --mount=type=secret,id=build_secret,required=true     cmake -S . -B /build -DCMAKE_BUILD_TYPE=Release -DKCD2S_BUILD_TESTS=OFF         -DKCDS_RELEASE_BUILD=ON -DKCDS_BUILD_SECRET_FILE=/run/secrets/build_secret         -DKCD2S_BUILD_GAME_CLIENT=OFF         ${KCD2S_BUILD_SALT:+-DKCD2S_BUILD_SALT=$KCD2S_BUILD_SALT}     && cmake --build /build --parallel ${BUILD_JOBS} --target kcd2s-server kcds-voice     && strip /build/Backend/integration/kcd2s-server

# The runtime C# server resources run on: .NET compiled to WebAssembly, built by
# Dev/tools/build_csharp_host.py with the SDK the CI pins (the official tarball, checked against
# its SHA-512). It runs inside the resource host's Node.js, so nothing of .NET ships but it.
FROM debian:bookworm-slim AS csharp
ARG DOTNET_SDK_VERSION=9.0.318
ARG DOTNET_SDK_SHA512=e8685293a3512178e0de1bb3c1663e31fdf9d761af705094bf833cc1ff6b9a18c543aa4141f0216503c98039c719585a6d32905b0bbb3279e93b7e7616e047c3
ENV DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl python3 \
    && rm -rf /var/lib/apt/lists/* \
    && curl -fsSL -o /tmp/dotnet.tar.gz \
        "https://builds.dotnet.microsoft.com/dotnet/Sdk/${DOTNET_SDK_VERSION}/dotnet-sdk-${DOTNET_SDK_VERSION}-linux-x64.tar.gz" \
    && echo "${DOTNET_SDK_SHA512}  /tmp/dotnet.tar.gz" | sha512sum -c - \
    && mkdir -p /usr/share/dotnet \
    && tar -xzf /tmp/dotnet.tar.gz -C /usr/share/dotnet \
    && rm /tmp/dotnet.tar.gz \
    && ln -s /usr/share/dotnet/dotnet /usr/local/bin/dotnet
WORKDIR /src
COPY Backend/scripting/runtimes/csharp ./Backend/scripting/runtimes/csharp
COPY Dev/tools/build_csharp_host.py ./Dev/tools/build_csharp_host.py
RUN python3 Dev/tools/build_csharp_host.py

# The release package: KCDS-Server-<version>-linux.tar.gz and its SHA256SUMS, made by
# Dev/tools/release/package.py from this build. Export it with
#   docker build -f Dev/docker/server/Dockerfile --target package --output <dir> .
FROM build AS package-build
COPY --from=csharp /src/Backend/scripting/runtimes/js/languages/dotnet ./Backend/scripting/runtimes/js/languages/dotnet
RUN apt-get update \
    && apt-get install -y --no-install-recommends python3 \
    && rm -rf /var/lib/apt/lists/* \
    && python3 Dev/tools/release/package.py --server-linux --build /build --out /out

FROM scratch AS package
COPY --from=package-build /out/ /

FROM node:26-bookworm-slim
RUN apt-get update     && apt-get install -y --no-install-recommends libcurl4 ca-certificates tini     && rm -rf /var/lib/apt/lists/*
WORKDIR /opt/kcds
COPY --from=build /build/Backend/integration/kcd2s-server ./kcd2s-server
COPY Backend/scripting/runtimes/js ./runtime/Backend/scripting/runtimes/js
COPY --from=csharp /src/Backend/scripting/runtimes/js/languages/dotnet ./runtime/Backend/scripting/runtimes/js/languages/dotnet
COPY Client/resources ./runtime/Client/resources
RUN mkdir -p ./resources
COPY --chmod=755 Dev/docker/server/start-server.sh ./start-server.sh
# Port 7777, level trosecko and the name "KCDS server" are the server's own defaults; a mounted
# /opt/kcds/server.json or KCDS_SERVER_* variables change them (start-server.sh).
ENV KCDS_DOCKER=1
EXPOSE 7777/udp 7777/tcp
# The server writes next to its resources: crash reports, backups, bans and admin accounts.
RUN chown -R node:node /opt/kcds
USER node
# -g: a stop signal reaches the server too, not only the start script that runs it (it starts the
# server again after a restart, exit code 75).
ENTRYPOINT ["tini", "-g", "--", "/opt/kcds/start-server.sh"]
