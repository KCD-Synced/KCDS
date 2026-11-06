"""Tests of virustotal_scan.py without the network."""

import pathlib
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(pathlib.Path(__file__).parent))

import virustotal_scan as vt  # noqa: E402


class FakeVirusTotal:
    def __init__(self, known_after_upload=True, fail=False):
        self.seen = {}
        self.uploads = []
        self.known_after_upload = known_after_upload
        self.fail = fail

    def known(self, sha256):
        if self.fail:
            raise RuntimeError("VirusTotal answered 500")
        return self.seen.get(sha256)

    def upload(self, path):
        self.uploads.append(path.name)
        if self.known_after_upload:
            self.seen[vt.sha256_of(path)] = {
                "last_analysis_stats": {"malicious": 1, "undetected": 60},
                "last_analysis_results": {
                    "Engine": {"category": "malicious", "result": "Trojan.Test"},
                    "Other": {"category": "undetected", "result": None},
                },
            }
        return "analysis"

    def analysis_finished(self, analysis):
        return True


class FilesToScan(unittest.TestCase):
    def test_the_archives_and_the_executables_inside_the_zips(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            dist = root / "dist"
            dist.mkdir()
            with zipfile.ZipFile(dist / "KCDS-Client-1.zip", "w") as client:
                client.writestr("kcds-launcher.exe", b"launcher")
                client.writestr("runtime/kcd2s-game-client-v4.dll", b"dll")
                client.writestr("README.txt", b"text")
            (dist / "KCDS-Server-1-linux.tar.gz").write_bytes(b"tar")
            (dist / "SHA256SUMS.txt").write_bytes(b"sums")
            found = vt.files_to_scan(dist, root / "unpacked")
            self.assertEqual(
                [name for name, _ in found],
                [
                    "KCDS-Client-1.zip",
                    "KCDS-Client-1.zip / kcds-launcher.exe",
                    "KCDS-Client-1.zip / runtime/kcd2s-game-client-v4.dll",
                    "KCDS-Server-1-linux.tar.gz",
                ],
            )
            self.assertEqual(found[2][1].read_bytes(), b"dll")


class Scan(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.path = pathlib.Path(self.folder.name) / "a.exe"
        self.path.write_bytes(b"a")

    def test_an_unknown_file_is_uploaded_and_its_detections_reported(self):
        client = FakeVirusTotal()
        result = vt.scan(client, "a.exe", self.path)
        self.assertEqual(client.uploads, ["a.exe"])
        self.assertEqual(result["malicious"], 1)
        self.assertEqual(result["engines"], 61)
        self.assertEqual(result["detections"], {"Engine": "Trojan.Test"})
        self.assertIn(result["sha256"], result["link"])

    def test_a_known_file_is_not_uploaded_again(self):
        client = FakeVirusTotal()
        vt.scan(client, "a.exe", self.path)
        vt.scan(client, "a.exe", self.path)
        self.assertEqual(client.uploads, ["a.exe"])

    def test_a_failed_call_is_a_file_without_a_result(self):
        result = vt.scan(FakeVirusTotal(fail=True), "a.exe", self.path)
        self.assertEqual(result["error"], "VirusTotal answered 500")


class NoEngineJudgedIt(unittest.TestCase):
    def test_stats_of_zeros_are_no_result_and_not_a_clean_file(self):
        result = vt.result_of("a.exe", "aa", {"last_analysis_stats": {"malicious": 0}})
        self.assertEqual(result["error"], "no result")
        self.assertFalse(vt.judged({"last_analysis_stats": {"malicious": 0, "undetected": 0}}))
        self.assertFalse(vt.judged(None))
        self.assertTrue(vt.judged({"last_analysis_stats": {"undetected": 1}}))


class Markdown(unittest.TestCase):
    def test_it_names_the_flagged_engines_and_the_files_without_a_result(self):
        attributes = {
            "last_analysis_stats": {"malicious": 1, "undetected": 60},
            "last_analysis_results": {"Engine": {"category": "malicious", "result": "Bad"}},
        }
        text = vt.markdown_of(
            [
                vt.result_of("flagged.exe", "aa", attributes),
                vt.result_of("clean.exe", "bb", {"last_analysis_stats": {"undetected": 61}}),
                vt.result_of("lost.exe", "cc", None),
            ]
        )
        self.assertIn("3 files: 1 flagged, 1 without a result.", text)
        self.assertIn("| flagged.exe | 1 | 0 | 61 |", text)
        self.assertIn("- Engine: Bad", text)
        self.assertIn("| lost.exe | no result |", text)


if __name__ == "__main__":
    unittest.main()
