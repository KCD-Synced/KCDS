"""Builds the catalog of the levels' own areas from a game installation.

The catalog is what Area.all('level') and Area.getAt answer for the level a server runs.

- A TriggerArea entity (the game's interiors, houses, stealth and quest areas) is a name, an
  EntityGuid and its Label properties in a level's objects (objects_mission0.xml) and layers
  (layers/*.xml) of Data/Levels/<level>/level.pak; its prism is baked into the same pak's
  TriggerAreas.fubar: a u32 version (3), a u32 level check, a u32 count, then per prism the
  entity's key (u64: its EntityGuid's parts 1, 2 and 3 in that order, as the entity system keys it),
  a f32 height and a u32 point count with that many world points (3 x f32).
- A SmartAreaShape (a settlement's or a location's area) carries its own polygon: its Area's
  Height and Points relative to the entity's Pos (no rotation in the levels).

- An AreaUnion (a house made of rooms) joins the areas its EntityLinks named
  area_union_member point at (by EntityId); it carries its own name and labels and is kept when at
  least one member is.

    python Dev/tools/areas/build_level_areas.py "<game folder>"

writes Backend/scripting/runtimes/js/areas/level_areas.json.gz (gzip: ten thousand areas are six
megabytes of JSON): { level: [area, ...] }, an area being
{ id, name, type, labels, height, points: [x, y, z, ...] } (world metres, centimetre precision),
or for a union { id, name, type: 'AreaUnion', labels, members: [member ids] } after the areas.
"""

import argparse
import gzip
import json
import re
import struct
import xml.etree.ElementTree as ElementTree
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/areas/level_areas.json.gz"
LEVELS = ("trosecko", "kutnohorsko")
LEVEL_FILES = re.compile(r"^(objects_mission0\.xml|layers/[^/]+\.xml)$", re.IGNORECASE)
PRISM_VERSION = 3


def read_entry(pak, name):
    # Some entries' local headers spell the path with backslashes.
    try:
        return pak.read(name)
    except zipfile.BadZipFile:
        info = pak.getinfo(name)
        info.orig_filename = name.replace("/", "\\")
        with pak.open(info) as entry:
            return entry.read()


def entity_key(guid):
    """A level EntityGuid ("108c8fab-ab6e-4bf9") as the entity system keys it."""
    value = int(guid.replace("-", ""), 16)
    return (value >> 32) | (((value >> 16) & 0xFFFF) << 32) | ((value & 0xFFFF) << 48)


def prisms_of(pak):
    """The baked TriggerArea prisms: entity key -> (height, [(x, y, z), ...])."""
    names = [name for name in pak.namelist() if name.lower() == "triggerareas.fubar"]
    if not names:
        raise ValueError("the level has no TriggerAreas.fubar")
    data = read_entry(pak, names[0])
    version, _check, count = struct.unpack_from("<III", data, 0)
    if version != PRISM_VERSION:
        raise ValueError(f"TriggerAreas.fubar version {version}, expected {PRISM_VERSION}")
    prisms = {}
    offset = 12
    for _ in range(count):
        key, height, points = struct.unpack_from("<QfI", data, offset)
        values = struct.unpack_from("<" + "f" * (3 * points), data, offset + 16)
        prisms[key] = (height, [values[i : i + 3] for i in range(0, 3 * points, 3)])
        offset += 16 + 12 * points
    if offset != len(data):
        raise ValueError("TriggerAreas.fubar has bytes after its prisms")
    return prisms


def labels_of(entity):
    properties = entity.find("Properties")
    text = properties.get("Label", "") if properties is not None else ""
    return [label.strip() for label in text.split(";") if label.strip()]


def flat(points):
    out = []
    for point in points:
        out += [round(float(value), 2) for value in point]
    return out


def union_members(entity):
    """The EntityIds an AreaUnion's area_union_member links point at."""
    links = entity.find("EntityLinks")
    if links is None:
        return []
    return [
        link.get("TargetId")
        for link in links.iter("Link")
        if link.get("Name") == "area_union_member" and link.get("TargetId")
    ]


def areas_of(level, pak):
    prisms = prisms_of(pak)
    areas = []
    # EntityId -> the area's id, and the unions to join once every area is known.
    by_entity = {}
    unions = []
    for name in sorted(pak.namelist()):
        if not LEVEL_FILES.match(name):
            continue
        root = ElementTree.fromstring(read_entry(pak, name))
        for entity in root.iter("Entity"):
            kind = entity.get("EntityClass")
            guid = entity.get("EntityGuid") or ""
            if kind == "AreaUnion" and guid:
                unions.append(entity)
                continue
            if kind not in ("TriggerArea", "SmartAreaShape") or not guid:
                continue
            area = {
                "id": guid.replace("-", "").lower(),
                "name": entity.get("Name", ""),
                "type": kind,
                "labels": labels_of(entity),
            }
            if kind == "TriggerArea":
                prism = prisms.get(entity_key(guid))
                if not prism:
                    continue
                area["height"] = round(prism[0], 2)
                area["points"] = flat(prism[1])
            else:
                shape = entity.find("Area")
                if shape is None:
                    continue
                origin = [float(value) for value in entity.get("Pos", "0,0,0").split(",")]
                points = []
                for point in shape.iter("Point"):
                    offset = [float(value) for value in point.get("Pos").split(",")]
                    points.append([origin[i] + offset[i] for i in range(3)])
                if len(points) < 3:
                    continue
                area["height"] = round(float(shape.get("Height", "0")), 2)
                area["points"] = flat(points)
            areas.append(area)
            if entity.get("EntityId"):
                by_entity[entity.get("EntityId")] = area["id"]
    areas.sort(key=lambda area: area["id"])
    joined = []
    for entity in unions:
        members = sorted(
            {by_entity[target] for target in union_members(entity) if target in by_entity}
        )
        if not members:
            continue
        joined.append(
            {
                "id": entity.get("EntityGuid").replace("-", "").lower(),
                "name": entity.get("Name", ""),
                "type": "AreaUnion",
                "labels": labels_of(entity),
                "members": members,
            }
        )
    joined.sort(key=lambda union: union["id"])
    return areas + joined


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("game")
    options = parser.parse_args()
    levels = Path(options.game) / "Data" / "Levels"
    catalog = {}
    for level in LEVELS:
        with zipfile.ZipFile(levels / level / "level.pak") as pak:
            catalog[level] = areas_of(level, pak)
        unions = sum(1 for area in catalog[level] if area["type"] == "AreaUnion")
        print(f"{level}: {len(catalog[level]) - unions} areas, {unions} unions")
    text = json.dumps(catalog, separators=(",", ":")) + "\n"
    # mtime 0: the same catalog gives the same bytes.
    OUTPUT.write_bytes(gzip.compress(text.encode("utf-8"), mtime=0))
    print(f"{OUTPUT} {OUTPUT.stat().st_size} bytes")


if __name__ == "__main__":
    main()
