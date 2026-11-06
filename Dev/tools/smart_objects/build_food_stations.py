"""Builds the catalog of the levels' drying racks and smokehouses from a game
installation.

SmartObject.stations lists them beside the dice tables and the crafting stations, and
SmartObject.remove takes one out of a dimension. A rack or a smokehouse is the level's
FoodProcessingTrigger entity whose script says "foodProcessingType:drying" or
"foodProcessingType:smoking" (a "cooking" one is a fire, which is CookPot's), read from each
level's objects (objects_mission0.xml) and layers (layers/*.xml) in Data/Levels/<level>/level.pak.

A station's id is its EntityGuid as sixteen lowercase hex digits, as LevelEdit names an entity.

    python Dev/tools/smart_objects/build_food_stations.py "<game folder>"

writes Backend/scripting/runtimes/js/smart_objects/food_stations.json, one row a line:
[level, id, kind, x, y, z].
"""

import argparse
import re
import xml.etree.ElementTree as ElementTree
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/smart_objects/food_stations.json"
LEVEL_FILES = re.compile(r"^(objects_mission0\.xml|layers/[^/]+\.xml)$", re.IGNORECASE)
KINDS = {"foodProcessingType:drying": "drying", "foodProcessingType:smoking": "smoking"}


def read_entry(pak, name):
    # Some entries' local headers spell the path with backslashes.
    try:
        return pak.read(name)
    except zipfile.BadZipFile:
        info = pak.getinfo(name)
        info.orig_filename = name.replace("/", "\\")
        with pak.open(info) as entry:
            return entry.read()


def stations_of(level, pak):
    found = {}
    for name in pak.namelist():
        if not LEVEL_FILES.match(name):
            continue
        text = read_entry(pak, name).decode("utf-8", errors="replace")
        if "FoodProcessingTrigger" not in text:
            continue
        for entity in ElementTree.fromstring(text).iter("Entity"):
            if entity.get("EntityClass") != "FoodProcessingTrigger":
                continue
            script = entity.find("Properties/Script")
            kind = KINDS.get(script.get("Misc", "") if script is not None else "")
            guid = entity.get("EntityGuid", "").replace("-", "").lower()
            position = entity.get("Pos")
            if not kind or len(guid) != 16 or not position:
                continue
            x, y, z = (float(part) for part in position.split(","))
            found[guid] = [level, guid, kind, round(x, 3), round(y, 3), round(z, 3)]
    return list(found.values())


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("game", type=Path, help="the game's installation folder")
    game = parser.parse_args().game
    rows = []
    for level_pak in sorted((game / "Data/Levels").glob("*/level.pak")):
        with zipfile.ZipFile(level_pak) as pak:
            rows.extend(stations_of(level_pak.parent.name, pak))
    rows.sort(key=lambda row: (row[0], row[2], row[1]))
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    lines = ",\n".join(
        f'  ["{row[0]}", "{row[1]}", "{row[2]}", {row[3]}, {row[4]}, {row[5]}]' for row in rows
    )
    OUTPUT.write_text("[\n" + lines + "\n]\n", encoding="utf-8", newline="\n")
    counts = {kind: sum(1 for row in rows if row[2] == kind) for kind in KINDS.values()}
    print(f"{len(rows)} stations: {counts}")


if __name__ == "__main__":
    main()
