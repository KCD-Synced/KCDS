#!/usr/bin/env python3
"""Builds the levels' travel points for a server that runs more than one map.

Each level's map has points the game calls FastTravelLevel (mark type 8 in whdata_0 of
Data/Levels/<level>/level.pak): a fast travel to one takes the player to another level, the one
its label names (ui_level_fasttravel_<level>). The level the player comes to puts them at its
startingPoint_<level> tag point, or where the level's own travel scene stands the player
(levelSwitch_dialogue_playerTagPoint) when it has none.

    python Dev/tools/maps/build_level_travel.py "<game folder>"

writes Backend/scripting/runtimes/js/maps/level_travel.json:
{ level: { marks: [{ id, to, position }], arrival: { position, rotation } | null } }.
"""

import argparse
import json
import re
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/maps/level_travel.json"
LEVELS = ("trosecko", "kutnohorsko")
MARKS = "whdata_0"
MISSION = "objects_mission0.xml"
MARK = re.compile(r"<Mark>(.*?)</Mark>", re.S)
ENTITY = re.compile(rb"<Entity [^>]*>")
FIELD = re.compile(rb'(\w+)="([^"]*)"')
LABEL = re.compile(r"^ui_level_fasttravel_(\w+)$")
ARRIVALS = ("startingPoint_{level}", "levelSwitch_dialogue_playerTagPoint")


def read_entry(pak, name):
    # Some entries' local headers spell the path with backslashes.
    try:
        return pak.read(name)
    except zipfile.BadZipFile:
        info = pak.getinfo(name)
        info.orig_filename = name.replace("/", "\\")
        with pak.open(info) as entry:
            return entry.read()


def tag(text, name):
    found = re.search(rf"<{name}>(.*?)</{name}>", text, re.S)
    return found.group(1).strip() if found else None


def point(text):
    x, y, z = (float(part) for part in text.split(","))
    return {"x": round(x, 2), "y": round(y, 2), "z": round(z, 2)}


def marks_of(pak):
    text = read_entry(pak, MARKS).decode("utf-8", "replace")
    marks = []
    for match in MARK.finditer(text):
        body = match.group(1)
        if tag(body, "MarkType") != "8":
            continue
        label = LABEL.match(tag(body, "CustomLabel") or "")
        position = tag(body, "Position")
        if not label or not position:
            continue
        marks.append({"id": tag(body, "Id"), "to": label.group(1), "position": point(position)})
    return marks


def arrival_of(pak, level):
    mission = read_entry(pak, MISSION)
    entities = {}
    for match in ENTITY.finditer(mission):
        fields = {
            key.decode(): value.decode("utf-8", "replace")
            for key, value in FIELD.findall(match.group(0))
        }
        entities.setdefault(fields.get("Name", ""), fields)
    for name in ARRIVALS:
        fields = entities.get(name.format(level=level))
        if not fields or "Pos" not in fields:
            continue
        arrival = {"name": name.format(level=level), "position": point(fields["Pos"])}
        if "Rotate" in fields:
            # The level writes a rotation w, x, y, z.
            w, x, y, z = (float(part) for part in fields["Rotate"].split(","))
            arrival["rotation"] = {
                "w": round(w, 5),
                "x": round(x, 5),
                "y": round(y, 5),
                "z": round(z, 5),
            }
        return arrival
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("game", help="the game's folder (the one that holds Data/)")
    args = parser.parse_args()
    result = {}
    for level in LEVELS:
        path = Path(args.game) / "Data" / "Levels" / level / "level.pak"
        with zipfile.ZipFile(path) as pak:
            result[level] = {"marks": marks_of(pak), "arrival": arrival_of(pak, level)}
        print(
            f"{level}: {len(result[level]['marks'])} travel marks, arrival {result[level]['arrival']}"
        )
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
