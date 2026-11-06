"""Builds the catalog of the door leaves the levels hang from a game installation.

The catalog is what Door.models lists and Door.spawn takes: every leaf mesh (Properties
object_Model) an AnimDoor entity of the game's levels uses, in a level's objects
(objects_mission0.xml) and layers (layers/*.xml) of Data/Levels/<level>/level.pak. A row keeps the
mesh path, the side its hinge is on, how many doors of the levels hang it, and the door properties
those doors give it most often, which a door a script hangs is built with: the animation set
(esDoorAnimSet, VillageLeft or VillageRight, which also gives the hinge), the smart object helpers
and type the game's NPCs use a door by, and its physics (bRigidBody, bPushableByPlayers).

    python Dev/tools/doors/build_door_models.py "<game folder>"

writes Backend/scripting/runtimes/js/doors/door_models.json: one row a line,
[path, hinge, placements, animSet, helpers, smartObjectType, rigid, pushable].
"""

import argparse
import collections
import json
import re
import xml.etree.ElementTree as ElementTree
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/doors/door_models.json"
LEVEL_FILES = re.compile(r"^(objects_mission0\.xml|layers/[^/]+\.xml)$", re.IGNORECASE)
DOOR_CLASS = "AnimDoor"
ANIM_SET_HINGES = {"VillageLeft": "left", "VillageRight": "right"}


def read_entry(pak, name):
    # Some entries' local headers spell the path with backslashes.
    try:
        return pak.read(name)
    except zipfile.BadZipFile:
        info = pak.getinfo(name)
        info.orig_filename = name.replace("/", "\\")
        with pak.open(info) as entry:
            return entry.read()


def flag(element, name, default):
    if element is None or element.get(name) is None:
        return default
    return element.get(name) not in ("0", "false")


def door_properties(pak, seen):
    for name in pak.namelist():
        if not LEVEL_FILES.match(name):
            continue
        text = read_entry(pak, name).decode("utf-8", errors="replace")
        for entity in ElementTree.fromstring(text).iter("Entity"):
            if entity.get("EntityClass") != DOOR_CLASS:
                continue
            properties = entity.find("Properties")
            model = properties.get("object_Model", "") if properties is not None else ""
            if not model:
                continue
            physics = properties.find("Physics")
            seen[model.lower()].append(
                (
                    properties.get("esDoorAnimSet", ""),
                    properties.get("soclass_SmartObjectHelpers", ""),
                    properties.get("guidSmartObjectType", ""),
                    flag(physics, "bRigidBody", False),
                    flag(physics, "bPushableByPlayers", False),
                )
            )


def most_common(values):
    return collections.Counter(values).most_common(1)[0][0]


def row_of(path, placements):
    anim_set = most_common(entry[0] for entry in placements)
    stem = path.rsplit("/", 1)[-1]
    hinge = ANIM_SET_HINGES.get(anim_set) or ("right" if "right" in stem else "left")
    if anim_set not in ANIM_SET_HINGES:
        anim_set = "VillageRight" if hinge == "right" else "VillageLeft"
    return [
        path,
        hinge,
        len(placements),
        anim_set,
        most_common(entry[1] for entry in placements),
        most_common(entry[2] for entry in placements),
        most_common(entry[3] for entry in placements),
        most_common(entry[4] for entry in placements),
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("game", help="the game's installation folder")
    args = parser.parse_args()
    game = Path(args.game)
    seen = collections.defaultdict(list)
    for level_pak in sorted((game / "Data/Levels").glob("*/level.pak")):
        with zipfile.ZipFile(level_pak) as pak:
            door_properties(pak, seen)
    rows = [row_of(path, seen[path]) for path in sorted(seen)]
    lines = ["["]
    lines.append(",\n".join("  " + json.dumps(row) for row in rows))
    lines.append("]")
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"{OUTPUT}: {len(rows)} door models")


if __name__ == "__main__":
    main()
