"""Package a KCDS release: the Client zip players download and the Linux server hosts run.

Only the files listed below are copied, so no source, symbols, tests or logs can slip in.

    python Dev/tools/release/package.py --build <cmake build dir> --version 0.1.1 --out <dir>
        (--game <KCD2 install root> | --logo-header <file>) [--server-address host:port]
        [--update-origin https://updates.kcdsynced.com] [--sentry-dsn-file <file holding the crash DSN>]

    python Dev/tools/release/package.py --server-linux --build <Linux cmake build dir> --out <dir>

Servers are hosted on Linux only (the Linux server package and the Docker image); there is no
Windows server package. --server-linux packages the Linux server,
KCDS-Server-<version>-linux.tar.gz: kcd2s-server, the resource host under runtime/, the example
resources, start-server.sh, server.json and README.txt. The release build runs it in Docker:
    docker build -f Dev/docker/server/Dockerfile --target package --output <dir> .

Without --server-linux the output directory gets KCDS-Client-<version>.zip,
KCDS-Types-<version>.zip (the SDK's TypeScript declarations: shared.d.ts,
server-api.d.ts and client-api.d.ts generated from this checkout's API, as the website shows it)
and SHA256SUMS.txt. The build must be a Release build of kcds-launcher, kcds-launcher-update,
kcd2s-launch-client, kcd2s-game-client and kcd2s-resource-runtime.

The shipped kcds-launcher.json updates from --update-origin (the official
https://updates.kcdsynced.com by default) and trusts the keys of update-publisher-keys.json: a
release never ships with updates off, because the file is never replaced by an update and such an
install could never update again (0.3.4). --no-updates leaves them off, for a package that is not
a release. The launcher then starts the game only from an install that
matches a release it verified from that origin, so the build needs signed-release verification
(OpenSSL) and the update feed must publish this same client zip before anyone downloads it.
--sentry-dsn-file turns crash reporting on with the DSN read from that file (a client DSN carries
only the public key; it is not printed).

The Client zip also gets assets/kcds-branding.pak, our main-menu logo that the game-client DLL
mounts at runtime. It is built here from Client/mods/logo with the DDS header of the installed
game's logo (read only), so the game-derived bytes are never committed. Without the game (the
Linux release build, Dev/docker/windows/Dockerfile) --logo-header gives those 128 bytes as a file.
"""

import argparse
import base64
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tempfile
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import installer  # noqa: E402  (sibling release tool)

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent

