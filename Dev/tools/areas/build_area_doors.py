"""Builds the catalog of the doors the levels link to their areas from a game installation.

The catalog is what Door.linkedTo, door.areas and area.doors answer. An area's link to a door is an
EntityLink named crime_door[type($enum:crime_doorKind.<kind>)] on the area, by EntityId in the level's
objects (objects_mission0.xml) and layers (layers/*.xml). A link between two layers is in the same
pak's waitinglinks.xml, by the two EntityGuids.

    python Dev/tools/areas/build_area_doors.py "<game folder>"

writes Backend/scripting/runtimes/js/areas/area_doors.json.gz: { level: { area id: [door, ...] } }, a
door being { guid, name, position: { x, y, z }, link }. The guid is as Door.guid and Door.find have it,
the level's EntityGuid with its parts the other way round (doors.hpp door_guid_text).
"""

import argparse
import gzip
import json
import re
import xml.etree.ElementTree as ElementTree
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/areas/area_doors.json.gz"
LEVELS = ("trosecko", "kutnohorsko")
LEVEL_FILES = re.compile(r"^(objects_mission0\.xml|layers/[^/]+\.xml)$", re.IGNORECASE)
LINK = re.compile(r"^crime_door\[type\(\$enum:crime_doorKind\.(\w+)\)\]$")
AREA_CLASSES = {"TriggerArea", "SmartAreaShape", "AreaUnion"}


def read_entry(pak, name):
    # Some entries' local headers spell the path with backslashes.
    try:
        return pak.read(name)
    except zipfile.BadZipFile:
        info = pak.getinfo(name)
        info.orig_filename = name.replace("/", "\\")
        with pak.open(info) as entry:
            return entry.read()


def plain(level_guid):
    return level_guid.replace("-", "").lower()


def door_guid(level_guid):
    """The level's "aaaaaaaa-bbbb-cccc" as Door.guid has it: "ccccbbbbaaaaaaaa"."""
    text = plain(level_guid)
    return text[12:16] + text[8:12] + text[0:8]


def entities_of(pak):
    for name in pak.namelist():
        normal = name.replace("\\", "/")
        if not LEVEL_FILES.match(normal):
            continue
        root = ElementTree.fromstring(read_entry(pak, name))
        for entity in root.iter():
            if entity.get("EntityGuid"):
                yield entity


def position_of(entity):
    x, y, z = (float(part) for part in (entity.get("Pos") or "0,0,0").split(","))
    return {"x": round(x, 3), "y": round(y, 3), "z": round(z, 3)}


def doors_of(pak):
    by_id = {}
    by_guid = {}
    areas = []
    for entity in entities_of(pak):
        by_guid[plain(entity.get("EntityGuid"))] = entity
        if entity.get("EntityId"):
            by_id[entity.get("EntityId")] = entity
        if entity.get("EntityClass") in AREA_CLASSES:
            areas.append(entity)
    links = {}
    for area in areas:
        area_id = plain(area.get("EntityGuid"))
        for link in area.iter("Link"):
            kind = LINK.match(link.get("Name") or "")
            target = by_id.get(link.get("TargetId"))
            if kind and target is not None:
                links.setdefault(area_id, {})[plain(target.get("EntityGuid"))] = kind[1]
    names = {name.replace("\\", "/").lower(): name for name in pak.namelist()}
    if "waitinglinks.xml" in names:
        root = ElementTree.fromstring(read_entry(pak, names["waitinglinks.xml"]))
        for waiting in root.iter("WaitingLink"):
            definition = waiting.findtext("LinkDefinition") or ""
            kind = LINK.match(definition)
            source = plain(waiting.get("SourceId") or "")
            target = plain(waiting.get("TargetId") or "")
            if kind and source in by_guid and target in by_guid:
                links.setdefault(source, {})[target] = kind[1]
    table = {}
    for area_id in sorted(links):
        rows = []
        for target, kind in sorted(links[area_id].items()):
            door = by_guid[target]
            rows.append(
                {
                    "guid": door_guid(door.get("EntityGuid")),
                    "name": door.get("Name") or "",
                    "position": position_of(door),
                    "link": kind,
                }
            )
        table[area_id] = rows
    return table


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game", type=Path)
    args = parser.parse_args()
    catalog = {}
    for level in LEVELS:
        with zipfile.ZipFile(args.game / "Data/Levels" / level / "level.pak") as pak:
            catalog[level] = doors_of(pak)
        count = sum(len(rows) for rows in catalog[level].values())
        print(f"{level}: {len(catalog[level])} areas with {count} door links")
    with gzip.open(OUTPUT, "wt", encoding="utf-8", compresslevel=9) as file:
        json.dump(catalog, file, separators=(",", ":"), sort_keys=True)


if __name__ == "__main__":
    main()
