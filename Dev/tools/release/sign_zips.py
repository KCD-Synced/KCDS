"""Hand a release's Windows executables to the code-signing step and put the signed ones back.

    python Dev/tools/release/sign_zips.py extract --dist <dist> --work <folder>
    (the signing action signs every file under <folder>/files in place)
    python Dev/tools/release/sign_zips.py pack --dist <dist> --work <folder>

extract copies every Windows executable (a PE image: .exe and .dll) out of the release zips in
<dist> (KCDS-Client-<version>.zip, KCDS-Types-<version>.zip) and every loose one in <dist> (an
installer) to <folder>/files, and lists them in <folder>/manifest.json. A file that already carries
a signature (third-party DLLs their publisher signed: Steam Audio, the DirectX compiler) is left as
it is: signing it again would replace its publisher's signature with ours.

pack checks that every listed file now carries a signature, writes each one back into its zip in
the place of the unsigned one (every other entry, the order, the dates and the compression stay as
they were) or over its loose file, and rewrites the SHA-256 lines of SHA256SUMS*.txt for the files
that changed.
"""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import sys
import zipfile

PE_SUFFIXES = {".exe", ".dll"}
# The PE optional header's data directories: the fifth (index 4) is the certificate table, which
# holds the Authenticode signature.
SECURITY_DIRECTORY = 4
PE32_MAGIC = 0x10B
PE32_PLUS_MAGIC = 0x20B


def fail(message):
    print("sign_zips: " + message, file=sys.stderr)
    sys.exit(1)


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def pe_signature_size(data):
    """The size of a PE image's certificate table (0: unsigned), or None if data is no PE image."""
    if len(data) < 0x40 or data[:2] != b"MZ":
        return None
    header = int.from_bytes(data[0x3C:0x40], "little")
    if header + 24 + 2 > len(data) or data[header : header + 4] != b"PE\0\0":
        return None
    optional = header + 24
    magic = int.from_bytes(data[optional : optional + 2], "little")
    if magic == PE32_MAGIC:
        count_offset = optional + 92
        directories = optional + 96
    elif magic == PE32_PLUS_MAGIC:
        count_offset = optional + 108
        directories = optional + 112
    else:
        return None
    if count_offset + 4 > len(data):
        return None
    count = int.from_bytes(data[count_offset : count_offset + 4], "little")
    if count <= SECURITY_DIRECTORY:
        return 0
    entry = directories + SECURITY_DIRECTORY * 8
    if entry + 8 > len(data):
        return None
    return int.from_bytes(data[entry + 4 : entry + 8], "little")


def is_executable_name(name):
    return PurePosixPath(name).suffix.lower() in PE_SUFFIXES


def safe_entry_path(name):
    """The zip entry's path as relative parts, refusing one that would leave the work folder."""
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts or "\\" in name or ":" in name:
        fail(f"unsafe entry name {name!r}")
    return path.parts


def release_zips(dist):
    return sorted(path for path in dist.glob("*.zip") if path.is_file())


def loose_executables(dist):
    return sorted(
        path for path in dist.iterdir() if path.is_file() and is_executable_name(path.name)
    )


