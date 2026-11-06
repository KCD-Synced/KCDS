"""npm_types.py: the npm package @kcds/types of the SDK's TypeScript declarations."""

import json
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import npm_types  # noqa: E402  (sibling release tool)

DECLARATIONS = {
    "shared.d.ts": "declare const console: any;\n",
    "server-api.d.ts": '/// <reference path="./shared.d.ts" />\n',
    "client-api.d.ts": '/// <reference path="./shared.d.ts" />\n',
}


class NpmVersionTest(unittest.TestCase):
    def test_a_release_keeps_its_version(self):
        self.assertEqual(npm_types.npm_version("0.4.0"), "0.4.0")

    def test_a_hotfix_is_named_as_the_update_feed_names_it(self):
        self.assertEqual(npm_types.npm_version("0.3.4.3"), "0.3.4-hotfix.3")

    def test_a_zero_hotfix_is_the_release(self):
        self.assertEqual(npm_types.npm_version("0.3.4.0"), "0.3.4")

    def test_anything_else_is_refused(self):
        for version in ("0.4", "v0.4.0", "0.4.0-beta", "0.4.0.1.2", ""):
            with self.subTest(version=version):
                with self.assertRaises(ValueError):
                    npm_types.npm_version(version)


class PackageFilesTest(unittest.TestCase):
    def files(self, version="0.3.4.3"):
        return npm_types.package_files(dict(DECLARATIONS), version, "MIT")

    def test_the_declarations_ship_unchanged(self):
        files = self.files()
        for name, text in DECLARATIONS.items():
            self.assertEqual(files[name], text)

    def test_each_side_has_an_entry_of_references_only(self):
        files = self.files()
        for side in ("server", "client"):
            entry = files[f"{side}.d.ts"].splitlines()
            references = [line for line in entry if not line.startswith("// ")]
            self.assertEqual(
                references,
                [
                    '/// <reference path="./shared.d.ts" />',
                    f'/// <reference path="./{side}-api.d.ts" />',
                ],
            )

    def test_the_manifest_names_the_package_its_version_and_its_files(self):
        files = self.files()
        manifest = json.loads(files["package.json"])
        self.assertEqual(manifest["name"], "@kcds/types")
        self.assertEqual(manifest["version"], "0.3.4-hotfix.3")
        self.assertEqual(manifest["license"], "MIT")
        self.assertEqual(manifest["exports"]["./server"], {"types": "./server.d.ts"})
        self.assertEqual(manifest["exports"]["./client"], {"types": "./client.d.ts"})
        self.assertNotIn(".", manifest["exports"])
        for name in manifest["files"]:
            self.assertIn(name, files)
        self.assertEqual(set(manifest["files"]) | {"package.json"}, set(files))

    def test_the_readme_names_the_version_to_install(self):
        readme = self.files()["README.md"]
        self.assertIn("KCDS 0.3.4.3", readme)
        self.assertIn("npm install --save-dev @kcds/types@0.3.4-hotfix.3", readme)
        self.assertIn(npm_types.DOCS, readme)
        for placeholder in ("{version}", "{npm_version}", "{docs}"):
            self.assertNotIn(placeholder, readme)

    def test_missing_declarations_are_refused(self):
        partial = dict(DECLARATIONS)
        del partial["client-api.d.ts"]
        with self.assertRaises(ValueError):
            npm_types.package_files(partial, "0.4.0", "MIT")


class ReleasedZipTest(unittest.TestCase):
    def test_the_zip_header_names_no_repository_path(self):
        old = (
            "// KCDS SDK 0.3.4.3: generated from the SDK's own API "
            "(Dev/tools/web/build_typescript_declarations.py).\n"
            "// Do not edit: regenerate it from the release instead.\n"
        )
        text = npm_types.public_header(old, "0.3.4.3")
        self.assertNotIn("Dev/", text)
        header = "// KCDS SDK 0.3.4.3: generated from the SDK's own API, as "
        self.assertTrue(text.startswith(header))
        self.assertTrue(text.endswith("// Do not edit: regenerate it from the release instead.\n"))

    def test_the_declarations_are_read_from_a_released_zip(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "KCDS-Types-0.3.4.3.zip"
            with zipfile.ZipFile(path, "w") as archive:
                for name, text in DECLARATIONS.items():
                    archive.writestr(name, text)
                archive.writestr("README.txt", "not a declaration")
            self.assertEqual(npm_types.zip_declarations(path), DECLARATIONS)


if __name__ == "__main__":
    unittest.main()
