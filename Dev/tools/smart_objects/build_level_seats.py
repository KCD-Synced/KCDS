"""Builds the catalog of the seats and beds placed in the game's levels, for Npc.useObject.

The game's people sit and lie on the levels' SmartObjectHolder and StanceSmartObject entities whose
soclass_SmartObjectHelpers is a seat or bed helper class (Sit_..., Bed_...) in a level's objects
(objects_mission0.xml) and layers (layers/*.xml) of Data/Levels/<level>/level.pak. Only the
classes Npc.useObject knows (smart_objects/use_helpers.json) are kept.

    python Dev/tools/smart_objects/build_level_seats.py "<game folder>"

writes Backend/scripting/runtimes/js/smart_objects/level_seats.json: per level, one row a seat,
[guid, helper class, x, y, z, qx, qy, qz, qw]: the entity's level GUID as sixteen lowercase hex
digits, where it stands in the world and how it is turned (the level's Rotate is w x y z).
"""

import argparse
import json
import re
import xml.etree.ElementTree as ElementTree
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/smart_objects/level_seats.json"
HELPERS = ROOT / "Backend/scripting/runtimes/js/smart_objects/use_helpers.json"
LEVELS = ("trosecko", "kutnohorsko")
LEVEL_FILES = re.compile(r"^(objects_mission0\.xml|layers/[^/]+\.xml)$", re.IGNORECASE)
# The entities a seat or a bed is: a prefab's holder, and the level's own stance places.
SEAT_CLASSES = {"SmartObjectHolder", "StanceSmartObject"}


def numbers(text, count):
    values = [float(part) for part in text.split(",")]
    if len(values) != count:
        raise ValueError(f"expected {count} numbers in {text!r}")
    return values


def read_entry(pak, name):
    # Some entries' local headers spell the path with backslashes.
    try:
        return pak.read(name)
    except zipfile.BadZipFile:
        info = pak.getinfo(name)
        info.orig_filename = name.replace("/", "\\")
        with pak.open(info) as entry:
            return entry.read()


def seats_of(pak, known):
    seats = {}
    for name in sorted(pak.namelist()):
        if not LEVEL_FILES.match(name):
            continue
        root = ElementTree.fromstring(read_entry(pak, name))
        for entity in root.iter("Entity"):
            if entity.get("EntityClass") not in SEAT_CLASSES:
                continue
            properties = entity.find("Properties")
            helper = (
                properties.get("soclass_SmartObjectHelpers") if properties is not None else None
            )
            guid = (entity.get("EntityGuid") or "").replace("-", "").lower()
            if helper not in known or not guid or not entity.get("Pos"):
                continue
            x, y, z = numbers(entity.get("Pos"), 3)
            w, qx, qy, qz = numbers(entity.get("Rotate") or "1,0,0,0", 4)
            seats[guid] = [
                guid,
                helper,
                round(x, 3),
                round(y, 3),
                round(z, 3),
                round(qx, 5),
                round(qy, 5),
                round(qz, 5),
                round(w, 5),
            ]
    return [seats[guid] for guid in sorted(seats)]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("game", help="the game's folder")
    game = Path(parser.parse_args().game)
    known = set(json.loads(HELPERS.read_text(encoding="utf-8")))
    levels = {}
    for level in LEVELS:
        with zipfile.ZipFile(game / "Data/Levels" / level / "level.pak") as pak:
            levels[level] = seats_of(pak, known)
        print(f"{level}: {len(levels[level])} seats and beds")
    lines = []
    for level, rows in levels.items():
        body = ",\n".join(f"    {json.dumps(row)}" for row in rows)
        lines.append(f'  "{level}": [\n{body}\n  ]')
    OUTPUT.write_text("{\n" + ",\n".join(lines) + "\n}\n", encoding="utf-8", newline="\n")
    print(f"wrote {OUTPUT}")


if __name__ == "__main__":
    main()
