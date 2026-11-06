"""Publish a packaged KCDS client to the launcher update feed (updates.kcdsynced.com).

    python Dev/tools/release/update_feed.py --version 0.1.1 --dist <package.py output>
        [--channel stable|preview] [--dry-run]

The launcher (Client/launcher/include/kcds/launcher/feed.hpp) reads

    GET {updateOrigin}/v1/updates/{channel}/manifest   signed Ed25519 release envelope
    GET {updateOrigin}/v1/updates/{channel}/versions/{version}/manifest
                                                       one KCDS version's envelope
    GET {updateOrigin}/v1/artifacts/{sha256}           raw file bytes

which the update host serves statically from /var/www/kcds-updates/{channel}/manifest.json,
/var/www/kcds-updates/{channel}/versions/{version}/manifest.json and
/var/www/kcds-updates/artifacts/{sha256}. Artifacts stay, so every release a channel ever served
can still be installed: a launcher started for a server of an older version (kcds-launcher
--version) fetches that release's envelope. This tool:

1. takes every file of KCDS-Client-<version>.zip except kcds-launcher.json (the deployment
   configuration is never a managed file);
2. reads the channel's current manifest from the update host and uses the next sequence number
   (the launcher refuses a lower sequence: anti-rollback);
3. signs the manifest with System-Backend/distribution/publish.cjs and the private key
   ~/.kcds/kcds-update-signing-key.pem (key id KCDS_UPDATE_KEY_ID, default kcds-2026-10),
   checked against the public keys in update-publisher-keys.json;
4. uploads the new artifacts first, then the release's envelope under versions/<version>/ and
   the one it replaces under its own version when that is not there yet, then replaces the
   manifest with one rename, so a client never sees a manifest whose files are missing;
5. checks over HTTPS that the served manifest is the signed one and every artifact answers 200.

The Linux server package goes to its own channel of the same feed (KCDS-Admin's
one-click update, Backend/scripting/runtimes/js/admin/server_updates.cjs): server-linux-<channel>
from KCDS-Server-<version>-linux.tar.gz, with only the files an update may replace: the server
binary, runtime/ and README.txt (never the owner's server.json, resources or start script).
Servers are hosted on Linux only; there is no Windows server channel.

The private key is read from its file only; it is never printed or uploaded.
"""

import argparse
import base64
import re
import json
import os
from pathlib import Path, PurePosixPath
import secrets
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request
import zipfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
# The feed publisher of the services (the private repository KCD-Synced/KCDS-Master, checked out
# by the release workflow as System-Backend/).
PUBLISHER = Path(
    os.environ.get("KCDS_FEED_PUBLISHER") or ROOT / "System-Backend/distribution/publish.cjs"
)
PUBLIC_KEYS = HERE / "update-publisher-keys.json"
SIGNING_KEY = Path(
    os.environ.get("KCDS_UPDATE_SIGNING_KEY_FILE")
    or Path.home() / ".kcds/kcds-update-signing-key.pem"
)
KEY_ID = os.environ.get("KCDS_UPDATE_KEY_ID", "kcds-2026-10")
SSH_KEY = Path(os.environ.get("KCDS_RELEASE_SSH_KEY_FILE") or Path.home() / ".ssh/kcds_vps_ed25519")
# The release host's user@host (required to publish; publish.py checks it).
SSH_TARGET = os.environ.get("KCDS_RELEASE_SSH", "")
REMOTE_ROOT = os.environ.get("KCDS_UPDATE_DIR", "/var/www/kcds-updates")
UPDATE_ORIGIN = os.environ.get("KCDS_UPDATE_ORIGIN", "https://updates.kcdsynced.com")
CHANNELS = ("stable", "preview")
# Files of the client zip that the launcher owns locally and must never be in a release.
NOT_MANAGED = {"kcds-launcher.json"}
PROTOCOL_VERSION = 1


def fail(message):
    print("update-feed: " + message, file=sys.stderr)
    sys.exit(1)