# (path inside the build directory, name inside the zip)
CLIENT_BINARIES = [
    ("Client/launcher/Release/kcds-launcher.exe", "kcds-launcher.exe"),
    ("Client/launcher/Release/kcds-launcher-update.exe", "kcds-launcher-update.exe"),
    # The crash handler that the launcher and the game-client DLL start (sentry-native).
    ("Client/launcher/Release/crashpad_handler.exe", "crashpad_handler.exe"),
    ("Client/runtime/Release/kcd2s-launch-client.exe", "kcd2s-launch-client.exe"),
    ("Client/runtime/Release/kcd2s-game-client-v4.dll", "kcd2s-game-client-v4.dll"),
    # Voice chat's 3D sound: the DLL loads it from beside itself (Client/voice/steamaudio.cmake).
    ("Client/runtime/Release/phonon.dll", "phonon.dll"),
    ("Client/runtime/Release/kcd2s-resource-runtime.exe", "kcd2s-resource-runtime.exe"),
    # CEF's renderer, GPU and utility processes for resource web views.
    ("Client/ui/web/Release/kcds-web-helper.exe", "kcds-web-helper.exe"),
]
# The official CEF runtime the game-client DLL loads for resource web views, from the build's
# copy next to the DLL. Only the en-US locale ships (the DLL pins it); CEF's licence and the
# Chromium credits go along under licenses/.
CEF_RUNTIME = [
    "libcef.dll",
    "chrome_elf.dll",
    "d3dcompiler_47.dll",
    "dxcompiler.dll",
    "dxil.dll",
    "vk_swiftshader.dll",
    "vk_swiftshader_icd.json",
    "vulkan-1.dll",
    "icudtl.dat",
    "resources.pak",
    "chrome_100_percent.pak",
    "chrome_200_percent.pak",
    "v8_context_snapshot.bin",
    "locales/en-US.pak",
]
CEF_NOTICES = [
    ("_deps/kcds_cef-src/LICENSE.txt", "licenses/CEF-LICENSE.txt"),
    ("_deps/kcds_cef-src/CREDITS.html", "licenses/CEF-CREDITS.html"),
]
# sentry-native and what Crashpad links on Windows, from the build's copy of the pinned SDK:
# (path inside the SDK, name inside the zip).
SENTRY_SOURCE = "_deps/kcds_sentry_native-src"
CRASHPAD_THIRD_PARTY = "external/crashpad/third_party"
SENTRY_NOTICES = [
    ("LICENSE", "licenses/sentry-native-LICENSE.txt"),
    ("external/crashpad/LICENSE", "licenses/crashpad-LICENSE.txt"),
    (f"{CRASHPAD_THIRD_PARTY}/cpp-httplib/cpp-httplib/LICENSE", "licenses/cpp-httplib-LICENSE.txt"),
    (f"{CRASHPAD_THIRD_PARTY}/getopt/LICENSE", "licenses/getopt-LICENSE.txt"),
    (
        f"{CRASHPAD_THIRD_PARTY}/mini_chromium/mini_chromium/LICENSE",
        "licenses/mini_chromium-LICENSE.txt",
    ),
    (
        f"{CRASHPAD_THIRD_PARTY}/mini_chromium/mini_chromium/base/third_party/icu/LICENSE",
        "licenses/mini_chromium-icu-LICENSE.txt",
    ),
    (f"{CRASHPAD_THIRD_PARTY}/mpack/LICENSE", "licenses/mpack-LICENSE.txt"),
    (f"{CRASHPAD_THIRD_PARTY}/zlib/zlib/LICENSE", "licenses/zlib-LICENSE.txt"),
]
# Licence files of the crypto library, beside its include folder: LibreSSL in a release build, or
# the OpenSSL that find_package found in a local one.
CRYPTO_NOTICE_NAMES = ["COPYING", "LICENSE.txt", "LICENSE"]
# Repository files shipped with the client: (source, name inside the zip). The game client loads
# the server browser page from ui/menu next to its DLL.
MCP_SERVER = ROOT / "Dev/tools/architect-mcp"
CLIENT_FILES = (
    [
        (ROOT / "Client/ui/menu/index.html", "ui/menu/index.html"),
        # The "Synced" lettering the DLL draws over the exe's own start splash.
        (
            ROOT / "Client/runtime/branding/assets/kcds-splash-overlay.png",
            "assets/kcds-splash-overlay.png",
        ),
    ]
    + [
        (path, "ui/menu/fonts/" + path.name)
        for path in sorted((ROOT / "Client/ui/menu/fonts").iterdir())
        if path.suffix in (".woff2", ".css", ".txt")
    ]
    + [
        # The MCP server of World Architect and the Asset/Animation Studio for AI assistants (the
        # editor's AI tab shows its path), with its folders (models/) and its screenshot script.
        (path, "architect-mcp/" + path.relative_to(MCP_SERVER).as_posix())
        for path in sorted(MCP_SERVER.rglob("*"))
        if path.suffix in (".cjs", ".ps1") and not path.name.endswith(".test.cjs")
    ]
)
# The .NET runtime C# resources run on, built by Dev/tools/build_csharp_host.py: on
# the client next to kcd2s-resource-runtime.exe (the build's kcds-csharp-host target), on the
# server in the resource host's languages/dotnet. Only its _framework folder runs; the client's
# copy of Kcds.Sdk.dll and its XML docs go to the Types zip for authors.
CLIENT_DOTNET = "Client/runtime/Release/dotnet"
SERVER_DOTNET = ROOT / "Backend/scripting/runtimes/js/languages/dotnet"
# The C# resource starter project of the Types zip.
CSHARP_TEMPLATE = ROOT / "Backend/scripting/runtimes/csharp/Template"
# The Linux server from a single-configuration (Makefile or Ninja) build.
LINUX_SERVER_BINARY = ("Backend/integration/kcd2s-server", "kcd2s-server")
LINUX_VOICE_BINARY = ("Backend/voice/kcds-voice", "kcds-voice")
# Our asset pak, built by Client/mods/logo/make_mod.py; the DLL loads it from assets/ next to itself.
BRANDING_BUILDER = ROOT / "Client/mods/logo/make_mod.py"
BRANDING_PAK = "assets/kcds-branding.pak"
# The server's resource host (Node.js): main.cjs and every module it requires, kept at their
# repository-relative paths under runtime/ so the relative requires still resolve.
SERVER_RUNTIME_ENTRY = ROOT / "Backend/scripting/runtimes/js/main.cjs"
# start-server.sh sends the server's output through it into the rotating log file.
SERVER_LOG_WRITER = ROOT / "Backend/scripting/runtimes/js/logging/log_file.cjs"
RELATIVE_REQUIRE = re.compile(r"require\('(\.\.?/[^']+)'\)")
# A data file a runtime module reads beside itself (areas/level_areas.json.gz): shipped, not scanned.
RUNTIME_DATA = re.compile(r"path\.join\(__dirname, '([^']+)'\)")
# Resources shipped in the server package's resources/ folder: none, a server starts with only its
# owner's (the folder holds RESOURCES_README).
EXAMPLE_RESOURCES = []
RESOURCES_README = (
    "Put each resource of your server in a folder of its own here: every folder with a\n"
    'package.json that has a "kcds" section is started. See https://kcdsynced.com/docs/resources\n'
)

