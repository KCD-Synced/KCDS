"""publish.py: published versions never change, the CDN is purged for every written URL
and every file is checked through its public URL."""

import hashlib
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import publish  # noqa: E402  (sibling release tool)

VERSION = "0.3.4.2"
BASE = "https://downloads.example"


class PublishTest(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.dist = Path(self.scratch.name)
        self.files = []
        for name, body in [
            (f"KCDS-Client-{VERSION}.zip", b"client"),
            (f"KCDS-Server-{VERSION}-linux.tar.gz", b"server"),
            ("SHA256SUMS.txt", b"sums"),
        ]:
            path = self.dist / name
            path.write_bytes(body)
            self.files.append(path)

    def tearDown(self):
        self.scratch.cleanup()

    def digest(self, body):
        return hashlib.sha256(body).hexdigest()

    def test_sums_are_read_as_sha256sum_writes_them(self):
        text = f"{'a' * 64}  KCDS-Client.zip\n{'b' * 64} *KCDS-Server-linux.tar.gz\nnot a line\n"
        self.assertEqual(
            publish.parse_sums(text),
            {"KCDS-Client.zip": "a" * 64, "KCDS-Server-linux.tar.gz": "b" * 64},
        )

    def test_a_published_file_never_changes(self):
        client, server, sums = self.files
        present = {client.name: self.digest(b"client"), server.name: self.digest(b"server")}
        # A repeated run uploads only what is missing.
        self.assertEqual(publish.unpublished(self.files, present), [sums])
        present[sums.name] = self.digest(b"sums")
        self.assertEqual(publish.unpublished(self.files, present), [])
        # Other bytes under a published name stop the run.
        present[server.name] = self.digest(b"another build")
        with redirect_stdout(io.StringIO()), self.assertRaises(SystemExit):
            publish.unpublished(self.files, present)

    def test_every_written_url_is_purged(self):
        urls = publish.written_urls(self.files, VERSION, BASE, "stable", False)
        self.assertIn(f"{BASE}/{VERSION}/KCDS-Client-{VERSION}.zip", urls)
        self.assertIn(f"{BASE}/latest/KCDS-Client.zip", urls)
        self.assertIn(f"{BASE}/latest/SHA256SUMS.txt", urls)
        origin = publish.update_feed.UPDATE_ORIGIN.rstrip("/")
        self.assertIn(f"{origin}/v1/updates/stable/manifest", urls)
        self.assertIn(f"{origin}/v1/updates/server-linux-stable/manifest", urls)
        # Servers are hosted on Linux only: no Windows server feed or download.
        self.assertFalse(any("server-windows" in url for url in urls))
        self.assertIn(f"{BASE}/latest/KCDS-Server-linux.tar.gz", urls)
        self.assertNotIn(f"{BASE}/latest/KCDS-Server.zip", urls)
        without_feed = publish.written_urls(self.files, VERSION, BASE, "stable", True)
        self.assertFalse(any("/v1/updates/" in url for url in without_feed))

    def test_the_purge_goes_in_batches_and_needs_a_token_when_required(self):
        urls = [f"{BASE}/{index}" for index in range(45)]
        with mock.patch.dict(os.environ, {"CLOUDFLARE_API_TOKEN": "", "CLOUDFLARE_ZONE_ID": ""}):
            with redirect_stdout(io.StringIO()), self.assertRaises(SystemExit):
                publish.purge_cdn(urls, True, False)
            with redirect_stdout(io.StringIO()) as out:
                publish.purge_cdn(urls, False, False)
            self.assertIn("not purged", out.getvalue())
        calls = []

        def answer(method, url, data=None, headers=None):
            calls.append((url, len(data["files"]), headers["Authorization"]))
            return 200, {"success": True}

        secrets = {"CLOUDFLARE_API_TOKEN": "token", "CLOUDFLARE_ZONE_ID": "zone"}
        with mock.patch.dict(os.environ, secrets), mock.patch.object(publish, "request", answer):
            with redirect_stdout(io.StringIO()):
                publish.purge_cdn(urls, True, False)
        self.assertEqual([count for _, count, _ in calls], [30, 15])
        self.assertTrue(all(url.endswith("/zones/zone/purge_cache") for url, _, _ in calls))

    def test_every_public_file_is_checked_against_its_build(self):
        expected = publish.public_files(self.files, VERSION, BASE)
        urls = dict(expected)
        self.assertEqual(urls[f"{BASE}/latest/KCDS-Client.zip"], self.digest(b"client"))
        sums = publish.latest_sums(self.files, VERSION).encode("utf8")
        self.assertEqual(urls[f"{BASE}/latest/SHA256SUMS.txt"], self.digest(sums))
        # The sums file is no download: it has no latest/ link of its own.
        self.assertNotIn(f"{BASE}/latest/SHA256SUMS", [u for u in urls if u.endswith("SUMS")])

    def test_a_cdn_copy_of_an_older_file_stops_the_run(self):
        url = f"{BASE}/latest/KCDS-Client.zip"
        good = self.digest(b"client")
        served = iter([self.digest(b"old"), good])
        with mock.patch.object(publish, "public_digest", lambda _: next(served)), mock.patch.object(
            publish.time, "sleep", lambda _: None
        ):
            with redirect_stdout(io.StringIO()):
                publish.verify_public([(url, good)])
        with mock.patch.object(
            publish, "public_digest", lambda _: self.digest(b"old")
        ), mock.patch.object(publish.time, "sleep", lambda _: None):
            with redirect_stdout(io.StringIO()), self.assertRaises(SystemExit):
                publish.verify_public([(url, good)])


if __name__ == "__main__":
    unittest.main()
