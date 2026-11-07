"""The release's debug symbols for the crash server (upload_symbols.py): which files go, and the
token read from the file the release workflow names."""

from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import upload_symbols  # noqa: E402


def fake_build(folder, without_pdb=()):
    """A build folder with every shipped client binary and, except those named, its PDB."""
    for source, name in upload_symbols.package.CLIENT_BINARIES:
        binary = folder / source
        binary.parent.mkdir(parents=True, exist_ok=True)
        binary.write_bytes(b"MZ")
        if name not in without_pdb:
            binary.with_suffix(".pdb").write_bytes(b"pdb")


class SymbolFiles(unittest.TestCase):
    def test_every_shipped_binary_goes_with_its_pdb(self):
        with tempfile.TemporaryDirectory() as folder:
            build = Path(folder)
            fake_build(build)
            names = [name for _, name in upload_symbols.symbol_files(build)]
            self.assertIn("kcd2s-game-client-v4.dll", names)
            self.assertIn("kcd2s-game-client-v4.pdb", names)
            self.assertEqual(len(names), 2 * len(upload_symbols.package.CLIENT_BINARIES))

    def test_a_third_party_binary_without_pdb_is_skipped_and_ours_is_refused(self):
        with tempfile.TemporaryDirectory() as folder:
            build = Path(folder)
            fake_build(build, without_pdb={"crashpad_handler.exe"})
            names = [name for _, name in upload_symbols.symbol_files(build)]
            self.assertNotIn("crashpad_handler.exe", names)
        with tempfile.TemporaryDirectory() as folder:
            build = Path(folder)
            fake_build(build, without_pdb={"kcd2s-game-client-v4.dll"})
            with self.assertRaises(SystemExit):
                upload_symbols.symbol_files(build)


class Token(unittest.TestCase):
    def test_the_token_comes_from_the_file_named(self):
        with tempfile.TemporaryDirectory() as folder:
            token = Path(folder) / "token"
            token.write_text("secret-token\n", encoding="utf8")
            with mock.patch.object(upload_symbols, "token_file", token):
                self.assertEqual(upload_symbols.token(), "secret-token")
            with mock.patch.object(upload_symbols, "token_file", Path(folder) / "missing"):
                with self.assertRaises(SystemExit):
                    upload_symbols.token()


if __name__ == "__main__":
    unittest.main()
