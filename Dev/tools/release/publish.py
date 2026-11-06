"""Publish a packaged KCDS release: downloads, update feed, GitHub release, Discord announcement.

Releases are published by the GitHub release workflow (.github/workflows/release.yml), which runs
this tool after it built the zips; it is not run by hand.

    python Dev/tools/release/publish.py --version 0.1.1 --dist <package.py output>
        --ref <commit sha on main> [--download-base https://downloads.kcdsynced.com]
        [--update-channel stable|preview] [--skip-update-feed] [--notes <markdown file>]
        [--require-cdn-purge] [--skip-github-release] [--dry-run]

Steps; a failed run can be repeated (files already published unchanged are kept, an existing
release is kept, and --skip-discord avoids a second announcement):
1. upload the Client and Types zips, the client installer KCDS-Setup-<version>.exe when
   package.py --installer made it, SHA256SUMS.txt and the Linux server
   (KCDS-Server-<version>-linux.tar.gz and SHA256SUMS-linux.txt from package.py --server-linux,
   servers are hosted on Linux only) over SSH to <host>:<dir>/<version>/
   (KCDS_RELEASE_SSH, user@host, required; key ~/.ssh/kcds_vps_ed25519 or the file
   KCDS_RELEASE_SSH_KEY_FILE names) and point <dir>/latest/ (KCDS-Client.zip,
   KCDS-Server-linux.tar.gz, SHA256SUMS.txt, ...) at them with hard links. A published version
   never changes: a file of it that is already on the host with other contents stops the run (a
   fix is a new version);
2. publish the client zip to the launcher update feed (update_feed.py, signing key
   ~/.kcds/kcds-update-signing-key.pem or KCDS_UPDATE_SIGNING_KEY_FILE), and the Linux server
   package to its channel of the same feed (KCDS-Admin's server update); skip both
   with --skip-update-feed;
3. purge the CDN (Cloudflare, CLOUDFLARE_API_TOKEN with Cache Purge on CLOUDFLARE_ZONE_ID) for
   every written URL when a token is configured (--require-cdn-purge: no token is an error), then
   download every file through its public URL and stop on any SHA-256 that differs from the built
   one. The purge is a shortcut, not the safeguard: a published version never changes and nginx
   serves latest/ with no-cache, so the public check is what guarantees the downloads;
4. create tag v<version> at --ref and a GitHub release with the zips attached and links to the
   downloads, or publish the workflow's draft of it (token: KCDS_GITHUB_TOKEN, else
   `gh auth token` of the signed-in GitHub CLI);
5. post a Discord embed with the download links (webhook: ~/.kcds/discord-release-webhook or
   the file KCDS_DISCORD_WEBHOOK_FILE names).

Secrets are read from files only and never printed.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parent))
import update_feed  # noqa: E402  (sibling release tool)

REPOSITORY = os.environ.get("KCDS_RELEASE_REPOSITORY", "")
PROJECT_API = f"https://api.github.com/repos/{REPOSITORY}"
UPLOAD_API = f"https://uploads.github.com/repos/{REPOSITORY}"
PROJECT_WEB = f"https://github.com/{REPOSITORY}"
HOME = Path.home()
DISCORD_WEBHOOK = Path(
    os.environ.get("KCDS_DISCORD_WEBHOOK_FILE") or HOME / ".kcds/discord-release-webhook"
)
SSH_KEY = Path(os.environ.get("KCDS_RELEASE_SSH_KEY_FILE") or HOME / ".ssh/kcds_vps_ed25519")
SSH_TARGET = os.environ.get("KCDS_RELEASE_SSH", "")
REMOTE_DIR = os.environ.get("KCDS_RELEASE_DIR", "/var/www/kcds-releases")
DOWNLOAD_BASE = "https://downloads.kcdsynced.com"
# A release (0.1.2) or a hotfix of one (0.1.2.1).
VERSION_PATTERN = re.compile(r"^\d+\.\d+\.\d+(\.\d+)?$")
EMBED_COLOR = 0xB8860B
CLOUDFLARE_API = "https://api.cloudflare.com/client/v4"
# Cloudflare purges at most this many URLs per call.
PURGE_BATCH = 30
# A public download that still serves other bytes after the purge is fetched again this many
# times, this many seconds apart, before the run stops.
PUBLIC_ATTEMPTS = 6
PUBLIC_RETRY_SECONDS = 10


def fail(message):
    print("publish: " + message, file=sys.stderr)
    sys.exit(1)


def secret(path):
    if not path.is_file():
        fail(f"secret file missing: {path}")
    return path.read_text(encoding="utf8").strip()


def request(method, url, data=None, headers=None):
    body = None if data is None else json.dumps(data).encode("utf8")
    message = urllib.request.Request(url, data=body, method=method, headers=headers or {})
    message.add_header("Content-Type", "application/json")
    message.add_header("User-Agent", "kcds-release-publisher")
    try:
        with urllib.request.urlopen(message, timeout=60) as response:
            text = response.read().decode("utf8")
            return response.status, json.loads(text) if text else None
    except urllib.error.HTTPError as error:
        return error.code, error.read(4096).decode("utf8", errors="replace")


def github_token():
    token = os.environ.get("KCDS_GITHUB_TOKEN", "").strip()
    if not token:
        try:
            token = subprocess.run(
                ["gh", "auth", "token"], capture_output=True, text=True, check=True
            ).stdout.strip()
        except (OSError, subprocess.CalledProcessError):
            token = ""
    if not token:
        fail("no GitHub token: set KCDS_GITHUB_TOKEN or sign in with `gh auth login`")
    return token


def github(method, endpoint, data=None):
    headers = {
        "Authorization": f"Bearer {github_token()}",
        "Accept": "application/vnd.github+json",
    }
    return request(method, PROJECT_API + endpoint, data, headers)


def github_upload(release_id, path):
    url = f"{UPLOAD_API}/releases/{release_id}/assets?name={urllib.parse.quote(path.name)}"
    message = urllib.request.Request(url, data=path.read_bytes(), method="POST")
    message.add_header("Authorization", f"Bearer {github_token()}")
    message.add_header("Accept", "application/vnd.github+json")
    message.add_header("Content-Type", "application/octet-stream")
    message.add_header("User-Agent", "kcds-release-publisher")
    try:
        with urllib.request.urlopen(message, timeout=600) as response:
            return response.status
    except urllib.error.HTTPError as error:
        return error.code


def ssh(arguments):
    base = ["-i", str(SSH_KEY), "-o", "BatchMode=yes", "-o", "ConnectTimeout=15"]
    return base + arguments


def digest_of(path):
    sha = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            sha.update(block)
    return sha.hexdigest()


def parse_sums(text):
    """{name: sha256} of "<sha256>  <name>" lines, as sha256sum writes them."""
    sums = {}
    for line in text.splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})\s+\*?(\S+)", line.strip())
        if match:
            sums[match.group(2)] = match.group(1)
    return sums


def published(version):
    """The SHA-256 of each file the download host already has for this version (none for a new
    version)."""
    remote = f"{REMOTE_DIR}/{version}"
    command = f"if [ -d '{remote}' ]; then cd '{remote}' && find . -maxdepth 1 -type f " + (
        "-printf '%f\\0' | xargs -0 -r sha256sum; fi"
    )
    result = subprocess.run(
        ["ssh", *ssh([SSH_TARGET, command])], capture_output=True, text=True, check=True
    )
    return parse_sums(result.stdout)


def unpublished(files, present):
    """The files the host does not have yet. A file it has with other contents stops the run:
    a published version never changes, a fix is a new version."""
    missing = []
    for path in files:
        if path.name not in present:
            missing.append(path)
        elif present[path.name] != digest_of(path):
            fail(
                f"{path.name} is already published with other contents; published files never "
                "change: release the fix as a new version"
            )
    return missing


def upload(files, version, dry_run):
    remote = f"{REMOTE_DIR}/{version}"
    if dry_run:
        print(f"upload: {len(files)} files to {SSH_TARGET}:{remote}/")
        return
    missing = unpublished(files, published(version))
    if not missing:
        print(f"upload: {version} is already published with these {len(files)} files")
        return
    print(f"upload: {len(missing)} of {len(files)} files to {SSH_TARGET}:{remote}/")
    subprocess.run(["ssh", *ssh([SSH_TARGET, f"mkdir -p '{remote}'"])], check=True)
    sources = [str(path) for path in missing]
    subprocess.run(["scp", *ssh([*sources, f"{SSH_TARGET}:{remote}/"])], check=True)


def downloadable(path):
    """A package players or hosts download (a zip, the client installer or the Linux server's
    tar.gz), not a sums file."""
    return path.suffix in (".zip", ".exe") or path.name.endswith(".tar.gz")


def latest_sums(files, version):
    """SHA256SUMS.txt for latest/, naming the version-free link names."""
    lines = []
    for path in files:
        if not downloadable(path):
            continue
        link = path.name.replace("-" + version, "")
        lines.append(f"{digest_of(path)}  {link}\n")
    return "".join(lines)


def point_latest(files, version, dry_run):
    """Hard-links the downloads under <dir>/latest/ (KCDS-Client.zip, KCDS-Server-linux.tar.gz,
    ...) to this version (the landing page links)."""
    print(f"latest: {REMOTE_DIR}/latest -> {version}")
    if dry_run:
        return
    commands = [f"mkdir -p '{REMOTE_DIR}/latest'"]
    for path in files:
        if not downloadable(path):
            continue
        link = path.name.replace("-" + version, "")
        source = f"{REMOTE_DIR}/{version}/{path.name}"
        target = f"{REMOTE_DIR}/latest/{link}"
        temporary = f"{REMOTE_DIR}/latest/.{link}.tmp"
        # A second run for the same version finds latest/ already linked to it; mv refuses to
        # move a file onto its own hard link, so that case keeps the link as it is.
        commands.append(
            f"{{ [ '{source}' -ef '{target}' ] || "
            f"{{ ln -f '{source}' '{temporary}' && mv -f '{temporary}' '{target}'; }}; }}"
        )
    commands.append(
        f"cat > '{REMOTE_DIR}/latest/.SHA256SUMS.txt.tmp' && "
        f"mv -f '{REMOTE_DIR}/latest/.SHA256SUMS.txt.tmp' '{REMOTE_DIR}/latest/SHA256SUMS.txt'"
    )
    subprocess.run(
        ["ssh", *ssh([SSH_TARGET, " && ".join(commands)])],
        # Bytes: text mode would write Windows line endings from a Windows host.
        input=latest_sums(files, version).encode(),
        check=True,
    )


def written_urls(files, version, base, channel, skip_update_feed):
    """Every public URL this run wrote: the version's files, latest/ and the feed manifests."""
    urls = [f"{base}/{version}/{path.name}" for path in files]
    for path in files:
        if downloadable(path):
            urls.append(f"{base}/latest/{path.name.replace('-' + version, '')}")
    urls.append(f"{base}/latest/SHA256SUMS.txt")
    if not skip_update_feed:
        origin = update_feed.UPDATE_ORIGIN.rstrip("/")
        for feed in (channel, f"server-linux-{channel}"):
            urls.append(f"{origin}/v1/updates/{feed}/manifest")
    return urls


