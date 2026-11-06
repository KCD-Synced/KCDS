#!/usr/bin/env python3
"""Builds the levels' exits for server.json npcHouses from a game installation.

The levels mark where their roads leave the map with TagPoints named levelExit_<n> in
objects_mission0.xml (Data/Levels/<level>/level.pak): the places the game's own travellers come
from and go to. Without houses the level's people walk to the nearest of them at night.

    python Dev/tools/npcs/build_level_exits.py "<game folder>"

writes Backend/scripting/runtimes/js/npcs/level_exits.json: { level: [{ name, position }] }.
"""

import argparse
import json
import re
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/npcs/level_exits.json"
LEVELS = ("trosecko", "kutnohorsko")
MISSION = "objects_mission0.xml"
ENTITY = re.compile(rb"<Entity [^>]*>")
FIELD = re.compile(rb'(\w+)="([^"]*)"')
EXIT = re.compile(r"^levelExit_(\d+)$")


def read_entry(pak, name):
    # Some entries' local headers spell the path with backslashes.
    try:
        return pak.read(name)
    except zipfile.BadZipFile:
        info = pak.getinfo(name)
        info.orig_filename = name.replace("/", "\\")
        with pak.open(info) as entry:
            return entry.read()


def exits_of(level_pak):
    with zipfile.ZipFile(level_pak) as pak:
        mission = read_entry(pak, MISSION)
    exits = []
    for match in ENTITY.finditer(mission):
        fields = {
            key.decode(): value.decode("utf-8", "replace")
            for key, value in FIELD.findall(match.group(0))
        }
        name = fields.get("Name", "")
        if fields.get("EntityClass") != "TagPoint" or not EXIT.match(name):
            continue
        x, y, z = (float(part) for part in fields["Pos"].split(","))
        exits.append(
            {"name": name, "position": {"x": round(x, 2), "y": round(y, 2), "z": round(z, 2)}}
        )
    exits.sort(key=lambda row: int(EXIT.match(row["name"]).group(1)))
    return exits


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("game", help="the game's folder (the one that holds Data/)")
    args = parser.parse_args()
    result = {}
    for level in LEVELS:
        result[level] = exits_of(Path(args.game) / "Data" / "Levels" / level / "level.pak")
        print(f"{level}: {len(result[level])} exits")
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
