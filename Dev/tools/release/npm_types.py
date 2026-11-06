"""The SDK's TypeScript declarations as the npm package @kcds/types.

    python Dev/tools/release/npm_types.py --version 0.4.0 --license MIT --out <dir> [--pack]
        [--from-zip KCDS-Types-<version>.zip]

Writes <dir>/kcds-types/, a package folder with the same three declaration files as
KCDS-Types-<version>.zip (generated from this checkout, package.py's types_files, or with
--from-zip taken from a released zip, for a version already out), one entry file per side and
the package's README.md and package.json:

    shared.d.ts       the types server and client scripts have alike
    server-api.d.ts   what server scripts have
    client-api.d.ts   what client scripts have
    server.d.ts       a server project's entry: "types": ["@kcds/types/server"]
    client.d.ts       a client project's entry: "types": ["@kcds/types/client"]

--pack also runs npm pack, which writes <dir>/kcds-types-<npm version>.tgz. To publish it:

    npm publish <dir>/kcds-types-<npm version>.tgz --access public --tag latest

The package's version is the KCDS version. npm takes three numbers only, so a hotfix x.y.z.w is
x.y.z-hotfix.w, the same name the update feed gives it; --tag latest makes it the default install
even so.
"""

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import package  # noqa: E402  (sibling release tool)

HERE = Path(__file__).resolve().parent
NAME = "@kcds/types"
FOLDER = "kcds-types"
SITE = "https://kcdsynced.com"
DOCS = f"{SITE}/docs/typescript"
DECLARATIONS = ("shared.d.ts", "server-api.d.ts", "client-api.d.ts")
SIDES = ("server", "client")


def fail(message):
    print(f"npm_types: {message}", file=sys.stderr)
    sys.exit(1)


def npm_version(version):
    """The npm version of a KCDS version: x.y.z stays, a hotfix x.y.z.w is x.y.z-hotfix.w."""
    match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)(?:\.(\d+))?", version)
    if not match:
        raise ValueError(f"{version!r} is not a KCDS version (x.y.z or x.y.z.w)")
    base = ".".join(match.group(1, 2, 3))
    hotfix = match.group(4)
    if hotfix is None or hotfix == "0":
        return base
    return f"{base}-hotfix.{hotfix}"


def entry_file(side, version):
    """A side's entry: only references, so the package stays global declarations, not a module."""
    return (
        f"// KCDS SDK {version}: the declarations {side} scripts have.\n"
        f'/// <reference path="./shared.d.ts" />\n'
        f'/// <reference path="./{side}-api.d.ts" />\n'
    )


def manifest(version, license_name):
    exports = {}
    for side in SIDES:
        exports[f"./{side}"] = {"types": f"./{side}.d.ts"}
    exports["./package.json"] = "./package.json"
    return {
        "name": NAME,
        "version": npm_version(version),
        "description": f"TypeScript declarations of the KCDS {version} SDK, "
        "for the server and client scripts of KCDS resources.",
        "keywords": [
            "kcds",
            "kingdom-come-deliverance-2",
            "multiplayer",
            "typescript",
            "types",
        ],
        "homepage": DOCS,
        "license": license_name,
        "files": [*DECLARATIONS, *(f"{side}.d.ts" for side in SIDES), "README.md"],
        "exports": exports,
    }


def public_header(text, version):
    """The declarations' first line as the generator writes it now; zips before 0.4.0 named a
    repository path there."""
    first, newline, rest = text.partition("\n")
    if first.startswith(f"// KCDS SDK {version}: generated from"):
        first = f"// KCDS SDK {version}: generated from the SDK's own API, as {SITE}/docs shows it."
    return first + newline + rest


def zip_declarations(path):
    """The three declaration files of a KCDS-Types-<version>.zip."""
    with zipfile.ZipFile(path) as archive:
        names = set(archive.namelist())
        return {name: archive.read(name).decode("utf8") for name in DECLARATIONS if name in names}


def readme(version):
    text = (HERE / "npm-types-readme.md").read_text("utf8")
    text = text.replace("{npm_version}", npm_version(version))
    text = text.replace("{version}", version)
    return text.replace("{docs}", DOCS)


def package_files(declarations, version, license_name):
    """{file name: text} of the package, from the three generated declaration files."""
    missing = [name for name in DECLARATIONS if name not in declarations]
    if missing:
        raise ValueError(f"the declarations lack {', '.join(missing)}")
    files = {}
    for name in DECLARATIONS:
        files[name] = public_header(declarations[name], version)
    for side in SIDES:
        files[f"{side}.d.ts"] = entry_file(side, version)
    files["README.md"] = readme(version)
    files["package.json"] = json.dumps(manifest(version, license_name), indent=2) + "\n"
    return files


def write_package(out, version, license_name, from_zip=None):
    folder = out / FOLDER
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir(parents=True)
    if from_zip:
        declarations = zip_declarations(from_zip)
    else:
        declarations = package.types_files(version)
    files = package_files(declarations, version, license_name)
    for name, text in files.items():
        (folder / name).write_text(text, encoding="utf8", newline="\n")
    return folder


def pack(folder, out):
    npm = shutil.which("npm")
    if not npm:
        fail("npm is not on PATH")
    result = subprocess.run(
        [npm, "pack", "--pack-destination", str(out.resolve())],
        cwd=folder,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        fail(f"npm pack failed: {result.stderr.strip()[-400:]}")
    return out / result.stdout.strip().splitlines()[-1]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--version", required=True, help="the KCDS version, x.y.z or x.y.z.w")
    parser.add_argument("--license", required=True, help="the package's SPDX licence, e.g. MIT")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--pack", action="store_true", help="also run npm pack")
    parser.add_argument("--from-zip", type=Path, help="a released KCDS-Types-<version>.zip")
    options = parser.parse_args()
    try:
        npm_version(options.version)
    except ValueError as error:
        fail(str(error))
    if options.from_zip and options.from_zip.name != f"KCDS-Types-{options.version}.zip":
        fail(f"{options.from_zip.name} is not the Types zip of {options.version}")
    options.out.mkdir(parents=True, exist_ok=True)
    try:
        folder = write_package(options.out, options.version, options.license, options.from_zip)
    except ValueError as error:
        fail(str(error))
    print(folder)
    if options.pack:
        print(pack(folder, options.out))


if __name__ == "__main__":
    main()
