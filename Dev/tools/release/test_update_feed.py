"""update_feed.py: a hotfix's envelope is served under the release label a launcher asks for
when it switches to the version a server runs, as well as under its version."""

import base64
import json
from pathlib import Path
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import update_feed  # noqa: E402  (sibling release tool)


def envelope(release):
    payload = {"release": release, "sequence": 7, "files": [{"sha256": "ab" * 32}]}
    return json.dumps({"payload": base64.b64encode(json.dumps(payload).encode()).decode()})


class EnvelopeFoldersTest(unittest.TestCase):
    def test_a_version_of_three_numbers_has_one_folder(self):
        self.assertEqual(update_feed.envelope_folders("1.2.1"), ["1.2.1"])

    def test_a_hotfix_is_also_served_under_its_release_label(self):
        self.assertEqual(
            update_feed.envelope_folders("1.2.0.1"), ["1.2.0.1", "1.2.0-hotfix.1"]
        )


class UploadTest(unittest.TestCase):
    def upload(self, version, previous_release):
        commands = []

        def remote(command, check=True):
            commands.append(command)
            return mock.Mock(stdout="ab" * 32)

        with mock.patch.object(update_feed, "remote", side_effect=remote), mock.patch.object(
            update_feed.subprocess, "run"
        ):
            update_feed.upload(
                Path("feed"), "stable", envelope(update_feed.release_label(version)), version,
                previous_release,
            )
        return commands[-1]

    def test_a_hotfix_envelope_is_written_under_both_folders(self):
        moves = self.upload("1.2.0.3", "1.2.1")
        versions = f"{update_feed.REMOTE_ROOT}/stable/versions"
        self.assertIn(f"'{versions}/1.2.0.3/manifest.json'", moves)
        self.assertIn(f"'{versions}/1.2.0-hotfix.3/manifest.json'", moves)

    def test_a_replaced_hotfix_is_kept_under_both_folders(self):
        moves = self.upload("1.2.2", "1.2.0-hotfix.2")
        versions = f"{update_feed.REMOTE_ROOT}/stable/versions"
        self.assertIn(f"{versions}/1.2.0.2/manifest.json", moves)
        self.assertIn(f"{versions}/1.2.0-hotfix.2/manifest.json", moves)
        self.assertNotIn("1.2.2-hotfix", moves)

    def test_verify_reads_every_folder_of_a_hotfix(self):
        text = envelope("1.2.0-hotfix.3")
        asked = []

        def fetch(url, method="GET"):
            asked.append(url)
            return 200, text.encode("utf8") if method == "GET" else b""

        with mock.patch.object(update_feed, "fetch", side_effect=fetch):
            update_feed.verify("stable", text, [], "1.2.0.3")
        self.assertTrue(any(url.endswith("/versions/1.2.0.3/manifest") for url in asked))
        self.assertTrue(any(url.endswith("/versions/1.2.0-hotfix.3/manifest") for url in asked))


if __name__ == "__main__":
    unittest.main()