# A release (0.1.2) or a hotfix of one (0.1.2.1).
VERSION_PATTERN = re.compile(r"^\d+\.\d+\.\d+(\.\d+)?$")
ADDRESS_PATTERN = re.compile(r"^[A-Za-z0-9.-]+:\d{1,5}$")
# A bare https origin as the launcher's feed::validate_config accepts it: host and optional port,
# no path, query or user info.
ORIGIN_PATTERN = re.compile(r"^https://[A-Za-z0-9](?:[A-Za-z0-9.-]*[A-Za-z0-9])?(?::\d{1,5})?/?$")
# A client DSN: https://<public key>@<host>/<project id>, never a secret key.
DSN_PATTERN = re.compile(r"^https://[A-Za-z0-9]{16,128}@[A-Za-z0-9.-]+(?::\d{1,5})?/\d{1,20}$")
PUBLISHER_KEYS = HERE / "update-publisher-keys.json"
# DER prefix of an Ed25519 SubjectPublicKeyInfo; the 32 raw key bytes follow it.
ED25519_SPKI_PREFIX = bytes.fromhex("302a300506032b6570032100")


def fail(message):
    print("package: " + message, file=sys.stderr)
    sys.exit(1)


def read_template(name, version, line_end="\r\n"):
    # A Windows checkout may hold the template with CRLF; the package's line ends are set here.
    text = (HERE / name).read_text(encoding="utf8").replace("\r\n", "\n")
    return text.replace("{{VERSION}}", version).replace("\n", line_end)


def raw_public_key(pem):
    """The 32 raw key bytes of an Ed25519 PEM public key, as the 64 hex characters the launcher reads."""
    lines = [line for line in pem.splitlines() if line and not line.startswith("-----")]
    der = base64.b64decode("".join(lines))
    if len(der) != len(ED25519_SPKI_PREFIX) + 32 or not der.startswith(ED25519_SPKI_PREFIX):
        fail("update-publisher-keys.json holds a key that is not Ed25519")
    return der[len(ED25519_SPKI_PREFIX) :].hex()


def publisher_keys():
    keys = json.loads(PUBLISHER_KEYS.read_text(encoding="utf8"))
    if not keys:
        fail("update-publisher-keys.json has no keys")
    return {key_id: raw_public_key(pem) for key_id, pem in keys.items()}


def build_version(build):
    """The version the build was configured with: project(VERSION) in the root CMakeLists.txt.

    The launcher, the game client and the server report this version (window, menu footer, master
    listing, compatibility check), so the zips carry it too and never another label.
    """
    cache = build / "CMakeCache.txt"
    if not cache.is_file():
        fail(f"missing {cache}: --build must be a configured CMake build directory")
    for line in cache.read_text(encoding="utf8", errors="replace").splitlines():
        name, _, value = line.partition("=")
        if name.startswith("CMAKE_PROJECT_VERSION:"):
            return value.strip()
    fail(f"{cache} has no CMAKE_PROJECT_VERSION")


