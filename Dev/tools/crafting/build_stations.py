"""Builds the crafting station catalog from a game installation.

The catalog is what Crafting.stations answers and what Crafting.occupancy and Crafting.session
name a station by: every smithy (Smithery) and alchemy table (AlchemyTable) placed in the game's
levels, read from each level's objects (objects_mission0.xml) and its layers (layers/*.xml) in
Data/Levels/<level>/level.pak.

A station's id is "<level>/<placement GUID>" (the entity's EntityGuid); its forward direction is
its turn applied to the level's forward axis (+y), on the ground plane.

    python Dev/tools/crafting/build_stations.py "<game folder>"

writes Backend/scripting/runtimes/js/crafting/stations.json.
"""

import argparse
import json
import math
import re
import xml.etree.ElementTree as ElementTree
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/crafting/stations.json"
KINDS = {"Smithery": "smithing", "AlchemyTable": "alchemy"}
LEVEL_FILES = re.compile(r"^(objects_mission0\.xml|layers/[^/]+\.xml)$", re.IGNORECASE)


def numbers(text, count):
    values = [float(part) for part in text.split(",")]
    if len(values) != count:
        raise ValueError(f"expected {count} numbers in {text!r}")
    return values


def forward_of(rotate):
    # Rotate is "w,x,y,z"; the forward axis +y turned by it, flattened.
    w, x, y, z = numbers(rotate, 4) if rotate else (1.0, 0.0, 0.0, 0.0)
    forward_x = 2.0 * (x * y - w * z)
    forward_y = 1.0 - 2.0 * (x * x + z * z)
    length = math.hypot(forward_x, forward_y) or 1.0
    return {"x": round(forward_x / length, 4), "y": round(forward_y / length, 4)}


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
        if "Smithery" not in text and "AlchemyTable" not in text:
            continue
        for entity in ElementTree.fromstring(text).iter("Entity"):
            kind = KINDS.get(entity.get("EntityClass", ""))
            guid = entity.get("EntityGuid")
            if not kind or not guid or not entity.get("Pos"):
                continue
            x, y, z = numbers(entity.get("Pos"), 3)
            found[guid] = {
                "id": f"{level}/{guid}",
                "kind": kind,
                "level": level,
                "layer": entity.get("EditorLayer", ""),
                "position": {"x": round(x, 3), "y": round(y, 3), "z": round(z, 3)},
                "forward": forward_of(entity.get("Rotate")),
            }
    return list(found.values())


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("game", type=Path, help="the game's installation folder")
    game = parser.parse_args().game
    stations = []
    for level_pak in sorted((game / "Data/Levels").glob("*/level.pak")):
        with zipfile.ZipFile(level_pak) as pak:
            stations.extend(stations_of(level_pak.parent.name, pak))
    stations.sort(key=lambda station: (station["level"], station["kind"], station["id"]))
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    lines = ",\n".join("  " + json.dumps(station, sort_keys=True) for station in stations)
    OUTPUT.write_text("[\n" + lines + "\n]\n", encoding="utf-8", newline="\n")
    counts = {kind: sum(1 for s in stations if s["kind"] == kind) for kind in KINDS.values()}
    print(f"{len(stations)} stations: {counts}")


if __name__ == "__main__":
    main()