def purge_cdn(urls, required, dry_run):
    """Drops the CDN's copies of the written URLs, so nobody gets an older file."""
    token = os.environ.get("CLOUDFLARE_API_TOKEN", "").strip()
    zone = os.environ.get("CLOUDFLARE_ZONE_ID", "").strip()
    if not token or not zone:
        if required:
            fail("CLOUDFLARE_API_TOKEN and CLOUDFLARE_ZONE_ID are needed to purge the CDN")
        print("cdn: no Cloudflare token; the CDN is not purged")
        return
    print(f"cdn: purging {len(urls)} URLs")
    if dry_run:
        return
    headers = {"Authorization": f"Bearer {token}"}
    for start in range(0, len(urls), PURGE_BATCH):
        batch = urls[start : start + PURGE_BATCH]
        status, result = request(
            "POST", f"{CLOUDFLARE_API}/zones/{zone}/purge_cache", {"files": batch}, headers
        )
        if status != 200 or not isinstance(result, dict) or not result.get("success"):
            fail(f"Cloudflare purge failed ({status})")


def public_digest(url):
    """The SHA-256 of what the public URL serves, or None when it does not answer."""
    message = urllib.request.Request(url)
    message.add_header("User-Agent", "kcds-release-publisher")
    sha = hashlib.sha256()
    try:
        with urllib.request.urlopen(message, timeout=600) as response:
            for block in iter(lambda: response.read(1 << 20), b""):
                sha.update(block)
    except (urllib.error.URLError, OSError):
        return None
    return sha.hexdigest()


