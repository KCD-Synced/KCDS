"""Builds the server's prop catalog from a game installation.

The catalog is every static mesh (.cgf) under Objects/ in the game's paks, without the level of
detail meshes, in path order, lower case: what Player.setDisguise and later
Prop.spawn take. It is stored per folder to stay small.

    python Dev/tools/props/build_catalog.py "<game folder>"

writes Backend/scripting/runtimes/js/props/catalog.json.
"""

import argparse
import json
import re
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/props/catalog.json"
# objects/.../name_lod1.cgf and the like are the engine's lower detail copies of a mesh.
LOD = re.compile(r"_lod\d*\.cgf$")


def meshes(game):
    found = set()
    for pak_path in sorted((Path(game) / "Data").glob("*.pak")):
        with zipfile.ZipFile(pak_path) as pak:
            for name in pak.namelist():
                path = name.replace("\\", "/").lower()
                if path.startswith("objects/") and path.endswith(".cgf") and not LOD.search(path):
                    found.add(path)
    return sorted(found)


def build(game):
    folders = {}
    for path in meshes(game):
        folder, _, file = path.rpartition("/")
        folders.setdefault(folder, []).append(file)
    return {"source": "Data/*.pak Objects/**/*.cgf", "folders": folders}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("game", help="the game's installation folder")
    args = parser.parse_args()
    catalog = build(args.game)
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(json.dumps(catalog, separators=(",", ":")) + "\n", encoding="utf8")
    count = sum(len(files) for files in catalog["folders"].values())
    print(f"{count} meshes in {len(catalog['folders'])} folders")


if __name__ == "__main__":
    main()
