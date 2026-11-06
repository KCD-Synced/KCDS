"""Pack the website's docs bundle: one JSON file the kcds-website server reads.

    python Dev/tools/web/build_docs_bundle.py --api sdk-api.json --out docs-bundle.json
        [--releases <folder of <version>.md release notes>]
    python Dev/tools/web/build_docs_bundle.py --sdk <cmake build dir>/sdk --out docs-bundle.json

It holds the API reference (each member also as Lua and C# write it, language_signatures.py),
the public docs (the KCDS-DOCS repository: KCDS_DOCS_DIR, or ../KCDS-DOCS next to this checkout),
each with only the resource languages and features a release publishes (published_languages.py),
the release notes of --releases and each release tag's date. The reference is the one
extract_sdk_api.cjs reads from the server and client runtimes (--api), or the one kcd2s-gen wrote
on the build from the IDL (<sdk>/docs/api.json).
Nothing private goes in: KCDS-DOCS is the public documentation, and release notes are published
with every release (the GitHub releases).
"""

import argparse
import datetime
import json
import os
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

from language_signatures import add_signatures  # noqa: E402
from published_languages import cut_api, cut_markdown, guide_published  # noqa: E402

ROOT = Path(__file__).resolve().parents[3]
VERSION = re.compile(r"^\d+(\.\d+)+$")


def fail(message):
    print("build_docs_bundle: " + message, file=sys.stderr)
    sys.exit(1)


def git(*arguments):
    result = subprocess.run(
        ["git", "-C", str(ROOT), *arguments], capture_output=True, text=True, check=True
    )
    return result.stdout.strip()


def markdown_files(folder, name_pattern=None):
    files = {}
    for path in sorted(folder.glob("*.md")):
        # The docs repository's own front page and licence are not docs pages.
        if path.stem.lower() in ("readme", "license"):
            continue
        if name_pattern and not name_pattern.match(path.stem):
            continue
        files[path.stem.lower()] = path.read_text(encoding="utf-8")
    return files


def public_docs(folder):
    """KCDS-DOCS as the published resource languages have it (published_languages.py)."""
    return {
        name: cut_markdown(text)
        for name, text in markdown_files(folder).items()
        if guide_published(name)
    }


def release_dates():
    """v<version> tags -> [{version, date}], the date the tag was made."""
    lines = git("tag", "--list", "v*", "--format=%(refname:short) %(creatordate:iso-strict)")
    dates = []
    for line in lines.splitlines():
        tag, _, date = line.partition(" ")
        version = tag[1:]
        if VERSION.match(version) and date:
            dates.append({"version": version, "date": date})
    return dates


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--api", help="the reference extract_sdk_api.cjs wrote")
    source.add_argument("--sdk", help="kcd2s-gen's output folder (<build>/sdk)")
    parser.add_argument("--out", required=True, help="the bundle file to write")
    parser.add_argument("--releases", help="a folder of release notes, one <version>.md each")
    options = parser.parse_args()

    docs_dir = Path(os.environ.get("KCDS_DOCS_DIR") or ROOT.parent / "KCDS-DOCS")
    if not docs_dir.is_dir():
        fail(f"{docs_dir} is missing: check out KCD-Synced/KCDS-DOCS there or set KCDS_DOCS_DIR")
    releases = markdown_files(Path(options.releases), VERSION) if options.releases else {}

    if options.api:
        api_file = Path(options.api)
        if not api_file.is_file():
            fail(f"{api_file} is missing: run Dev/tools/web/extract_sdk_api.cjs first")
    else:
        api_file = Path(options.sdk) / "docs" / "api.json"
        if not api_file.is_file():
            fail(f"{api_file} is missing: build the kcd2s_sdk_docs target first")
    api = json.loads(api_file.read_text(encoding="utf-8"))
    if api.get("schema") != 1:
        fail(f"{api_file}: schema {api.get('schema')!r}, this bundle needs 1")
    # Each member as Lua and C# scripts write it, for the reference's language tabs; then only
    # the languages a release publishes (published_languages.py).
    add_signatures(api)
    cut_api(api)

    bundle = {
        "schema": 1,
        "commit": os.environ.get("CI_COMMIT_SHA") or git("rev-parse", "HEAD"),
        "ref": os.environ.get("CI_COMMIT_REF_NAME") or git("rev-parse", "--abbrev-ref", "HEAD"),
        "generatedAt": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
        "api": api,
        "docs": public_docs(docs_dir),
        "releases": releases,
        "releaseDates": release_dates(),
    }
    out = Path(options.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    temporary = out.with_name(out.name + ".tmp")
    temporary.write_text(json.dumps(bundle, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    temporary.replace(out)
    print(
        f"build_docs_bundle: {len(api.get('types', []))} API types, {len(bundle['docs'])} docs, "
        f"{len(bundle['releases'])} releases ({len(bundle['releaseDates'])} tagged) -> {out}"
    )


if __name__ == "__main__":
    main()
