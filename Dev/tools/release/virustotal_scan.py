#!/usr/bin/env python3
"""Scans a release's files on VirusTotal and writes a report; the release workflow runs it on every
build, before anything is published.

    python Dev/tools/release/virustotal_scan.py --dist <dist> --out <report dir> [--key-file <file>]

It scans every zip and tarball of <dist> and, one by one, every .exe and .dll inside the zips, since
engines often judge an executable and not the archive it came in. A file VirusTotal already knows by
its SHA-256 is not uploaded again. The key comes from --key-file or VIRUSTOTAL_API_KEY.

Writes virustotal.json and virustotal.md to <report dir>. It exits 1 when a file got no result and 0
otherwise: a detection is reported, it does not stop a release. Anything uploaded is kept by
VirusTotal and can be read by its paid subscribers, so this is not a private check.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import sys
import tempfile
import time
import urllib.error
import urllib.request
import uuid
import zipfile

API = "https://www.virustotal.com/api/v3"
GUI_FILE = "https://www.virustotal.com/gui/file/"
# The public API's limit is 4 requests a minute.
REQUEST_INTERVAL_SECONDS = 15.5
RATE_LIMIT_WAIT_SECONDS = 60
MAX_RATE_LIMIT_RETRIES = 10
# Files above this go to an upload URL of their own.
DIRECT_UPLOAD_BYTES = 32 * 1024 * 1024
ANALYSIS_POLL_SECONDS = 60
ANALYSIS_TIMEOUT_SECONDS = 30 * 60
SCANNED_ARCHIVES = (".zip", ".tar.gz")
SCANNED_INSIDE = (".exe", ".dll")


class AlreadyUploading(Exception):
    """VirusTotal already has this file in its queue."""


class VirusTotal:
    """The few calls of VirusTotal's API this needs, at the rate the key is allowed."""

    def __init__(self, key: str):
        self.key = key
        self.last_request = 0.0

    def request(self, url: str, body: bytes | None = None, content_type: str = "") -> dict | None:
        """The answer's JSON, or None for a 404."""
        for _ in range(MAX_RATE_LIMIT_RETRIES):
            wait = self.last_request + REQUEST_INTERVAL_SECONDS - time.monotonic()
            if wait > 0:
                time.sleep(wait)
            self.last_request = time.monotonic()
            headers = {"x-apikey": self.key}
            if content_type:
                headers["Content-Type"] = content_type
            request = urllib.request.Request(url, data=body, headers=headers)
            try:
                with urllib.request.urlopen(request, timeout=600) as answer:
                    return json.loads(answer.read())
            except urllib.error.HTTPError as error:
                if error.code == 404:
                    return None
                # The same file is being analysed for someone else's upload: ask for it later.
                if error.code == 409:
                    raise AlreadyUploading() from None
                if error.code != 429:
                    raise RuntimeError(f"VirusTotal answered {error.code}") from None
                time.sleep(RATE_LIMIT_WAIT_SECONDS)
        raise RuntimeError("VirusTotal kept answering 429")

    def known(self, sha256: str) -> dict | None:
        """The file's attributes, or None when VirusTotal has not seen it."""
        answer = self.request(f"{API}/files/{sha256}")
        return answer["data"]["attributes"] if answer else None

    def upload(self, path: pathlib.Path) -> str:
        """Uploads a file; the id of its analysis."""
        url = f"{API}/files"
        if path.stat().st_size > DIRECT_UPLOAD_BYTES:
            url = self.request(f"{API}/files/upload_url")["data"]
        boundary = uuid.uuid4().hex
        head = (
            f'--{boundary}\r\nContent-Disposition: form-data; name="file"; '
            f'filename="{path.name}"\r\nContent-Type: application/octet-stream\r\n\r\n'
        ).encode()
        tail = f"\r\n--{boundary}--\r\n".encode()
        body = head + path.read_bytes() + tail
        answer = self.request(url, body, f"multipart/form-data; boundary={boundary}")
        return answer["data"]["id"]

    def analysis_finished(self, analysis: str) -> bool:
        answer = self.request(f"{API}/analyses/{analysis}")
        # A new analysis can answer 404 for a moment.
        return answer is not None and answer["data"]["attributes"]["status"] == "completed"


