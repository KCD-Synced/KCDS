"""Tests of sign_zips.py with small synthetic PE images, without the signing service."""

import contextlib
import io
import json
import pathlib
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(pathlib.Path(__file__).parent))

import sign_zips  # noqa: E402

PE_HEADER = 0x40
OPTIONAL_HEADER = PE_HEADER + 24
DIRECTORIES = OPTIONAL_HEADER + 112
SECURITY_ENTRY = DIRECTORIES + sign_zips.SECURITY_DIRECTORY * 8


def pe_image(payload=b"code", signed=False):
    """A minimal PE32+ image: the headers the signature check reads, then payload."""
    image = bytearray(DIRECTORIES + 16 * 8)
    image[0:2] = b"MZ"
    image[0x3C:0x40] = PE_HEADER.to_bytes(4, "little")
    image[PE_HEADER : PE_HEADER + 4] = b"PE\0\0"
    image[OPTIONAL_HEADER : OPTIONAL_HEADER + 2] = sign_zips.PE32_PLUS_MAGIC.to_bytes(2, "little")
    image[OPTIONAL_HEADER + 108 : OPTIONAL_HEADER + 112] = (16).to_bytes(4, "little")
    image += payload
    if signed:
        sign(image)
    return bytes(image)


def sign(image):
    """What the signing action does, as far as the tests see it: a certificate table at the end."""
    certificate = b"certificate"
    offset = len(image)
    image[SECURITY_ENTRY : SECURITY_ENTRY + 4] = offset.to_bytes(4, "little")
    image[SECURITY_ENTRY + 4 : SECURITY_ENTRY + 8] = len(certificate).to_bytes(4, "little")
    image += certificate


def sign_all(work):
    for path in sorted((work / "files").rglob("*")):
        if path.is_file():
            image = bytearray(path.read_bytes())
            sign(image)
            path.write_bytes(bytes(image))


def quietly(function, *arguments):
    with contextlib.redirect_stdout(io.StringIO()):
        return function(*arguments)


class Release:
    """A dist folder like the release's: the Client and Types zips and SHA256SUMS.txt."""

    def __init__(self, root):
        self.dist = root / "dist"
        self.work = root / "work"
        self.dist.mkdir()
        self.client = self.dist / "KCDS-Client-1.2.3.zip"
        self.types = self.dist / "KCDS-Types-1.2.3.zip"
        with zipfile.ZipFile(self.client, "w", zipfile.ZIP_DEFLATED) as archive:
            archive.comment = b"KCDS"
            archive.writestr(zipfile.ZipInfo("KCDS-Client-1.2.3/", (2026, 1, 2, 3, 4, 6)), b"")
            self.add(archive, "KCDS-Client-1.2.3/kcds-launcher.exe", pe_image(b"launcher"))
            self.add(archive, "KCDS-Client-1.2.3/kcds-launcher.json", b'{"sentry": {}}')
            self.add(archive, "KCDS-Client-1.2.3/phonon.dll", pe_image(b"steam audio", signed=True))
            self.add(archive, "KCDS-Client-1.2.3/kcd2s-game-client-v4.dll", pe_image(b"mod"))
        with zipfile.ZipFile(self.types, "w", zipfile.ZIP_DEFLATED) as archive:
            self.add(archive, "csharp/Kcds.Sdk.dll", pe_image(b"sdk"))
            self.add(archive, "README.txt", b"types")
        self.write_sums()

    @staticmethod
    def add(archive, name, data):
        info = zipfile.ZipInfo(name, date_time=(2026, 1, 2, 3, 4, 6))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o644 << 16
        archive.writestr(info, data)

    def write_sums(self):
        lines = [
            f"{sign_zips.sha256_file(path)}  {path.name}\n" for path in (self.client, self.types)
        ]
        (self.dist / "SHA256SUMS.txt").write_text("".join(lines), encoding="utf8")
        (self.dist / "SHA256SUMS-linux.txt").write_text(
            "abc  KCDS-Server-1.2.3-linux.tar.gz\n", encoding="utf8"
        )


