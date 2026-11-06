#!/usr/bin/env python3
"""Builds the catalog of Npc.placements from a game installation.

The levels place their people as NPC and NPC_Female entities in objects_mission0.xml and layers/*.xml.
An entity names its soul by its own name: its Name is a soul_name of the soul tables
(Libs/Tables/rpg/soul*.xml in Tables.pak). A placement whose name matches no soul is left out, as it
is the stand-in of a script or a quest's and has no soul to give.

    python Dev/tools/npcs/build_placements.py "<game folder>"

writes Backend/scripting/runtimes/js/npcs/placements.json.gz: { level: [placement, ...] }, a placement
being { name, guid, layer, soul, actorClass, position, rotation }.
"""

import argparse
import gzip
import json
import re
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/npcs/placements.json.gz"
LEVELS = ("trosecko", "kutnohorsko")
LEVEL_FILES = re.compile(r"^(objects_mission0\.xml|layers/[^/]+\.xml)$", re.IGNORECASE)
SOUL_FILES = re.compile(r"^Libs/Tables/rpg/soul(__[^/]*)?\.xml$", re.IGNORECASE)
SOUL = re.compile(r"<soul [^>]*>")
ENTITY = re.compile(r'<Entity [^>]*EntityClass="(?:NPC|NPC_Female)"[^>]*>')
FIELD = re.compile(r'(\w+)="([^"]*)"')


def read_entry(pak, name):
    # Some entries' local headers spell the path with backslashes.
    try:
        return pak.read(name)
    except zipfile.BadZipFile:
        info = pak.getinfo(name)
        info.orig_filename = name.replace("/", "\\")
        with pak.open(info) as entry:
            return entry.read()


def souls_of(game):
    souls = {}
    with zipfile.ZipFile(game / "Data/Tables.pak") as pak:
        for name in pak.namelist():
            if not SOUL_FILES.match(name):
                continue
            text = pak.read(name).decode("utf-8", "replace")
            for tag in SOUL.finditer(text):
                fields = dict(FIELD.findall(tag[0]))
                if fields.get("soul_name") and fields.get("soul_id"):
                    souls[fields["soul_name"]] = fields["soul_id"]
    return souls


def numbers(text, fallback):
    if not text:
        return fallback
    return [float(part) for part in text.split(",")]


def placements_of(pak, souls):
    rows = {}
    for name in sorted(pak.namelist()):
        if not LEVEL_FILES.match(name.replace("\\", "/")):
            continue
        text = read_entry(pak, name).decode("utf-8", "replace")
        for tag in ENTITY.finditer(text):
            fields = dict(FIELD.findall(tag[0]))
            soul = souls.get(fields.get("Name", ""))
            guid = fields.get("EntityGuid", "").replace("-", "").lower()
            if not soul or not guid:
                continue
            x, y, z = numbers(fields.get("Pos"), [0.0, 0.0, 0.0])
            w, rx, ry, rz = numbers(fields.get("Rotate"), [1.0, 0.0, 0.0, 0.0])
            rows[guid] = {
                "name": fields["Name"],
                "guid": guid,
                "layer": fields.get("EditorLayer", ""),
                "soul": soul,
                "actorClass": fields["EntityClass"],
                "position": {"x": round(x, 3), "y": round(y, 3), "z": round(z, 3)},
                "rotation": {
                    "w": round(w, 5),
                    "x": round(rx, 5),
                    "y": round(ry, 5),
                    "z": round(rz, 5),
                },
            }
    return [rows[guid] for guid in sorted(rows)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game", type=Path)
    args = parser.parse_args()
    souls = souls_of(args.game)
    print(f"{len(souls)} souls")
    catalog = {}
    for level in LEVELS:
        with zipfile.ZipFile(args.game / "Data/Levels" / level / "level.pak") as pak:
            catalog[level] = placements_of(pak, souls)
        print(f"{level}: {len(catalog[level])} placements")
    with gzip.open(OUTPUT, "wt", encoding="utf-8", compresslevel=9) as file:
        json.dump(catalog, file, separators=(",", ":"), sort_keys=True)


if __name__ == "__main__":
    main()
