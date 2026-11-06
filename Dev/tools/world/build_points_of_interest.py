#!/usr/bin/env python3
"""Builds the catalog of World.landmarks from a game installation.

The levels' designers keep each point of interest in a layer of its own: an entity's EditorLayer is
"Main/<group>/poi_<name>/...". A point's position is the middle of what its layers place on the map,
taken axis by axis as the median so a stray entity far away does not move it. It is not a map marker of
the game's: the map's own markers are not in these files.

    python Dev/tools/world/build_points_of_interest.py "<game folder>"

writes Backend/scripting/runtimes/js/world/points_of_interest.json: { level: [{ name, group, position }] }.
"""

import argparse
import json
import re
import statistics
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/world/points_of_interest.json"
LEVELS = ("trosecko", "kutnohorsko")
LEVEL_FILES = re.compile(r"^(objects_mission0\.xml|layers/[^/]+\.xml)$", re.IGNORECASE)
ENTITY = re.compile(r"<Entity [^>]*>")
LAYER = re.compile(r'EditorLayer="Main/([^/"]+)/(?:series_)?poi_([^/"]+)')
POSITION = re.compile(r' Pos="([-\d.]+),([-\d.]+),([-\d.]+)"')
# A position under this is relative to a parent, not a place on the map (metres).
LEAST_ON_MAP = 50.0
LEAST_ENTITIES = 2


def read_entry(pak, name):
    # Some entries' local headers spell the path with backslashes.
    try:
        return pak.read(name)
    except zipfile.BadZipFile:
        info = pak.getinfo(name)
        info.orig_filename = name.replace("/", "\\")
        with pak.open(info) as entry:
            return entry.read()


def points_of(pak):
    places = {}
    for name in sorted(pak.namelist()):
        if not LEVEL_FILES.match(name.replace("\\", "/")):
            continue
        text = read_entry(pak, name).decode("utf-8", "replace")
        for match in ENTITY.finditer(text):
            tag = match[0]
            layer = LAYER.search(tag)
            position = POSITION.search(tag)
            if not layer or not position:
                continue
            x, y, z = (float(position[axis]) for axis in (1, 2, 3))
            if x < LEAST_ON_MAP or y < LEAST_ON_MAP:
                continue
            places.setdefault((layer[2], layer[1]), []).append((x, y, z))
    rows = []
    for (point, group), positions in sorted(places.items()):
        if len(positions) < LEAST_ENTITIES:
            continue
        rows.append(
            {
                "name": point,
                "group": group,
                "position": {
                    "x": round(statistics.median(p[0] for p in positions), 2),
                    "y": round(statistics.median(p[1] for p in positions), 2),
                    "z": round(statistics.median(p[2] for p in positions), 2),
                },
            }
        )
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game", type=Path)
    args = parser.parse_args()
    catalog = {}
    for level in LEVELS:
        with zipfile.ZipFile(args.game / "Data/Levels" / level / "level.pak") as pak:
            catalog[level] = points_of(pak)
        print(f"{level}: {len(catalog[level])} points of interest")
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    with open(OUTPUT, "w", encoding="utf-8", newline="\n") as file:
        json.dump(catalog, file, indent=1, sort_keys=True)
        file.write("\n")


if __name__ == "__main__":
    main()