def build_has_signing(build):
    """True when the build linked OpenSSL, so its launcher can verify signed releases."""
    cache = build / "CMakeCache.txt"
    if not cache.is_file():
        return False
    for line in cache.read_text(encoding="utf8", errors="replace").splitlines():
        name, _, value = line.partition("=")
        if name.startswith("KCDS_OPENSSL_LIBCRYPTO:") and value.strip():
            return True
        if name.startswith("OPENSSL_CRYPTO_LIBRARY:") and value and not value.endswith("NOTFOUND"):
            return True
    return False


def cache_path(build, *names):
    """The first of these CMake cache entries that is set, as a path; None when none is."""
    text = (build / "CMakeCache.txt").read_text(encoding="utf8", errors="replace")
    values = {}
    for line in text.splitlines():
        name, _, value = line.partition("=")
        values[name.partition(":")[0]] = value.strip()
    for name in names:
        value = values.get(name, "")
        if value and not value.endswith("NOTFOUND"):
            return Path(value)
    return None


def required_cache_path(build, name):
    path = cache_path(build, name)
    if path is None:
        fail(f"{build / 'CMakeCache.txt'} has no {name}, so its licence cannot ship")
    return path


def crypto_notice(build):
    """The licence file of the linked crypto library, or None for a build without one."""
    if not build_has_signing(build):
        return None
    include = cache_path(build, "KCDS_OPENSSL_INCLUDE", "OPENSSL_INCLUDE_DIR")
    if include is None:
        fail("the build links a crypto library, but its include folder is not in the CMake cache")
    for name in CRYPTO_NOTICE_NAMES:
        path = include.parent / name
        if path.is_file():
            return path
    fail(f"no licence file of the crypto library beside {include}")


def client_notices(build):
    """Every third-party notice of the client zip: (source file, name inside the zip)."""
    notices = [(build / source, name) for source, name in CEF_NOTICES]
    sentry = build / SENTRY_SOURCE
    notices += [(sentry / source, name) for source, name in SENTRY_NOTICES]
    minhook = required_cache_path(build, "KCD2S_MINHOOK_DIR")
    notices.append((minhook / "LICENSE.txt", "licenses/MinHook-LICENSE.txt"))
    webview2 = required_cache_path(build, "KCDS_WEBVIEW2_ROOT")
    notices.append((webview2 / "LICENSE.txt", "licenses/WebView2-LICENSE.txt"))
    notices.append((webview2 / "NOTICE.txt", "licenses/WebView2-NOTICE.txt"))
    voice = ROOT / "Client/voice/third_party"
    notices.append((voice / "opus/COPYING", "licenses/opus-LICENSE.txt"))
    notices.append((voice / "rnnoise/COPYING", "licenses/rnnoise-LICENSE.txt"))
    notices.append((voice / "steamaudio/THIRDPARTY.md", "licenses/steamaudio-THIRDPARTY.md"))
    notices.append(
        (
            ROOT / "Backend/voice/third_party/monocypher/LICENCE.md",
            "licenses/monocypher-LICENSE.txt",
        )
    )
    crypto = crypto_notice(build)
    if crypto is not None:
        notices.append((crypto, "licenses/crypto-LICENSE.txt"))
    return notices


def build_is_developer(build):
    """True when the build is a developer build (KCDS_DEV_BUILD): its launch client starts the
    game windowed at 960x540 with -devmode, for the local test kit, never for players.
    """
    cache = build / "CMakeCache.txt"
    if not cache.is_file():
        return False
    for line in cache.read_text(encoding="utf8", errors="replace").splitlines():
        name, _, value = line.partition("=")
        if name.startswith("KCDS_DEV_BUILD:"):
            return value.strip().upper() in ("ON", "TRUE", "1", "YES")
    return False


# Where official releases update from (Client/launcher/CMakeLists.txt has the same origin).
OFFICIAL_UPDATE_ORIGIN = "https://updates.kcdsynced.com"


def client_config(server_address, update_origin, sentry_dsn):
    config = json.loads((ROOT / "Client/launcher/config.example.json").read_text(encoding="utf8"))
    config["serverAddress"] = server_address
    config["updateOrigin"] = update_origin.rstrip("/")
    config["publisherPublicKeys"] = publisher_keys() if update_origin else {}
    config["sentry"]["enabled"] = bool(sentry_dsn)
    config["sentry"]["dsn"] = sentry_dsn
    return json.dumps(config, indent=2) + "\n"


def add_file(archive, source, name):
    if not source.is_file():
        fail(f"missing {source}")
    archive.write(source, name)