def ssh_options():
    return ["-i", str(SSH_KEY), "-o", "BatchMode=yes", "-o", "ConnectTimeout=15"]


def remote(command, check=True):
    # The command goes through the remote shell's standard input: the moves of a release with
    # many new artifacts are longer than a Windows command line may be (0.3.2's server feed).
    return subprocess.run(
        ["ssh", *ssh_options(), SSH_TARGET, "sh -s"],
        input=command,
        check=check,
        capture_output=True,
        text=True,
    )


def extract_client(zip_path, version, target):
    """Unpacks the client zip without its top folder; returns [(managed path, file)]."""
    prefix = f"KCDS-Client-{version}/"
    files = []
    with zipfile.ZipFile(zip_path) as archive:
        for entry in archive.infolist():
            if entry.is_dir():
                continue
            if not entry.filename.startswith(prefix):
                fail(f"unexpected entry {entry.filename} in {zip_path.name}")
            managed = entry.filename[len(prefix) :]
            if managed in NOT_MANAGED:
                continue
            if PurePosixPath(managed).is_absolute() or ".." in PurePosixPath(managed).parts:
                fail(f"unsafe path {managed} in {zip_path.name}")
            destination = target / managed
            destination.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(entry) as source, open(destination, "wb") as output:
                shutil.copyfileobj(source, output)
            files.append((managed, destination))
    if not files:
        fail(f"{zip_path.name} contains no files")
    return sorted(files)


# The server binary of the Linux server package.
SERVER_BINARY = "kcd2s-server"


def server_managed(name):
    """Whether a file of the server package is one the server's own update replaces."""
    return name in (SERVER_BINARY, "README.txt") or name.startswith("runtime/")


def extract_server(package, version, target):
    """The Linux server package's managed files without its top folder: [(managed path, file)]."""
    prefix = f"KCDS-Server-{version}/"
    files = []
    with tarfile.open(package, "r:gz") as archive:
        for entry in archive.getmembers():
            if not entry.isfile():
                continue
            if not entry.name.startswith(prefix):
                fail(f"unexpected entry {entry.name} in {package.name}")
            managed = entry.name[len(prefix) :]
            if PurePosixPath(managed).is_absolute() or ".." in PurePosixPath(managed).parts:
                fail(f"unsafe path {managed} in {package.name}")
            if not server_managed(managed):
                continue
            destination = target / managed
            destination.parent.mkdir(parents=True, exist_ok=True)
            with archive.extractfile(entry) as source, open(destination, "wb") as output:
                shutil.copyfileobj(source, output)
            files.append((managed, destination))
    if not any(name == SERVER_BINARY for name, _ in files):
        fail(f"{package.name} has no server binary")
    return sorted(files)


def current_manifest(channel):
    """The channel's live signed envelope as text, or None for a new channel."""
    result = remote(f"cat '{REMOTE_ROOT}/{channel}/manifest.json'", check=False)
    if result.returncode != 0:
        return None
    return result.stdout


def envelope_sequence(envelope_text):
    envelope = json.loads(envelope_text)
    manifest = json.loads(base64.b64decode(envelope["payload"]))
    return manifest["sequence"], manifest["release"]


def release_label(version):
    """The manifest's release for a version. Launchers accept only x.y.z or x.y.z-tag
    (the launcher's is_release_version), so a hotfix x.y.z.w goes out as
    x.y.z-hotfix.w; the label is never shown or compared (rollback protection is the sequence).
    """
    parts = version.split(".")
    if len(parts) == 4:
        return f"{parts[0]}.{parts[1]}.{parts[2]}-hotfix.{parts[3]}"
    return version