def verify_public(expected):
    """Downloads every published file through its public URL and stops on one whose bytes are not
    the built ones (a CDN copy of an older file)."""
    for url, digest in expected:
        for attempt in range(1, PUBLIC_ATTEMPTS + 1):
            served = public_digest(url)
            if served == digest:
                break
            if attempt == PUBLIC_ATTEMPTS:
                shown = served[:12] if served else "nothing"
                fail(f"{url} serves {shown}, not the published {digest[:12]}")
            time.sleep(PUBLIC_RETRY_SECONDS)
    print(f"public: all {len(expected)} files match their SHA-256 through the public URLs")


def public_files(files, version, base):
    """(public URL, SHA-256) of every file of the version and of latest/."""
    expected = []
    for path in files:
        digest = digest_of(path)
        expected.append((f"{base}/{version}/{path.name}", digest))
        if downloadable(path):
            expected.append((f"{base}/latest/{path.name.replace('-' + version, '')}", digest))
    sums = latest_sums(files, version).encode("utf8")
    expected.append((f"{base}/latest/SHA256SUMS.txt", hashlib.sha256(sums).hexdigest()))
    return expected


def verify_downloads(links):
    for name, url in links:
        status, _ = request("HEAD", url)
        if status != 200:
            fail(f"download {name} is not reachable ({status}): {url}")
    print("downloads: every link answers 200")