def add_text(archive, name, text):
    archive.writestr(name, text.encode("utf8"))


def branding_pak(game_root, logo_header):
    spec = importlib.util.spec_from_file_location("kcds_branding_builder", BRANDING_BUILDER)
    builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(builder)
    try:
        return builder.build_pak(game_root, logo_header)
    except builder.BrandingError as error:
        fail(f"branding pak: {error}")


def runtime_files():
    found = set()
    pending = [SERVER_RUNTIME_ENTRY.resolve(), SERVER_LOG_WRITER.resolve()]
    while pending:
        path = pending.pop()
        if path in found:
            continue
        if path.is_dir():
            # A data folder a module reads (the levels' herbs): every file in it goes along.
            for inner in sorted(path.rglob("*")):
                if inner.is_file():
                    pending.append(inner.resolve())
            found.add(path)
            continue
        if not path.is_file():
            fail(f"server runtime module missing: {path}")
        found.add(path)
        if path.suffix not in (".cjs", ".js"):
            continue
        text = path.read_text(encoding="utf8")
        for target in RELATIVE_REQUIRE.findall(text):
            pending.append((path.parent / target).resolve())
        for target in RUNTIME_DATA.findall(text):
            pending.append((path.parent / target).resolve())
    return sorted(path for path in found if path.is_file())


def resource_files(folder):
    for path in sorted(folder.rglob("*")):
        if path.is_file() and "node_modules" not in path.parts:
            yield path


def dotnet_files(folder):
    """The files of a built .NET runtime folder that run: its _framework folder."""
    framework = folder / "_framework"
    if not (framework / "dotnet.js").is_file():
        fail(f"no .NET runtime in {folder}: build it with Dev/tools/build_csharp_host.py")
    return sorted(path for path in framework.rglob("*") if path.is_file())


def template_files():
    """The C# starter project's own files (no build output)."""
    for path in sorted(CSHARP_TEMPLATE.rglob("*")):
        parts = path.relative_to(CSHARP_TEMPLATE).parts
        if path.is_file() and "bin" not in parts and "obj" not in parts:
            yield path


def package_client(build, out, version, launcher_config, game_root, logo_header):
    folder = f"KCDS-Client-{version}"
    target = out / f"{folder}.zip"
    pak = branding_pak(game_root, logo_header)
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as archive:
        for source, name in CLIENT_BINARIES:
            add_file(archive, build / source, f"{folder}/{name}")
        for source, name in CLIENT_FILES:
            add_file(archive, source, f"{folder}/{name}")
        for name in CEF_RUNTIME:
            add_file(archive, build / "Client/runtime/Release" / name, f"{folder}/{name}")
        dotnet = build / CLIENT_DOTNET
        for path in dotnet_files(dotnet):
            add_file(archive, path, f"{folder}/dotnet/{path.relative_to(dotnet).as_posix()}")
        for source, name in client_notices(build):
            add_file(archive, source, f"{folder}/{name}")
        archive.writestr(f"{folder}/{BRANDING_PAK}", pak)
        add_text(archive, f"{folder}/kcds-launcher.json", launcher_config)
        add_text(archive, f"{folder}/README.txt", read_template("client-readme.txt", version))
    return target


def add_tar_file(archive, source, name, mode=0o644):
    if not source.is_file():
        fail(f"missing {source}")
    info = archive.gettarinfo(str(source), name)
    info.mode = mode
    info.uid = 0
    info.gid = 0
    info.uname = ""
    info.gname = ""
    with open(source, "rb") as stream:
        archive.addfile(info, stream)


def add_tar_text(archive, name, text, mode=0o644):
    data = text.encode("utf8")
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.mode = mode
    archive.addfile(info, io.BytesIO(data))