def sign_release(feed_root, channel, version, sequence, files, previous):
    """Runs the System-Backend publisher into a local feed directory."""
    if previous is not None:
        (feed_root / channel).mkdir(parents=True, exist_ok=True)
        (feed_root / channel / "manifest.json").write_text(previous, encoding="utf8")
    config = {
        "schemaVersion": 1,
        "releaseRoot": str(feed_root),
        "publicKeys": json.loads(PUBLIC_KEYS.read_text(encoding="utf8")),
    }
    release = {
        "channel": channel,
        "release": release_label(version),
        "sequence": sequence,
        "protocolVersion": PROTOCOL_VERSION,
        "keyId": KEY_ID,
        "files": [{"path": managed, "source": str(path)} for managed, path in files],
    }
    config_path = feed_root.parent / "feed-config.json"
    release_path = feed_root.parent / "release-input.json"
    config_path.write_text(json.dumps(config), encoding="utf8")
    release_path.write_text(json.dumps(release), encoding="utf8")
    subprocess.run(
        ["node", str(PUBLISHER), str(config_path), str(release_path), str(SIGNING_KEY)],
        check=True,
    )
    return (feed_root / channel / "manifest.json").read_text(encoding="utf8")


def version_of(label):
    """The KCDS version of a manifest's release (release_label undone): x.y.z-hotfix.w is
    x.y.z.w."""
    match = re.fullmatch(r"([0-9]+\.[0-9]+\.[0-9]+)-hotfix\.([0-9]+)", label)
    return f"{match.group(1)}.{match.group(2)}" if match else label


def upload(feed_root, channel, envelope_text, version, previous_release):
    manifest = json.loads(base64.b64decode(json.loads(envelope_text)["payload"]))
    hashes = sorted({file["sha256"] for file in manifest["files"]})
    previous = version_of(previous_release) if previous_release else None
    # The versions name folders on the update host: digits and dots only.
    for name in (version, previous):
        if name is not None and not re.fullmatch(r"[0-9]+(\.[0-9]+)*", name):
            fail(f"version {name!r} cannot name a folder")
    release = version
    present = set(remote(f"ls '{REMOTE_ROOT}/artifacts'").stdout.split())
    missing = [digest for digest in hashes if digest not in present]
    incoming = f"{REMOTE_ROOT}/.incoming-{secrets.token_hex(8)}"
    print(f"upload: {len(missing)} new artifacts of {len(hashes)}, then the {channel} manifest")

    remote(f"mkdir -m 700 '{incoming}'")
    sources = [str(feed_root / "artifacts" / digest) for digest in missing]
    sources.append(str(feed_root / channel / "manifest.json"))
    subprocess.run(["scp", *ssh_options(), *sources, f"{SSH_TARGET}:{incoming}/"], check=True)

    # Artifacts first, then the release's own envelope and the one it replaces under
    # its release when that has none yet, the manifest last; each step is a rename on one file
    # system.
    versions = f"{REMOTE_ROOT}/{channel}/versions"
    moves = [f"chmod 644 '{incoming}'/*"]
    for digest in missing:
        moves.append(f"mv -f '{incoming}/{digest}' '{REMOTE_ROOT}/artifacts/{digest}'")
    moves.append(f"mkdir -p '{REMOTE_ROOT}/{channel}' '{versions}/{release}'")
    if previous:
        kept = f"{versions}/{previous}/manifest.json"
        moves.append(
            f"if [ -f '{REMOTE_ROOT}/{channel}/manifest.json' ] && [ ! -f '{kept}' ]; then "
            f"mkdir -p '{versions}/{previous}' && "
            f"cp '{REMOTE_ROOT}/{channel}/manifest.json' '{kept}.part' && "
            f"mv -f '{kept}.part' '{kept}'; fi"
        )
    moves.append(f"cp '{incoming}/manifest.json' '{versions}/{release}/manifest.json.part'")
    moves.append(
        f"mv -f '{versions}/{release}/manifest.json.part' '{versions}/{release}/manifest.json'"
    )
    moves.append(f"mv -f '{incoming}/manifest.json' '{REMOTE_ROOT}/{channel}/manifest.json'")
    moves.append(f"rmdir '{incoming}'")
    remote(" && ".join(moves))
    return hashes