def release_description(version, links, notes):
    lines = [f"KCDS {version}.", ""]
    if notes:
        lines += [notes.strip(), ""]
    lines.append("Downloads (public):")
    lines += [f"- [{name}]({url})" for name, url in links]
    lines += ["", "Unpack the Client zip anywhere and follow its README.txt."]
    return "\n".join(lines)


def github_release(version, ref, links, notes, files, prerelease, dry_run):
    tag = f"v{version}"
    status, existing = github("GET", f"/releases/tags/{urllib.parse.quote(tag)}")
    if status != 200:
        # A draft is not found by its tag: the workflow's draft of this version (its zips job).
        status, listed = github("GET", "/releases?per_page=30")
        drafts = listed if status == 200 and isinstance(listed, list) else []
        existing = next(
            (entry for entry in drafts if entry.get("tag_name") == tag and entry.get("draft")),
            None,
        )
        status = 200 if existing else 404
    if status == 200 and existing.get("draft"):
        print(f"github: publishing the draft release {tag}")
        if dry_run:
            return
        data = {
            "draft": False,
            "body": release_description(version, links, notes),
            "prerelease": prerelease,
        }
        status, result = github("PATCH", f"/releases/{existing['id']}", data)
        if status != 200:
            fail(f"GitHub release could not be published ({status}): {result}")
        return
    if status == 200:
        print(f"github: release {tag} already exists")
        return
    print(f"github: release {tag} at {ref} with {len(files)} files")
    if dry_run:
        return
    data = {
        "tag_name": tag,
        "target_commitish": ref,
        "name": f"KCDS {version}",
        "body": release_description(version, links, notes),
        "prerelease": prerelease,
    }
    status, result = github("POST", "/releases", data)
    if status not in (200, 201):
        fail(f"GitHub release failed ({status}): {result}")
    for path in files:
        uploaded = github_upload(result["id"], path)
        if uploaded not in (200, 201):
            fail(f"GitHub release file {path.name} failed ({uploaded})")


