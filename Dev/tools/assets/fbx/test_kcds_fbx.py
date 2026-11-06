"""kcds_fbx.py: Blender found and called headless to make glTF of an FBX file; the
refusals. Blender itself is not run here: a stand-in answers as it would."""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import kcds_fbx  # noqa: E402


class Finished:
    def __init__(self, stdout="", stderr="", returncode=0):
        self.stdout = stdout
        self.stderr = stderr
        self.returncode = returncode


class FbxTest(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.mkdtemp()
        self.fbx = os.path.join(self.folder, "model.fbx")
        with open(self.fbx, "wb") as file:
            file.write(b"Kaydara FBX Binary  \0")
        self.blender = os.path.join(self.folder, "blender.exe")
        with open(self.blender, "wb") as file:
            file.write(b"")
        self.environment = {"KCDS_BLENDER": self.blender}

    def test_blender_runs_headless_with_factory_settings_and_writes_the_glb(self):
        calls = []
        glb = os.path.join(self.folder, "model.glb")

        def run(command, **options):
            calls.append(command)
            with open(command[-1], "wb") as file:
                file.write(b"glTF")
            return Finished()

        made = kcds_fbx.convert(self.fbx, glb, self.environment, run)
        self.assertEqual(made, {"glb": glb, "blender": self.blender})
        command = calls[0]
        self.assertEqual(command[0], self.blender)
        self.assertIn("-b", command)
        self.assertIn("--factory-startup", command)
        self.assertEqual(command[-2:], [os.path.abspath(self.fbx), os.path.abspath(glb)])
        self.assertIn("import_scene.fbx", command[command.index("--python-expr") + 1])

    def test_blender_without_a_glb_is_reported_with_its_error(self):
        def run(command, **options):
            return Finished(stderr="Error: File format is not supported", returncode=1)

        with self.assertRaisesRegex(kcds_fbx.FbxError, "File format is not supported"):
            kcds_fbx.convert(self.fbx, os.path.join(self.folder, "out.glb"), self.environment, run)

    def test_not_an_fbx_file_is_refused(self):
        with self.assertRaisesRegex(kcds_fbx.FbxError, "not an FBX file"):
            kcds_fbx.convert(os.path.join(self.folder, "model.obj"), "out.glb", self.environment)

    def test_a_blender_path_that_is_no_file_is_refused(self):
        with self.assertRaisesRegex(kcds_fbx.FbxError, "KCDS_BLENDER names no file"):
            kcds_fbx.find_blender({"KCDS_BLENDER": os.path.join(self.folder, "none.exe")})

    def test_the_newest_installed_blender_is_taken(self):
        for version in ("Blender 4.2", "Blender 5.2", "Blender 5.10"):
            folder = os.path.join(self.folder, "Blender Foundation", version)
            os.makedirs(folder)
            with open(os.path.join(folder, "blender.exe"), "wb") as file:
                file.write(b"")
        found = kcds_fbx.find_blender({"ProgramFiles": self.folder})
        self.assertTrue(found.endswith(os.path.join("Blender 5.10", "blender.exe")))


if __name__ == "__main__":
    unittest.main()
