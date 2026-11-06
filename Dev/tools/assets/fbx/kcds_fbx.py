"""An FBX file as glTF 2.0 for the studio's glTF imports, through Blender's own FBX
reader run headless: FBX is a closed format, and Blender reads it as well as any free tool does.

    python kcds_fbx.py <file.fbx> <out.glb>

Finds Blender at KCDS_BLENDER, else the newest "Blender Foundation\\Blender <version>\\blender.exe"
in Program Files. Blender starts with its factory settings and no add-ons of the user, imports
the file into an empty scene and exports the scene as a .glb (meshes, skins, animations,
materials, textures packed in). Answers a JSON line: { glb, blender } or { error }. The .glb then
goes to kcds_gltf.py, kcds_gltf_skin.py or kcds_gltf_anim.py as any glTF file.
"""

import glob
import json
import os
import subprocess
import sys

BLENDER_SCRIPT = (
    "import bpy, sys\n"
    "args = sys.argv[sys.argv.index('--') + 1:]\n"
    "bpy.ops.wm.read_factory_settings(use_empty=True)\n"
    "bpy.ops.import_scene.fbx(filepath=args[0])\n"
    "bpy.ops.export_scene.gltf(filepath=args[1], export_format='GLB', export_skins=True, "
    "export_animations=True)\n"
)
TIMEOUT_SECONDS = 120


class FbxError(ValueError):
    """An FBX file that could not be made glTF."""


def _version_key(path):
    folder = os.path.basename(os.path.dirname(path))
    parts = folder.replace("Blender", "").strip().split(".")
    return tuple(int(part) if part.isdigit() else 0 for part in parts)


def find_blender(environment=os.environ):
    given = environment.get("KCDS_BLENDER")
    if given:
        if not os.path.isfile(given):
            raise FbxError(f"KCDS_BLENDER names no file: {given}")
        return given
    found = []
    for root in (environment.get("ProgramFiles", r"C:\Program Files"), r"C:\Program Files"):
        found.extend(glob.glob(os.path.join(root, "Blender Foundation", "Blender*", "blender.exe")))
    if not found:
        raise FbxError("Blender is not installed (KCDS_BLENDER names blender.exe elsewhere)")
    return max(set(found), key=_version_key)


def convert(fbx_path, glb_path, environment=os.environ, run=subprocess.run):
    if not fbx_path.lower().endswith(".fbx") or not os.path.isfile(fbx_path):
        raise FbxError(f"not an FBX file: {fbx_path}")
    blender = find_blender(environment)
    if os.path.exists(glb_path):
        os.remove(glb_path)
    command = [
        blender,
        "-b",
        "--factory-startup",
        "--python-expr",
        BLENDER_SCRIPT,
        "--",
        os.path.abspath(fbx_path),
        os.path.abspath(glb_path),
    ]
    try:
        finished = run(command, capture_output=True, text=True, timeout=TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired as error:
        raise FbxError(f"Blender took longer than {TIMEOUT_SECONDS} s") from error
    if not os.path.isfile(glb_path):
        lines = [
            line for line in (finished.stdout + finished.stderr).splitlines() if "rror" in line
        ]
        detail = lines[-1].strip() if lines else f"exit code {finished.returncode}"
        raise FbxError(f"Blender did not convert the file: {detail}")
    return {"glb": glb_path, "blender": blender}


def main(argv):
    if len(argv) != 2:
        print("usage: python kcds_fbx.py <file.fbx> <out.glb>")
        return 2
    try:
        made = convert(argv[0], argv[1])
    except (FbxError, OSError) as error:
        print(json.dumps({"error": str(error)}))
        return 1
    print(json.dumps(made))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
