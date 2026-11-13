import datetime
import unittest

import watch

STEAMCMD_OUTPUT = """Redirecting stderr to '/root/Steam/logs/stderr.txt'
Logging in user 'anonymous' to Steam Public...OK
AppID : 1771300, change number : 29000000/0, last change : Wed Nov 12 10:00:00 2026
"1771300"
{
\t"common"
\t{
\t\t"name"\t\t"Kingdom Come: Deliverance II"
\t\t"type"\t\t"Game"
\t}
\t"depots"
\t{
\t\t"1771301"
\t\t{
\t\t\t"manifests"
\t\t\t{
\t\t\t\t"public"
\t\t\t\t{
\t\t\t\t\t"gid"\t\t"1111111111111111111"
\t\t\t\t\t"size"\t\t"100"
\t\t\t\t}
\t\t\t}
\t\t}
\t\t"1771302"
\t\t{
\t\t\t"manifests"
\t\t\t{
\t\t\t\t"public"\t\t"2222222222222222222"
\t\t\t}
\t\t}
\t\t"branches"
\t\t{
\t\t\t"public"
\t\t\t{
\t\t\t\t"buildid"\t\t"20500123"
\t\t\t\t"timeupdated"\t\t"1762900000"
\t\t\t}
\t\t\t"beta"
\t\t\t{
\t\t\t\t"buildid"\t\t"20600000"
\t\t\t}
\t\t}
\t}
}
Unloading Steam API...OK
"""


class ParseTest(unittest.TestCase):
    def test_public_build_and_depot_manifests_are_read(self):
        info = watch.app_info(STEAMCMD_OUTPUT, "1771300")
        self.assertEqual(info["common"]["name"], "Kingdom Come: Deliverance II")
        build = watch.public_build(info)
        self.assertEqual(build["build"], "20500123")
        self.assertEqual(
            build["depots"], {"1771301": "1111111111111111111", "1771302": "2222222222222222222"}
        )

    def test_missing_app_or_build_is_an_error(self):
        with self.assertRaises(watch.VdfError):
            watch.app_info("Logging in...FAILED\n", "1771300")
        with self.assertRaises(watch.VdfError):
            watch.public_build({"depots": {"branches": {}}})
        with self.assertRaises(watch.VdfError):
            watch.app_info('"1771300"\n{\n"common"\n{\n', "1771300")

    def test_vdf_escapes_comments_and_errors(self):
        parsed = watch.parse_vdf('"a" { "b" "x\\"y" // note\n "c" { } }')
        self.assertEqual(parsed, {"a": {"b": 'x"y', "c": {}}})
        for broken in ['"a" {', '"a" }', "{ }", '"a" "b" "c"', "a b"]:
            with self.assertRaises(watch.VdfError, msg=broken):
                watch.parse_vdf(broken)


class TextTest(unittest.TestCase):
    def test_issue_names_both_builds_and_the_checklist(self):
        text = watch.issue_text("20500123", "20400000", "2026-11-13T12:00:00Z")
        self.assertIn("Steam build 20500123", text["title"])
        self.assertIn("20400000", text["body"])
        self.assertIn("- [ ] KCDS release needed?", text["body"])
        self.assertIn("unknown", watch.issue_text("1", "", "t")["body"])

    def test_discord_text_links_the_issue(self):
        text = watch.discord_text("2", "1", "https://example.invalid/issues/9")
        self.assertIn("1 -> 2", text)
        self.assertIn("https://example.invalid/issues/9", text)


class UnreachableTest(unittest.TestCase):
    def test_alert_goes_out_once_after_an_hour(self):
        last = datetime.datetime(2026, 11, 13, 12, 0, tzinfo=datetime.timezone.utc)
        minutes = [
            m
            for m in range(0, 180, 10)
            if watch.unreachable_alert_due(last, last + datetime.timedelta(minutes=m + 1))
        ]
        self.assertEqual(minutes, [60])


if __name__ == "__main__":
    unittest.main()