def sha256_of(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def files_to_scan(dist: pathlib.Path, unpacked: pathlib.Path) -> list[tuple[str, pathlib.Path]]:
    """(name in the report, path) of the archives of dist and the executables inside its zips."""
    found = []
    for archive in sorted(dist.iterdir()):
        if not archive.name.endswith(SCANNED_ARCHIVES):
            continue
        found.append((archive.name, archive))
        if archive.suffix != ".zip":
            continue
        with zipfile.ZipFile(archive) as opened:
            for inner in opened.namelist():
                if not inner.lower().endswith(SCANNED_INSIDE):
                    continue
                target = unpacked / archive.stem / pathlib.PurePosixPath(inner).name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(opened.read(inner))
                found.append((f"{archive.name} / {inner}", target))
    return found


def result_of(name: str, sha256: str, attributes: dict | None) -> dict:
    """A file's line of the report, from VirusTotal's attributes; None for no result."""
    result = {"file": name, "sha256": sha256, "link": GUI_FILE + sha256}
    stats = attributes.get("last_analysis_stats", {}) if attributes else {}
    # Stats of zeros mean no engine has judged it yet: that is no result, not a clean file.
    if not sum(stats.values()):
        result["error"] = "no result"
        return result
    engines = attributes.get("last_analysis_results", {})
    result["malicious"] = stats.get("malicious", 0)
    result["suspicious"] = stats.get("suspicious", 0)
    result["engines"] = sum(stats.values())
    result["detections"] = {
        engine: verdict.get("result") or verdict.get("category")
        for engine, verdict in sorted(engines.items())
        if verdict.get("category") in ("malicious", "suspicious")
    }
    return result


def judged(attributes: dict | None) -> bool:
    """Whether any engine has judged the file."""
    return bool(attributes) and sum(attributes.get("last_analysis_stats", {}).values()) > 0


def scan(client: VirusTotal, name: str, path: pathlib.Path) -> dict:
    """A file's result, uploading it when VirusTotal does not know it."""
    sha256 = sha256_of(path)
    try:
        attributes = client.known(sha256)
        if not judged(attributes):
            deadline = time.monotonic() + ANALYSIS_TIMEOUT_SECONDS
            try:
                analysis = client.upload(path)
                while not client.analysis_finished(analysis):
                    if time.monotonic() > deadline:
                        raise RuntimeError("the analysis did not finish in time")
                    time.sleep(ANALYSIS_POLL_SECONDS)
            except AlreadyUploading:
                pass
            # The file's own record can follow its finished analysis by a while.
            attributes = client.known(sha256)
            while not judged(attributes):
                if time.monotonic() > deadline:
                    raise RuntimeError("no engine judged it in time")
                time.sleep(ANALYSIS_POLL_SECONDS)
                attributes = client.known(sha256)
    except (RuntimeError, OSError) as error:
        result = result_of(name, sha256, None)
        result["error"] = str(error)
        return result
    return result_of(name, sha256, attributes)


def markdown_of(results: list[dict]) -> str:
    flagged = [r for r in results if r.get("malicious") or r.get("suspicious")]
    failed = [r for r in results if "error" in r]
    lines = [
        "# VirusTotal",
        "",
        f"{len(results)} files: {len(flagged)} flagged, {len(failed)} without a result.",
        "",
        "| File | Malicious | Suspicious | Engines | Link |",
        "| --- | --- | --- | --- | --- |",
    ]
    for result in results:
        if "error" in result:
            lines.append(
                f"| {result['file']} | {result['error']} | | | [report]({result['link']}) |"
            )
            continue
        lines.append(
            f"| {result['file']} | {result['malicious']} | {result['suspicious']} | "
            f"{result['engines']} | [report]({result['link']}) |"
        )
    for result in flagged:
        lines += ["", f"## {result['file']}", ""]
        for engine, verdict in result["detections"].items():
            lines.append(f"- {engine}: {verdict}")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dist", required=True, type=pathlib.Path)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--key-file", type=pathlib.Path)
    args = parser.parse_args()
    key = os.environ.get("VIRUSTOTAL_API_KEY", "")
    if args.key_file:
        key = args.key_file.read_text().strip()
    if not key:
        print("no VirusTotal key: --key-file or VIRUSTOTAL_API_KEY", file=sys.stderr)
        return 1
    client = VirusTotal(key)
    results = []
    with tempfile.TemporaryDirectory() as unpacked:
        targets = files_to_scan(args.dist, pathlib.Path(unpacked))
        for name, path in targets:
            result = scan(client, name, path)
            results.append(result)
            print(
                f"{name}: {result.get('error') or str(result['malicious']) + ' malicious'}",
                flush=True,
            )
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "virustotal.json").write_text(json.dumps(results, indent=2) + "\n")
    (args.out / "virustotal.md").write_text(markdown_of(results), encoding="utf-8")
    if not results or any("error" in result for result in results):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