def package_server_linux(build, out, version):
    """The Linux server package, as a tar.gz that keeps the exec bits."""
    folder = f"KCDS-Server-{version}"
    target = out / f"{folder}-linux.tar.gz"
    with tarfile.open(target, "w:gz") as archive:
        source, name = LINUX_SERVER_BINARY
        add_tar_file(archive, build / source, f"{folder}/{name}", 0o755)
        source, name = LINUX_VOICE_BINARY
        add_tar_file(archive, build / source, f"{folder}/{name}", 0o755)
        for path in runtime_files() + dotnet_files(SERVER_DOTNET):
            relative = path.relative_to(ROOT).as_posix()
            add_tar_file(archive, path, f"{folder}/runtime/{relative}")
        for resource in EXAMPLE_RESOURCES:
            for path in resource_files(resource):
                relative = path.relative_to(resource.parent).as_posix()
                add_tar_file(archive, path, f"{folder}/resources/{relative}")
        add_tar_text(archive, f"{folder}/resources/README.txt", RESOURCES_README)
        add_tar_text(
            archive,
            f"{folder}/start-server.sh",
            read_template("start-server.sh", version, "\n"),
            0o755,
        )
        add_tar_text(
            archive,
            f"{folder}/README.txt",
            read_template("server-readme.txt", version, "\n"),
        )
        # The server's settings with their documented defaults.
        add_tar_text(
            archive,
            f"{folder}/server.json",
            read_template("server.json", version, "\n"),
        )
    return target


TYPES_README = """KCDS SDK {version}: TypeScript declarations

Generated from the SDK of this release (the same API as https://kcdsynced.com/docs).

  shared.d.ts       the types server and client scripts have alike (Vector3, Quaternion, ...)
  server-api.d.ts   what server scripts have
  client-api.d.ts   what client scripts have

A resource's server scripts are checked with shared.d.ts and server-api.d.ts, its client scripts
with shared.d.ts and client-api.d.ts (two tsconfig projects, or /// <reference path> lines).
Where the API gives no type, "any" stands in.
"""

TYPES_README_LUA = """
For Lua scripts, lua/ has the same API as annotations for the Lua language server (LuaLS):

  lua/kcds-server.lua   what server scripts have
  lua/kcds-client.lua   what client scripts have

List the side's file under workspace.library in the resource's .luarc.json.
"""

TYPES_README_CS = """
For C# scripts, csharp/ has the SDK as a .NET assembly, with its documentation for the editor:

  csharp/Kcds.Sdk.dll    the typed SDK a C# resource is built against (the host has its own copy)
  csharp/Kcds.Sdk.xml    its documentation comments
  csharp/template/       a resource to start from: server/ and client/ projects, package.json

Copy csharp/template to the resource's folder, keep Kcds.Sdk.dll one folder above it (or pass
-p:KcdsSdk=<path>), and run `dotnet build -c Release` in server/ and client/ (the .NET 9 SDK).
The scripts land in bin/server/ and bin/client/, which package.json lists; players get only
bin/client/.
"""


