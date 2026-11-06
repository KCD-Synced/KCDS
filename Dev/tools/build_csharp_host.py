"""Builds the C# resource host: .NET compiled to WebAssembly with the KCDS SDK.

    python Dev/tools/build_csharp_host.py [--out DIR] [--force]

Publishes Backend/scripting/runtimes/csharp/Kcds.Host (which references Kcds.Sdk) for browser-wasm
and copies its _framework folder to DIR (default Backend/scripting/runtimes/js/languages/dotnet,
not committed), where csharp_host.cjs loads it on the server and the client runtime bundle takes
it for players' games. Skips the build when the sources have not changed since the last one
(a hash of them is kept in DIR/build.hash) unless --force.

The runtime's JavaScript is patched after publishing, so that a resource that ends the .NET runtime
(Environment.Exit) cannot end the host's process: in Node the runtime would call process.exit and
set process.exitCode. Both now only throw, which fails that resource.
"""

import argparse
import hashlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCES = ROOT / "Backend" / "scripting" / "runtimes" / "csharp"
DEFAULT_OUT = ROOT / "Backend" / "scripting" / "runtimes" / "js" / "languages" / "dotnet"

# (file pattern, upstream text, replacement): each must be found once, or the build fails.
PATCHES = [
    (
        "dotnet.js",
        "Ue&&He.process?He.process.exit(e):Fe.quit&&Fe.quit(e,t),t",
        "t",
    ),
    (
        "dotnet.native.js",
        "quit_=(status,toThrow)=>{process.exitCode=status;throw toThrow}",
        "quit_=(status,toThrow)=>{throw toThrow}",
    ),
]


def sources_hash():
    digest = hashlib.sha256()
    for path in sorted(SOURCES.rglob("*")):
        if path.is_file() and "bin" not in path.parts and "obj" not in path.parts:
            digest.update(str(path.relative_to(SOURCES)).encode("utf-8"))
            digest.update(path.read_bytes())
    digest.update(Path(__file__).read_bytes())
    return digest.hexdigest()


def patch(framework):
    for pattern, upstream, replacement in PATCHES:
        files = sorted(framework.glob(pattern))
        if len(files) != 1:
            sys.exit(f"build_csharp_host: expected one {pattern}, found {len(files)}")
        text = files[0].read_text(encoding="utf-8")
        if text.count(upstream) != 1:
            sys.exit(f"build_csharp_host: {files[0].name} no longer has the code to patch")
        files[0].write_text(text.replace(upstream, replacement), encoding="utf-8", newline="")


def build(out, force):
    wanted = sources_hash()
    stamp = out / "build.hash"
    if not force and stamp.is_file() and stamp.read_text(encoding="utf-8").strip() == wanted:
        return False
    with tempfile.TemporaryDirectory() as folder:
        published = Path(folder) / "publish"
        command = [
            "dotnet",
            "publish",
            str(SOURCES / "Kcds.Host" / "Kcds.Host.csproj"),
            "-c",
            "Release",
            "-o",
            str(published),
            "--nologo",
        ]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0:
            sys.exit(
                f"build_csharp_host: dotnet publish failed\n{result.stdout[-4000:]}{result.stderr[-2000:]}"
            )
        framework = published / "wwwroot" / "_framework"
        patch(framework)
        if out.exists():
            shutil.rmtree(out)
        shutil.copytree(framework, out / "_framework")
        # The runtime's scripts are ES modules; Node then loads them without guessing.
        (out / "_framework" / "package.json").write_text('{ "type": "module" }\n', encoding="utf-8")
        shutil.copy2(
            SOURCES / "Kcds.Sdk" / "bin" / "Release" / "net9.0" / "Kcds.Sdk.dll",
            out / "Kcds.Sdk.dll",
        )
        shutil.copy2(
            SOURCES / "Kcds.Sdk" / "bin" / "Release" / "net9.0" / "Kcds.Sdk.xml",
            out / "Kcds.Sdk.xml",
        )
    stamp.write_text(wanted + "\n", encoding="utf-8")
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--force", action="store_true")
    options = parser.parse_args()
    built = build(options.out, options.force)
    print(f"build_csharp_host: {'built' if built else 'up to date'} in {options.out}")


if __name__ == "__main__":
    main()