def fetch(url, method="GET"):
    request = urllib.request.Request(url, method=method)
    request.add_header("User-Agent", "kcds-release-publisher")
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            return response.status, response.read() if method == "GET" else b""
    except urllib.error.HTTPError as error:
        return error.code, b""


def verify(channel, envelope_text, hashes, version):
    origin = UPDATE_ORIGIN.rstrip("/")
    status, body = fetch(f"{origin}/v1/updates/{channel}/manifest")
    if status != 200 or body.decode("utf8") != envelope_text:
        fail(f"the served {channel} manifest is not the signed one (HTTP {status})")
    # The release's own envelope: the launchers' route serves the client channels.
    if channel in CHANNELS:
        status, body = fetch(f"{origin}/v1/updates/{channel}/versions/{version}/manifest")
        if status != 200 or body.decode("utf8") != envelope_text:
            fail(f"the served {channel} {version} envelope is not the signed one (HTTP {status})")
    for digest in hashes:
        status, _ = fetch(f"{origin}/v1/artifacts/{digest}", method="HEAD")
        if status != 200:
            fail(f"artifact {digest} is not served (HTTP {status})")
    print(f"verify: {origin} serves the {channel} manifest and all {len(hashes)} artifacts")


def publish_update_feed(version, dist, channel, dry_run):
    if channel not in CHANNELS:
        fail("channel must be stable or preview")
    zip_path = dist / f"KCDS-Client-{version}.zip"
    if not zip_path.is_file():
        fail(f"missing {zip_path}; run package.py first")
    with tempfile.TemporaryDirectory(prefix="kcds-feed-") as scratch:
        scratch = Path(scratch)
        files = extract_client(zip_path, version, scratch / "files")
        print(f"update-feed: {len(files)} managed files from {zip_path.name}")
        publish_channel(channel, version, files, scratch, dry_run)


def publish_server_feed(version, dist, channel, dry_run):
    """The Linux server package in dist, to its channel server-linux-<channel>."""
    if channel not in CHANNELS:
        fail("channel must be stable or preview")
    package = dist / f"KCDS-Server-{version}-linux.tar.gz"
    server_channel = f"server-linux-{channel}"
    if not package.is_file():
        print(f"update-feed: no {package.name}; {server_channel} stays as it is")
        return
    with tempfile.TemporaryDirectory(prefix="kcds-feed-") as scratch:
        scratch = Path(scratch)
        files = extract_server(package, version, scratch / "files")
        print(f"update-feed: {len(files)} managed files from {package.name}")
        publish_channel(server_channel, version, files, scratch, dry_run)


def publish_channel(channel, version, files, scratch, dry_run):
    """Signs the files as the channel's next release, uploads and verifies it."""
    if not dry_run and not SIGNING_KEY.is_file():
        fail(f"signing key missing: {SIGNING_KEY}")
    previous = current_manifest(channel)
    sequence = 1
    release = None
    if previous is not None:
        current, release = envelope_sequence(previous)
        print(f"update-feed: {channel} currently serves {release} (sequence {current})")
        sequence = current + 1
    print(f"update-feed: {channel} {release_label(version)} as sequence {sequence}")
    if dry_run:
        return
    envelope_text = sign_release(scratch / "feed", channel, version, sequence, files, previous)
    hashes = upload(scratch / "feed", channel, envelope_text, version, release)
    verify(channel, envelope_text, hashes, version)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--version", required=True)
    parser.add_argument("--dist", required=True, type=Path)
    parser.add_argument("--channel", default="stable", choices=CHANNELS)
    parser.add_argument("--dry-run", action="store_true")
    options = parser.parse_args()
    publish_update_feed(options.version, options.dist, options.channel, options.dry_run)
    publish_server_feed(options.version, options.dist, options.channel, options.dry_run)


if __name__ == "__main__":
    main()