def sdk_contract(version):
    """The SDK's API of this checkout, as extract_sdk_api.cjs reads it from the runtimes."""
    with tempfile.TemporaryDirectory() as folder:
        api = Path(folder) / "api.json"
        extract = ROOT / "Dev" / "tools" / "web" / "extract_sdk_api.cjs"
        result = subprocess.run(
            ["node", str(extract), str(ROOT), str(api), "--version", version],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0 or not api.is_file():
            fail(f"the SDK's API could not be extracted: {result.stderr.strip()[-400:]}")
        return json.loads(api.read_text("utf8"))


def web_tool(name):
    """A generator of Dev/tools/web (build_typescript_declarations, build_lua_annotations)."""
    sys.path.insert(0, str(ROOT / "Dev" / "tools" / "web"))
    return __import__(name)


def types_files(version, contract=None):
    """The SDK's TypeScript declarations of this checkout: {file name: text} (shared.d.ts,
    server-api.d.ts, client-api.d.ts). The zip and the npm package (npm_types.py) ship these."""
    contract = contract if contract is not None else sdk_contract(version)
    return web_tool("build_typescript_declarations").build(contract, version)


def package_types(out, version, build):
    """The SDK for resource authors as KCDS-Types-<version>.zip: the TypeScript declarations, and
    for each language Dev/tools/web/published_languages.py publishes, the Lua annotations (lua)
    and the C# SDK with its starter project (csharp)."""
    languages = web_tool("published_languages").PUBLISHED
    target = out / f"KCDS-Types-{version}.zip"
    contract = sdk_contract(version)
    files = types_files(version, contract)
    if "lua" in languages:
        for name, text in web_tool("build_lua_annotations").build(contract, version).items():
            files[f"lua/{name}"] = text
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, text in files.items():
            add_text(archive, name, text)
        if "cs" in languages:
            # The C# SDK: the assembly the client build made, and a starter project.
            dotnet = build / CLIENT_DOTNET
            add_file(archive, dotnet / "Kcds.Sdk.dll", "csharp/Kcds.Sdk.dll")
            add_file(archive, dotnet / "Kcds.Sdk.xml", "csharp/Kcds.Sdk.xml")
            for path in template_files():
                name = path.relative_to(CSHARP_TEMPLATE).as_posix()
                add_file(archive, path, f"csharp/template/{name}")
        readme = TYPES_README.format(version=version)
        if "lua" in languages:
            readme += TYPES_README_LUA
        if "cs" in languages:
            readme += TYPES_README_CS
        add_text(archive, "README.txt", readme)
    return target


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--version", help="must equal the build's project version (the default)")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--server-address", default="")
    parser.add_argument("--update-origin", default=OFFICIAL_UPDATE_ORIGIN)
    parser.add_argument(
        "--no-updates",
        action="store_true",
        help="ship with launcher updates off (never for a release)",
    )
    parser.add_argument("--sentry-dsn-file", type=Path)
    parser.add_argument(
        "--game",
        type=Path,
        help="KCD2 install root, read for the branding pak (the client package needs it)",
    )
    parser.add_argument(
        "--logo-header",
        type=Path,
        help="file with the game logo's 128-byte DDS header, in place of --game",
    )
    parser.add_argument(
        "--server-linux",
        action="store_true",
        help="package the Linux server from a Linux build; servers run on Linux only",
    )
    parser.add_argument(
        "--installer",
        action="store_true",
        help="also build the client installer KCDS-Setup-<version>.exe (Inno Setup 6)",
    )
    options = parser.parse_args()
    branding_sources = (options.game is not None) + (options.logo_header is not None)
    if not options.server_linux and branding_sources != 1:
        fail("the client package needs one of --game and --logo-header")

    version = build_version(options.build)
    if not options.server_linux and build_is_developer(options.build):
        fail(
            "this is a developer build (KCDS_DEV_BUILD=ON: windowed games with -devmode); package "
            "a build configured with -DKCDS_DEV_BUILD=OFF"
        )
    if options.version is None:
        options.version = version
    elif options.version != version:
        fail(
            f"--version {options.version} differs from the build's version {version}: change "
            "project(VERSION) in CMakeLists.txt and rebuild, so the binaries report the same version"
        )
    if not VERSION_PATTERN.match(options.version):
        fail("version must be MAJOR.MINOR.PATCH")
    if options.server_address and not ADDRESS_PATTERN.match(options.server_address):
        fail("server address must be host:port")
    if options.no_updates or options.server_linux:
        options.update_origin = ""
    elif not options.update_origin:
        fail("a release updates from --update-origin; use --no-updates for a package that is none")
    if options.update_origin:
        if not ORIGIN_PATTERN.match(options.update_origin):
            fail("update origin must be a bare https origin, e.g. https://updates.kcdsynced.com")
        if not build_has_signing(options.build):
            fail("this build cannot verify signed releases (no OpenSSL)")
    sentry_dsn = ""
    if options.sentry_dsn_file:
        sentry_dsn = options.sentry_dsn_file.read_text(encoding="utf8").strip()
        if not DSN_PATTERN.match(sentry_dsn):
            fail("the DSN file does not hold an https client DSN")
    options.out.mkdir(parents=True, exist_ok=True)

    if options.server_linux:
        archive = package_server_linux(options.build, options.out, options.version)
        sums = f"{sha256(archive)}  {archive.name}\n"
        (options.out / "SHA256SUMS-linux.txt").write_text(sums, encoding="utf8")
        print(f"{archive}  {archive.stat().st_size} bytes")
        return

    launcher_config = client_config(options.server_address, options.update_origin, sentry_dsn)
    archives = [
        package_client(
            options.build,
            options.out,
            options.version,
            launcher_config,
            options.game,
            options.logo_header,
        ),
        package_types(options.out, options.version, options.build),
    ]
    if options.installer:
        archives.append(installer.build_installer(archives[0], options.out))
    sums = "".join(f"{sha256(path)}  {path.name}\n" for path in archives)
    (options.out / "SHA256SUMS.txt").write_text(sums, encoding="utf8")
    for path in archives:
        print(f"{path}  {path.stat().st_size} bytes")


if __name__ == "__main__":
    main()