def extract(dist, work):
    files = work / "files"
    if work.exists():
        shutil.rmtree(work)
    files.mkdir(parents=True)
    manifest = []

    for archive_path in release_zips(dist):
        with zipfile.ZipFile(archive_path) as archive:
            for info in archive.infolist():
                if info.is_dir() or not is_executable_name(info.filename):
                    continue
                data = archive.read(info)
                signature = pe_signature_size(data)
                if signature is None:
                    fail(
                        f"{archive_path.name}: {info.filename} is named as an executable but is no PE image"
                    )
                if signature:
                    print(f"keeps its publisher's signature: {archive_path.name}: {info.filename}")
                    continue
                target = files.joinpath(archive_path.stem, *safe_entry_path(info.filename))
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
                manifest.append(
                    {
                        "zip": archive_path.name,
                        "entry": info.filename,
                        "file": target.relative_to(work).as_posix(),
                        "unsigned_sha256": sha256_bytes(data),
                    }
                )

    for loose in loose_executables(dist):
        data = loose.read_bytes()
        signature = pe_signature_size(data)
        if signature is None:
            fail(f"{loose.name} is named as an executable but is no PE image")
        if signature:
            print(f"keeps its publisher's signature: {loose.name}")
            continue
        target = files / "loose" / loose.name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        manifest.append(
            {
                "zip": None,
                "entry": loose.name,
                "file": target.relative_to(work).as_posix(),
                "unsigned_sha256": sha256_bytes(data),
            }
        )

    if not manifest:
        fail(f"no unsigned executable in {dist}")
    (work / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf8")
    for item in manifest:
        where = f"{item['zip']}: " if item["zip"] else ""
        print(f"to sign: {where}{item['entry']}")
    print(f"{len(manifest)} file(s) to sign")
    return manifest


def signed_bytes(work, item):
    """The signed file of a manifest item, refused if it is still unsigned."""
    path = work / item["file"]
    if not path.is_file():
        fail(f"the signed {item['file']} is missing")
    data = path.read_bytes()
    signature = pe_signature_size(data)
    if signature is None:
        fail(f"the signed {item['file']} is no PE image")
    if not signature or sha256_bytes(data) == item["unsigned_sha256"]:
        fail(f"{item['file']} was not signed")
    return data


def copy_entry_info(info):
    """A new entry header with the old one's name, date, attributes and compression."""
    copy = zipfile.ZipInfo(info.filename, date_time=info.date_time)
    copy.compress_type = info.compress_type
    copy.external_attr = info.external_attr
    copy.create_system = info.create_system
    copy.comment = info.comment
    copy.extra = info.extra
    return copy


def rewrite_zip(archive_path, replacements):
    """Write archive_path again with the entries named in replacements given their new bytes."""
    temporary = archive_path.with_name(archive_path.name + ".signing")
    with zipfile.ZipFile(archive_path) as source:
        unknown = set(replacements) - set(source.namelist())
        if unknown:
            fail(f"{archive_path.name} has no entry {sorted(unknown)[0]}")
        with zipfile.ZipFile(temporary, "w") as target:
            target.comment = source.comment
            for info in source.infolist():
                data = replacements.get(info.filename)
                if data is None:
                    data = source.read(info)
                target.writestr(copy_entry_info(info), data)
    temporary.replace(archive_path)


def rewrite_sums(dist, changed):
    """Renew the lines of every SHA256SUMS*.txt that name a changed file."""
    for sums in sorted(dist.glob("SHA256SUMS*.txt")):
        lines = []
        for line in sums.read_text(encoding="utf8").splitlines():
            digest, separator, name = line.partition("  ")
            if separator and name in changed:
                line = f"{sha256_file(dist / name)}  {name}"
            lines.append(line)
        sums.write_text("".join(f"{line}\n" for line in lines), encoding="utf8")


def pack(dist, work):
    manifest_path = work / "manifest.json"
    if not manifest_path.is_file():
        fail(f"no {manifest_path}: run extract first")
    manifest = json.loads(manifest_path.read_text(encoding="utf8"))

    by_zip = {}
    changed = set()
    for item in manifest:
        data = signed_bytes(work, item)
        if item["zip"] is None:
            (dist / item["entry"]).write_bytes(data)
            changed.add(item["entry"])
        else:
            by_zip.setdefault(item["zip"], {})[item["entry"]] = data

    for name, replacements in sorted(by_zip.items()):
        archive_path = dist / name
        if not archive_path.is_file():
            fail(f"{name} is missing from {dist}")
        rewrite_zip(archive_path, replacements)
        changed.add(name)

    rewrite_sums(dist, changed)
    for name in sorted(changed):
        print(f"signed: {name}  {sha256_file(dist / name)}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", choices=["extract", "pack"])
    parser.add_argument("--dist", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    options = parser.parse_args()
    if not options.dist.is_dir():
        fail(f"no folder {options.dist}")
    if options.command == "extract":
        extract(options.dist, options.work)
    else:
        pack(options.dist, options.work)


if __name__ == "__main__":
    main()