def discord(version, links, notes, dry_run):
    fields = [{"name": name, "value": f"[Download]({url})", "inline": True} for name, url in links]
    description = notes.strip() if notes else f"KCDS {version} is out."
    embed = {
        "title": f"KCDS {version} released",
        "url": f"https://kcdsynced.com/changelog/{version}",
        "description": description[:3500],
        "color": EMBED_COLOR,
        "fields": fields,
    }
    print(f"discord: embed with {len(fields)} download links")
    if dry_run:
        return
    status, _ = request("POST", secret(DISCORD_WEBHOOK), {"embeds": [embed]})
    if status not in (200, 204):
        fail(f"Discord webhook failed ({status})")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--version", required=True)
    parser.add_argument("--dist", required=True, type=Path)
    parser.add_argument("--ref", required=True)
    parser.add_argument("--download-base", default=DOWNLOAD_BASE)
    parser.add_argument("--update-channel", default="stable", choices=("stable", "preview"))
    parser.add_argument("--skip-update-feed", action="store_true")
    parser.add_argument("--notes", type=Path)
    parser.add_argument("--skip-discord", action="store_true")
    # No tag and no GitHub release: a release published from a checkout without a release
    # repository (the downloads, the feed and Discord still go out).
    parser.add_argument("--skip-github-release", action="store_true")
    parser.add_argument("--require-cdn-purge", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    options = parser.parse_args()

    # Nothing goes public that names a partner, a tester or where a fix came from (the check's
    # word list is kept outside the repository; KCDS_PUBLIC_TEXT_CHECK names it when present).
    checker = os.environ.get("KCDS_PUBLIC_TEXT_CHECK")
    if checker:
        guard = [sys.executable, checker]
        if options.notes:
            guard.append(str(options.notes))
        subprocess.run(guard, check=True)

    if not REPOSITORY and not options.skip_github_release:
        fail("set KCDS_RELEASE_REPOSITORY to the owner/name of the release repository")
    if not SSH_TARGET and not options.dry_run:
        fail("set KCDS_RELEASE_SSH to the release host's user@host")
    if not VERSION_PATTERN.match(options.version):
        fail("version must be MAJOR.MINOR.PATCH")
    if not re.fullmatch(r"[0-9a-f]{40}", options.ref):
        fail("--ref must be a full commit sha")
    if not options.download_base.startswith("https://"):
        fail("--download-base must be an https:// URL")

    version = options.version
    # The server is the Linux package only (package.py --server-linux): servers are
    # hosted on Linux and in Docker, there is no Windows server package.
    names = [
        f"KCDS-Client-{version}.zip",
        f"KCDS-Server-{version}-linux.tar.gz",
        "SHA256SUMS.txt",
        f"KCDS-Types-{version}.zip",
        "SHA256SUMS-linux.txt",
    ]
    files = [options.dist / name for name in names]
    for path in files:
        if not path.is_file():
            fail(f"missing {path}; run package.py first")
    base = options.download_base.rstrip("/")
    links = [
        ("Client", f"{base}/{version}/{names[0]}"),
        ("Linux server", f"{base}/{version}/{names[1]}"),
        ("SHA256SUMS", f"{base}/{version}/{names[2]}"),
        ("TypeScript declarations", f"{base}/{version}/{names[3]}"),
    ]
    # The client installer, when package.py --installer made it.
    setup = options.dist / f"KCDS-Setup-{version}.exe"
    if setup.is_file():
        files.append(setup)
        links.insert(1, ("Client installer", f"{base}/{version}/{setup.name}"))
    notes = options.notes.read_text(encoding="utf8") if options.notes else ""

    upload(files, version, options.dry_run)
    if not options.dry_run:
        verify_downloads(links)
    # The feed goes live before latest/ points at the new zips: a client packaged with an
    # update origin only starts once it can verify its install against the feed.
    if not options.skip_update_feed:
        update_feed.publish_update_feed(
            version, options.dist, options.update_channel, options.dry_run
        )
        update_feed.publish_server_feed(
            version, options.dist, options.update_channel, options.dry_run
        )
    point_latest(files, version, options.dry_run)
    # Nobody may download a CDN copy of an older file under these URLs.
    urls = written_urls(files, version, base, options.update_channel, options.skip_update_feed)
    purge_cdn(urls, options.require_cdn_purge, options.dry_run)
    if not options.dry_run:
        verify_public(public_files(files, version, base))
    prerelease = options.update_channel == "preview"
    if options.skip_github_release:
        print("github: skipped (--skip-github-release)")
    else:
        github_release(version, options.ref, links, notes, files, prerelease, options.dry_run)
    if not options.skip_discord:
        discord(version, links, notes, options.dry_run)


if __name__ == "__main__":
    main()
