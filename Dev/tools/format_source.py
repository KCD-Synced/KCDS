"""Format maintained source; immutable engine dumps and generated assets are excluded."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
# bin/obj: .NET build output; dotnet: the C# resource host Dev/tools/build_csharp_host.py publishes;
# third_party: the voice engine's vendored libraries.
SKIP = {
    "node_modules",
    "dist",
    "build",
    "vendor",
    "generated",
    "third_party",
    "__pycache__",
    "bin",
    "obj",
    "dotnet",
}
CPP = {".cpp", ".hpp", ".h", ".inl", ".c"}
WEB = {".js", ".cjs", ".mjs", ".ts", ".tsx", ".html", ".css"}


def files():
    result = []
    for folder in ("Client", "Backend", "Dev/tools", "Dev/testkit"):
        for file in (ROOT / folder).rglob("*"):
            if file.is_file() and not SKIP.intersection(file.relative_to(ROOT).parts):
                if file.suffix in CPP | WEB | {".py", ".go"}:
                    result.append(file)
    return sorted(result)


# The longest command line per formatter call. npx is a .CMD file on Windows, run by cmd.exe,
# which refuses command lines over 8191 characters (32767 for other programs).
LONGEST_COMMAND = 7000


def run(command):
    subprocess.run(command, cwd=ROOT, check=True)


def batches(command, paths):
    fixed = sum(len(part) + 1 for part in command)
    batch = []
    length = fixed
    for path in paths:
        if batch and length + len(path) + 1 > LONGEST_COMMAND:
            yield batch
            batch = []
            length = fixed
        batch.append(path)
        length += len(path) + 1
    if batch:
        yield batch


def run_batches(command, paths):
    for batch in batches(command, paths):
        run([*command, *batch])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    options = parser.parse_args()
    source = files()
    cpp = [str(p.relative_to(ROOT)) for p in source if p.suffix in CPP]
    web = [str(p.relative_to(ROOT)) for p in source if p.suffix in WEB]
    python = [str(p.relative_to(ROOT)) for p in source if p.suffix == ".py"]
    go = [str(p.relative_to(ROOT)) for p in source if p.suffix == ".go"]
    print(
        f"Maintained source: C++={len(cpp)}, JS/TS/UI={len(web)}, Python={len(python)}, Go={len(go)}",
        flush=True,
    )
    clang = os.environ.get("KCDS_CLANG_FORMAT", "clang-format")
    version = subprocess.check_output([clang, "--version"], text=True)
    if "19.1.5" not in version:
        raise RuntimeError("Use clang-format 19.1.5 for reproducible formatting")
    if cpp:
        run_batches([clang, *(["--dry-run", "--Werror"] if options.check else ["-i"])], cpp)
    if web:
        prettier = os.environ.get("KCDS_PRETTIER")
        prefix = (
            [shutil.which("node") or "node", prettier]
            if prettier
            else [shutil.which("npx") or "npx", "--no-install", "prettier"]
        )
        run_batches(
            [
                *prefix,
                "--config",
                str(ROOT / ".prettierrc.json"),
                "--plugin-search-dir",
                str(ROOT / "Dev/tools"),
                "--check" if options.check else "--write",
            ],
            web,
        )
    if python:
        run_batches(
            [
                sys.executable,
                "-m",
                "black",
                "--line-length",
                "100",
                *(["--check"] if options.check else []),
            ],
            python,
        )
    gofmt = os.environ.get("KCDS_GOFMT", "gofmt")
    if go:
        if options.check:
            unformatted = "".join(
                subprocess.check_output([gofmt, "-l", *batch], cwd=ROOT, text=True)
                for batch in batches([gofmt, "-l"], go)
            )
            if unformatted.strip():
                raise RuntimeError("gofmt required for:\n" + unformatted)
        else:
            run_batches([gofmt, "-w"], go)


if __name__ == "__main__":
    main()
