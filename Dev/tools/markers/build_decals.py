"""Builds the catalog of the game's decal materials for Marker.list.

Every .mtl under Materials/decals/ in the game's IPL_GameData.pak is a material a decal marker
can project, named as Marker.place takes it: lower case, without the extension, for example
materials/decals/arrow.

    python Dev/tools/markers/build_decals.py "<game folder>"

writes Backend/scripting/runtimes/js/markers/decals.json.
"""

import argparse
import json
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/markers/decals.json"
PAK = Path("Data") / "IPL_GameData.pak"
FOLDER = "materials/decals/"
EXTENSION = ".mtl"


def decal_materials(game):
    names = set()
    with zipfile.ZipFile(Path(game) / PAK) as pak:
        for entry in pak.namelist():
            name = entry.replace("\\", "/").lower()
            if name.startswith(FOLDER) and name.endswith(EXTENSION):
                names.add(name[: -len(EXTENSION)])
    return sorted(names)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("game", help="the game's installation folder")
    args = parser.parse_args()
    names = decal_materials(args.game)
    OUTPUT.write_text(json.dumps(names, indent=2) + "\n", encoding="utf8")
    print(f"{len(names)} decal materials -> {OUTPUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
