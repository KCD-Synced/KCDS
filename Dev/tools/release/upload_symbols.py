"""Upload a release build's private debug symbols to the crash server (GlitchTip).

    python Dev/tools/release/upload_symbols.py --build <cmake build dir> [--dry-run]

Every client binary that ships in the Client zip (package.py CLIENT_BINARIES) is uploaded together
with its PDB, so crash reports from that exact build show function names, files and lines. The PDBs
are never shipped. The upload uses the Sentry debug-file protocol that GlitchTip implements:
gzipped SHA-1-named chunks to the organization's chunk-upload endpoint, then difs/assemble per
file; GlitchTip reads each file's debug id itself. Files the server already has are skipped.

Token: ~/.kcds/glitchtip-symbols-token (project:write), or the file --token-file names (the
release workflow's), read from the file and never printed.
"""

import argparse
import gzip
import hashlib
import json
from pathlib import Path
import sys
import time
import urllib.error
import urllib.request
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
import package  # noqa: E402  (sibling release tool: the list of shipped client binaries)

SERVER = "https://crash.kcdsynced.com"
ORGANIZATION = "kcds"
PROJECT = "kcds-client"
TOKEN = Path.home() / ".kcds/glitchtip-symbols-token"
USER_AGENT = "kcds-release-symbols"
# Seconds to wait for GlitchTip to finish assembling the uploaded files.
ASSEMBLE_TIMEOUT = 120
# Shipped third-party binaries (sentry-native's crash handler, Steam Audio's prebuilt phonon.dll):
# uploaded when their PDB exists.
THIRD_PARTY = {"crashpad_handler.exe", "phonon.dll"}


def fail(message):
    print("upload_symbols: " + message, file=sys.stderr)
    sys.exit(1)


token_file = TOKEN


def token():
    if not token_file.is_file():
        fail(f"token file missing: {token_file}")
    return token_file.read_text(encoding="utf8").strip()


def request(method, url, body=None, content_type=None):
    message = urllib.request.Request(url, data=body, method=method)
    message.add_header("Authorization", "Bearer " + token())
    message.add_header("User-Agent", USER_AGENT)
    if content_type:
        message.add_header("Content-Type", content_type)
    try:
        with urllib.request.urlopen(message, timeout=120) as response:
            text = response.read().decode("utf8")
            return json.loads(text) if text else None
    except urllib.error.HTTPError as error:
        fail(f"{method} {url} failed ({error.code}): {error.read(300).decode('utf8', 'replace')}")


def symbol_files(build):
    """The shipped client binaries and their PDBs, as (path, name) pairs."""
    files = []
    for source, name in package.CLIENT_BINARIES:
        binary = build / source
        pdb = binary.with_suffix(".pdb")
        if not binary.is_file():
            fail(f"missing {binary}")
        if not pdb.is_file() and name in THIRD_PARTY:
            print(f"{name}: no PDB (third-party build), skipped")
            continue
        if not pdb.is_file():
            fail(f"missing {pdb}: build Release with the PDBs (see CMakeLists.txt)")
        files.append((binary, name))
        files.append((pdb, Path(name).with_suffix(".pdb").name))
    return files


def chunks_of(data, size):
    return [data[offset : offset + size] for offset in range(0, len(data), size)] or [b""]


def multipart(name, filename, payload):
    boundary = uuid.uuid4().hex
    head = (
        f"--{boundary}\r\n"
        f'Content-Disposition: form-data; name="{name}"; filename="{filename}"\r\n'
        "Content-Type: application/octet-stream\r\n\r\n"
    ).encode("ascii")
    tail = f"\r\n--{boundary}--\r\n".encode("ascii")
    return head + payload + tail, f"multipart/form-data; boundary={boundary}"


def upload(build, dry_run):
    files = symbol_files(build)
    settings = request("GET", f"{SERVER}/api/0/organizations/{ORGANIZATION}/chunk-upload/")
    chunk_size = int(settings["chunkSize"])
    assemble = {}
    for path, name in files:
        data = path.read_bytes()
        pieces = chunks_of(data, chunk_size)
        checksums = [hashlib.sha1(piece).hexdigest() for piece in pieces]
        total = hashlib.sha1(data).hexdigest()
        print(f"{name}: {len(data)} bytes, {len(pieces)} chunk(s)")
        if not dry_run:
            # One chunk per request (GlitchTip's chunksPerRequest is 1); the part's file name is
            # the chunk's SHA-1, which the server verifies after inflating it.
            for piece, checksum in zip(pieces, checksums):
                body, content_type = multipart("file_gzip", checksum, gzip.compress(piece))
                request("POST", settings["url"], body, content_type)
        assemble[total] = {"name": name, "chunks": checksums}
    if dry_run:
        return
    url = f"{SERVER}/api/0/projects/{ORGANIZATION}/{PROJECT}/files/difs/assemble/"
    body = json.dumps(assemble).encode("utf8")
    deadline = time.monotonic() + ASSEMBLE_TIMEOUT
    while True:
        states = request("POST", url, body, "application/json")
        missing = [states[key]["missingChunks"] for key in states if states[key]["missingChunks"]]
        if missing:
            fail(f"the server is missing uploaded chunks: {missing}")
        pending = [key for key in states if states[key]["state"] != "ok"]
        if not pending:
            break
        if time.monotonic() > deadline:
            fail(f"{len(pending)} file(s) not assembled after {ASSEMBLE_TIMEOUT} s")
        time.sleep(3)
    print(f"symbols: {len(assemble)} file(s) on {SERVER} ({ORGANIZATION}/{PROJECT})")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--token-file", type=Path, default=TOKEN)
    options = parser.parse_args()
    global token_file
    token_file = options.token_file
    upload(options.build, options.dry_run)


if __name__ == "__main__":
    main()