class Extract(unittest.TestCase):
    def test_the_unsigned_executables_of_every_zip_and_not_the_signed_ones(self):
        with tempfile.TemporaryDirectory() as folder:
            release = Release(pathlib.Path(folder))
            manifest = quietly(sign_zips.extract, release.dist, release.work)
            entries = sorted((item["zip"], item["entry"]) for item in manifest)
            self.assertEqual(
                entries,
                [
                    ("KCDS-Client-1.2.3.zip", "KCDS-Client-1.2.3/kcd2s-game-client-v4.dll"),
                    ("KCDS-Client-1.2.3.zip", "KCDS-Client-1.2.3/kcds-launcher.exe"),
                    ("KCDS-Types-1.2.3.zip", "csharp/Kcds.Sdk.dll"),
                ],
            )
            launcher = release.work / "files/KCDS-Client-1.2.3/KCDS-Client-1.2.3/kcds-launcher.exe"
            self.assertEqual(launcher.read_bytes(), pe_image(b"launcher"))
            written = json.loads((release.work / "manifest.json").read_text(encoding="utf8"))
            self.assertEqual(written, manifest)

    def test_a_loose_installer_is_signed_too(self):
        with tempfile.TemporaryDirectory() as folder:
            release = Release(pathlib.Path(folder))
            (release.dist / "KCDS-Setup-1.2.3.exe").write_bytes(pe_image(b"setup"))
            manifest = quietly(sign_zips.extract, release.dist, release.work)
            loose = [item for item in manifest if item["zip"] is None]
            self.assertEqual([item["entry"] for item in loose], ["KCDS-Setup-1.2.3.exe"])

    def test_an_executable_name_that_is_no_pe_image_is_refused(self):
        with tempfile.TemporaryDirectory() as folder:
            release = Release(pathlib.Path(folder))
            with zipfile.ZipFile(release.types, "a") as archive:
                archive.writestr("broken.dll", b"not a PE image")
            with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                quietly(sign_zips.extract, release.dist, release.work)

    def test_an_entry_that_would_leave_the_work_folder_is_refused(self):
        with tempfile.TemporaryDirectory() as folder:
            release = Release(pathlib.Path(folder))
            with zipfile.ZipFile(release.types, "a") as archive:
                archive.writestr("../escape.exe", pe_image())
            with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                quietly(sign_zips.extract, release.dist, release.work)


class Pack(unittest.TestCase):
    def test_the_signed_files_replace_the_unsigned_ones_and_nothing_else_changes(self):
        with tempfile.TemporaryDirectory() as folder:
            release = Release(pathlib.Path(folder))
            with zipfile.ZipFile(release.client) as archive:
                before = [
                    (info.filename, info.date_time, info.compress_type, info.external_attr)
                    for info in archive.infolist()
                ]
            quietly(sign_zips.extract, release.dist, release.work)
            sign_all(release.work)
            quietly(sign_zips.pack, release.dist, release.work)

            with zipfile.ZipFile(release.client) as archive:
                after = [
                    (info.filename, info.date_time, info.compress_type, info.external_attr)
                    for info in archive.infolist()
                ]
                self.assertEqual(after, before)
                self.assertEqual(archive.comment, b"KCDS")
                launcher = archive.read("KCDS-Client-1.2.3/kcds-launcher.exe")
                self.assertGreater(sign_zips.pe_signature_size(launcher), 0)
                unsigned = pe_image(b"launcher")
                self.assertEqual(launcher[:SECURITY_ENTRY], unsigned[:SECURITY_ENTRY])
                self.assertEqual(
                    launcher[SECURITY_ENTRY + 8 : len(unsigned)], unsigned[SECURITY_ENTRY + 8 :]
                )
                self.assertEqual(
                    archive.read("KCDS-Client-1.2.3/phonon.dll"),
                    pe_image(b"steam audio", signed=True),
                )
                self.assertEqual(
                    archive.read("KCDS-Client-1.2.3/kcds-launcher.json"), b'{"sentry": {}}'
                )
            with zipfile.ZipFile(release.types) as archive:
                self.assertGreater(
                    sign_zips.pe_signature_size(archive.read("csharp/Kcds.Sdk.dll")), 0
                )

    def test_the_checksums_name_the_signed_zips(self):
        with tempfile.TemporaryDirectory() as folder:
            release = Release(pathlib.Path(folder))
            quietly(sign_zips.extract, release.dist, release.work)
            sign_all(release.work)
            quietly(sign_zips.pack, release.dist, release.work)
            sums = (release.dist / "SHA256SUMS.txt").read_text(encoding="utf8")
            self.assertEqual(
                sums,
                f"{sign_zips.sha256_file(release.client)}  {release.client.name}\n"
                f"{sign_zips.sha256_file(release.types)}  {release.types.name}\n",
            )
            linux = (release.dist / "SHA256SUMS-linux.txt").read_text(encoding="utf8")
            self.assertEqual(linux, "abc  KCDS-Server-1.2.3-linux.tar.gz\n")

    def test_a_file_the_signing_step_left_unsigned_stops_the_release(self):
        with tempfile.TemporaryDirectory() as folder:
            release = Release(pathlib.Path(folder))
            quietly(sign_zips.extract, release.dist, release.work)
            client_bytes = release.client.read_bytes()
            with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                quietly(sign_zips.pack, release.dist, release.work)
            self.assertEqual(release.client.read_bytes(), client_bytes)


class SignatureSize(unittest.TestCase):
    def test_unsigned_signed_and_not_a_pe_image(self):
        self.assertEqual(sign_zips.pe_signature_size(pe_image()), 0)
        self.assertGreater(sign_zips.pe_signature_size(pe_image(signed=True)), 0)
        self.assertIsNone(sign_zips.pe_signature_size(b"MZ" + bytes(100)))
        self.assertIsNone(sign_zips.pe_signature_size(b"PK\3\4"))


if __name__ == "__main__":
    unittest.main()
