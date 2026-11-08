# syntax=docker/dockerfile:1
# KCDS Windows client, built on Linux: clang-cl, lld-link, llvm-rc and llvm-mt with
# the MSVC CRT and the Windows SDK from xwin (Dev/cmake/clang-cl-xwin.cmake). No Windows host or
# Windows container is needed. The server is hosted on Linux only: its release build is
# Dev/docker/server/Dockerfile, so this build leaves the Windows server out (KCD2S_BUILD_SERVER=OFF).
#
# Build from the repository root:
#   docker build -f Dev/docker/windows/Dockerfile --target binaries --output <dir> .
# writes the Release binaries (game client DLL with its CEF runtime, launch client, resource
# runtime, launcher, updater and crashpad handler) to <dir>.

FROM ubuntu:24.04 AS toolchain
ARG LLVM=19
ARG XWIN_VERSION=0.6.5
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       ca-certificates curl cmake ninja-build python3 git \
       clang-${LLVM} lld-${LLVM} llvm-${LLVM} \
    && rm -rf /var/lib/apt/lists/*
ENV PATH="/usr/lib/llvm-${LLVM}/bin:${PATH}"
# The MSVC CRT and Windows SDK (the license is the user's to accept: xwin --accept-license).
RUN curl -fsSL "https://github.com/Jake-Shadle/xwin/releases/download/${XWIN_VERSION}/xwin-${XWIN_VERSION}-x86_64-unknown-linux-musl.tar.gz" \
       | tar -xz -C /usr/local/bin --strip-components=1 "xwin-${XWIN_VERSION}-x86_64-unknown-linux-musl/xwin" \
    && xwin --accept-license --arch x86_64 splat --output /xwin \
    && rm -rf .xwin-cache
ENV KCDS_XWIN=/xwin
# clang works as clang-cl when called by that name; Ubuntu ships no such link. The SDK's headers
# are spelled as the SDK ships them (xwin adds lower-case links); CEF includes "Softpub.h".
RUN ln -s clang /usr/lib/llvm-${LLVM}/bin/clang-cl \
    && ln -s SoftPub.h /xwin/sdk/include/um/Softpub.h

# The pinned third-party sources the client build takes from outside the repository: MinHook
# (KCD2S_MINHOOK_DIR) and the WebView2 SDK (KCDS_WEBVIEW2_ROOT).
ARG MINHOOK_COMMIT=9fbd087432700d73fc571118d6a9697a36443d88
ARG WEBVIEW2_VERSION=1.0.2365.46
RUN apt-get update && apt-get install -y --no-install-recommends unzip     && rm -rf /var/lib/apt/lists/*     && mkdir -p /deps/minhook /deps/webview2     && curl -fsSL "https://github.com/TsudaKageyu/minhook/archive/${MINHOOK_COMMIT}.tar.gz"        | tar -xz -C /deps/minhook --strip-components=1     && curl -fsSL -o /tmp/webview2.nupkg        "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${WEBVIEW2_VERSION}"     && unzip -q /tmp/webview2.nupkg -d /deps/webview2     && rm /tmp/webview2.nupkg

# The launcher's Ed25519 release verification (EVP_DigestVerify with EVP_PKEY_ED25519) links a
# static libcrypto with the MSVC ABI: LibreSSL builds with clang-cl through CMake (OpenSSL's own
# Windows build needs nmake). Built from the pinned release tarball, never kept in the repository.
ARG LIBRESSL_VERSION=4.1.0
COPY Dev/cmake/clang-cl-xwin.cmake /opt/kcds/clang-cl-xwin.cmake
RUN curl -fsSL "https://cdn.openbsd.org/pub/OpenBSD/LibreSSL/libressl-${LIBRESSL_VERSION}.tar.gz"        | tar -xz -C /tmp     && cmake -S /tmp/libressl-${LIBRESSL_VERSION} -B /tmp/libressl-build -G Ninja        -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=/opt/kcds/clang-cl-xwin.cmake        -DBUILD_SHARED_LIBS=OFF -DLIBRESSL_APPS=OFF -DLIBRESSL_TESTS=OFF        -DCMAKE_INSTALL_PREFIX=/deps/libressl     && cmake --build /tmp/libressl-build --target install     && cp /tmp/libressl-${LIBRESSL_VERSION}/COPYING /deps/libressl/COPYING     && rm -rf /tmp/libressl-${LIBRESSL_VERSION} /tmp/libressl-build

# The .NET SDK builds the runtime C# client resources run on (the kcds-csharp-host
# target): .NET compiled to WebAssembly, the same on any build host. The official tarball of the
# SDK the CI pins, checked against its SHA-512.
ARG DOTNET_SDK_VERSION=9.0.318
ARG DOTNET_SDK_SHA512=e8685293a3512178e0de1bb3c1663e31fdf9d761af705094bf833cc1ff6b9a18c543aa4141f0216503c98039c719585a6d32905b0bbb3279e93b7e7616e047c3
ENV DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
RUN curl -fsSL -o /tmp/dotnet.tar.gz \
        "https://builds.dotnet.microsoft.com/dotnet/Sdk/${DOTNET_SDK_VERSION}/dotnet-sdk-${DOTNET_SDK_VERSION}-linux-x64.tar.gz" \
    && echo "${DOTNET_SDK_SHA512}  /tmp/dotnet.tar.gz" | sha512sum -c - \
    && mkdir -p /usr/share/dotnet \
    && tar -xzf /tmp/dotnet.tar.gz -C /usr/share/dotnet \
    && rm /tmp/dotnet.tar.gz \
    && ln -s /usr/share/dotnet/dotnet /usr/local/bin/dotnet \
    && dotnet --version

FROM toolchain AS build
# How many compile jobs at once (--build-arg BUILD_JOBS=22); empty: one per core.
ARG BUILD_JOBS=""
# A folder in the build context holding the pinned sentry-native and CEF archives
# (--build-arg DEPS_DIR=/src/Dev/docker/local-deps): the build downloads neither. Empty: downloaded.
ARG DEPS_DIR=""
# A hotfix that keeps the previous release's wire label: kcds-<previous version> (as the server's
# Dockerfile); empty: the version's own.
ARG KCD2S_BUILD_SALT=""
WORKDIR /src
COPY . .
# Every Release client target: the game client with its CEF runtime and web helper, the launch
# client, the resource runtime, and the launcher with its updater and crashpad handler
# (crash reporting on, as the release ships). The build secret is a BuildKit secret:
# CMake derives the handshake key from it, and it never lands in a layer.
RUN --mount=type=secret,id=build_secret,required=true --mount=type=secret,id=sentry_dsn,required=true     cmake -S . -B /build -G "Ninja Multi-Config" -DCMAKE_CONFIGURATION_TYPES=Release         -DKCDS_RELEASE_BUILD=ON -DKCDS_BUILD_SECRET_FILE=/run/secrets/build_secret -DKCDS_CRASH_DSN_FILE=/run/secrets/sentry_dsn ${KCD2S_BUILD_SALT:+-DKCD2S_BUILD_SALT=$KCD2S_BUILD_SALT} ${DEPS_DIR:+-DKCDS_DEPS_DIR=$DEPS_DIR}        -DCMAKE_TOOLCHAIN_FILE=/src/Dev/cmake/clang-cl-xwin.cmake        -DKCD2S_BUILD_GAME_CLIENT=ON -DKCD2S_BUILD_SERVER=OFF -DKCD2S_BUILD_TESTS=OFF -DKCDS_ENABLE_SENTRY=ON        -DKCD2S_MINHOOK_DIR=/deps/minhook -DKCDS_WEBVIEW2_ROOT=/deps/webview2        -DKCDS_OPENSSL_INCLUDE=/deps/libressl/include        -DKCDS_OPENSSL_LIBCRYPTO=/deps/libressl/lib/crypto.lib     && cmake --build /build --config Release --parallel ${BUILD_JOBS}

# Every shipped client binary with its PDB, in the build's layout: the release uploads the symbols
# to the crash server from here (Dev/tools/release/upload_symbols.py --build).
FROM scratch AS binaries
COPY --from=build /build/Client/runtime/Release/ /Client/runtime/Release/
COPY --from=build /build/Client/launcher/Release/ /Client/launcher/Release/
COPY --from=build /build/Client/ui/web/Release/ /Client/ui/web/Release/

# The release zips (KCDS-Client-<version>.zip, KCDS-Types-<version>.zip and SHA256SUMS.txt), made
# by Dev/tools/release/package.py from this build:
#   docker build -f Dev/docker/windows/Dockerfile --target package \
#     --secret id=logo_header,src=<file> [--secret id=sentry_dsn,src=<file>] \
#     [--build-arg SERVER_ADDRESS=host:port] [--build-arg UPDATE_ORIGIN=https://...] --output <dir> .
# logo_header holds the 128-byte DDS header of the game's own logo (the branding pak keeps it, see
# Client/mods/logo/make_mod.py); it comes from the game, so it is a build secret and never a layer.
FROM build AS package-build
ARG PILLOW_VERSION=12.3.0
RUN apt-get update \
    && apt-get install -y --no-install-recommends python3-pip xz-utils \
    && rm -rf /var/lib/apt/lists/* \
    && pip install --break-system-packages --no-cache-dir "pillow==${PILLOW_VERSION}"
# Node runs the SDK's extractor for KCDS-Types-<version>.zip (package.py), the
# version the CI and the servers run; the official tarball, checked against its SHA-256.
ARG NODE_VERSION=26.13.0
ARG NODE_SHA256=d28a96af6c38596f6d9d97c0ee531c13d238d757742387d1928de822d959e997
RUN curl -fsSL -o /tmp/node.tar.xz \
        "https://nodejs.org/dist/v${NODE_VERSION}/node-v${NODE_VERSION}-linux-x64.tar.xz" \
    && echo "${NODE_SHA256}  /tmp/node.tar.xz" | sha256sum -c - \
    && tar -xJf /tmp/node.tar.xz -C /usr/local --strip-components=1 \
    && rm /tmp/node.tar.xz \
    && node --version
ARG SERVER_ADDRESS=""
ARG UPDATE_ORIGIN=""
RUN --mount=type=secret,id=logo_header,required=true \
    --mount=type=secret,id=sentry_dsn \
    set -e; \
    crash_reports=""; \
    if [ -s /run/secrets/sentry_dsn ]; then \
        crash_reports="--sentry-dsn-file /run/secrets/sentry_dsn"; \
    fi; \
    python3 Dev/tools/release/package.py --build /build --out /out \
        --logo-header /run/secrets/logo_header \
        --server-address "${SERVER_ADDRESS}" ${UPDATE_ORIGIN:+--update-origin "$UPDATE_ORIGIN"} \
        ${crash_reports}

FROM scratch AS package
COPY --from=package-build /out/ /
