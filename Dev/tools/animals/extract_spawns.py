"""Extracts the game's animal spawn points for the server's animal population.

    python Dev/tools/animals/extract_spawns.py [--game "<game folder>"]

writes Backend/scripting/runtimes/js/animals/spawn_points.json. It reads, read only, the entity
files of the open-world levels (Data/Levels/<level>/level.pak: objects_mission0.xml for the
always-present world and layers/*.xml for the quest layers that stream in), the level's trigger
area polygons (triggerareas.fubar), the souls and soul archetypes (Data/Tables.pak,
Libs/Tables/rpg/soul__*.xml and soul_archetype.xml) and the spawner prefabs (Data/IPL_GameData.pak,
Prefabs/Animal/*.xml). Decoys the game keeps at the level's corner (layer Main/_script/
animalPrecache) are skipped. The levels are trosecko and kutnohorsko; klaster is the monastery
add-on and not part of the open world.

An entry is one spawn point:

* type "spawner": an AnimalSpawner entity. It spawns "count" animals of its soul inside the polygon
  of the TriggerArea it links as spawnArea, and again "respawnDays" days after they are gone.
  "flock" tells that they spawn as one group. A spawner without an area has "master", the guid of
  the spawner whose area it shares (it adds its own kind to that spawner's group). What an entity
  does not set comes from its prefab, then from the entity class (AnimalSpawner.lua in
  Data/Scripts.pak: 5 animals, 14 days, in a flock).
* type "placed": an animal entity put at a fixed place (a farm's cow, a dog at its home, a quest's
  wolf). "count" is 1.
* type "flock": a group entity of small animals (Chickens, Rats, FieldMouse ...). "count" is its
  Boid nCount, "radius" the distance its animals roam from the entity when the data gives one.

The fields of an entry (a missing field was not in the data):

    level       trosecko or kutnohorsko
    type        spawner, placed or flock
    kind        the animal class (RoeDeerHind, Wolf ...) or the flock class (Chickens ...)
    pos         [x, y, z] in metres, rounded to 0.1
    count       animals per spawn
    respawnDays spawner only
    flock       spawner only, true when its animals spawn as one group
    defaults    spawner only, the properties (AnimalCount, RespawnDays, SpawnInFlock) that neither
                the entity nor its prefab set, so "count", "respawnDays" or "flock" is the entity
                class's default and only a guess of what the game spawns
    soul        spawner only, the soul name (animal_roe_deer_hind ...) or its guid when unknown
    soulGuid    spawner only, the soul_id (36 characters with dashes, lower case) of that soul, as
                the souls tables (soul__*.xml) list it, which is what the server spawns a body by.
                The level data names the soul by this guid, so it is kept as it stands, also when
                the tables do not list it. A spawner without a valid soul guid has none.
                Placed and flock entries carry no soul in the data and have no soulGuid either: the
                server spawns them with the defaultSoulGuid of their kind
    master      spawner only, guid of the spawner it shares an area with
    area        spawner only: {"name", "guid", "pos", "height", "points": [[x, y, z], ...]}, the
                polygon of the spawn area, "height" its extent above the points
    streamed    true when the entity is in a quest layer (it exists only while the quest streams
                it in), absent for the always-present world
    layer       the editor layer path of the entity
    name, guid  the source entity's name and guid

The file's header, next to the entries, holds:

    levels            the levels that were read
    summary           per level: "entries" (how many of each type), "kinds" (per kind the entries
                      and the animals they spawn) and "soulGuids" (per type how many entries have
                      a soulGuid, "resolved", and how many have none, "unresolved")
    defaultSoulGuids  per kind the soul guid to spawn an animal of that kind with when its entry
                      names no soul (placed and flock entries). It is the soulGuid that most spawner
                      entries of the kind have, over all levels; of several with the same number
                      of entries the one that sorts first wins. A kind that no spawner entry
                      names a soul for has no default.

triggerareas.fubar holds, after the header (u32 version 3, u32 checksum, u32 area count), one
record per area: u32 + u16 + u16 guid (the entity's EntityGuid), f32 height, u32 point count and
the points as three f32 each. All numbers are little endian.
"""

import argparse
import io
import json
import re
import struct
import xml.etree.ElementTree as ElementTree
import zipfile
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/animals/spawn_points.json"
DEFAULT_GAME = r"C:\Program Files (x86)\Steam\steamapps\common\KingdomComeDeliverance2"
LEVELS = ("trosecko", "kutnohorsko")
WORLD_FILE = "objects_mission0.xml"
LAYER_FOLDER = "layers/"
AREA_FILE = "triggerareas.fubar"
AREA_VERSION = 3
SOUL_TABLES = "Libs/Tables/rpg/soul__"
ARCHETYPE_TABLE = "Libs/Tables/rpg/soul_archetype.xml"
PREFAB_FOLDER = "Prefabs/Animal/"

SPAWNER_CLASS = "AnimalSpawner"
AREA_CLASS = "TriggerArea"
PLACED_CLASSES = (
    "Boar",
    "CattleBull",
    "CattleCow",
    "Dog",
    "Hare",
    "Hen",
    "Pig",
    "Raven",
    "RedDeerDoe",
    "RedDeerStag",
    "RoeDeerBuck",
    "RoeDeerHind",
    "SheepEwe",
    "SheepRam",
    "WildDog",
    "Wolf",
)
FLOCK_CLASSES = ("Chickens", "ChickensBrownLight", "ChickensWhite", "FieldMouse", "Rats")
# The editor layer of the animals the game spawns once to load their assets, far from the map.
PRECACHE_LAYER = "Main/_script/animalPrecache"
# What the AnimalSpawner entity class gives a property that neither the prefab nor the entity sets.
SPAWNER_DEFAULTS = {"AnimalCount": "5", "RespawnDays": "14", "SpawnInFlock": "1"}
# The prefab of an entity is in its name, between brackets: Name[Folder/prefab12_<guid>].
PREFAB_NAME = re.compile(r"\[(?P<path>[^\]]*?[^\d\]])\d*_[0-9a-f]{8}-")
# A soul_id as the souls tables write it, in lower case.
SOUL_GUID = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")


def read_member(pak, name):
    """Reads a member of the pak. The packs write the member names with backslashes in the local
    headers and slashes in the directory, which zipfile refuses to read, so the data is read
    from the local header's end."""
    info = pak.getinfo(name)
    pak.fp.seek(info.header_offset)
    header = pak.fp.read(30)
    name_length, extra_length = struct.unpack("<HH", header[26:30])
    pak.fp.seek(info.header_offset + 30 + name_length + extra_length)
    packed = pak.fp.read(info.compress_size)
    if info.compress_type == zipfile.ZIP_STORED:
        return packed
    if info.compress_type == zipfile.ZIP_DEFLATED:
        return zlib.decompress(packed, -15)
    raise SystemExit(f"{name}: unsupported compression {info.compress_type}")


def parse_vector(text):
    if not text:
        return [0.0, 0.0, 0.0]
    return [float(part) for part in text.split(",")]


def rounded(vector):
    return [round(value, 1) + 0.0 for value in vector]


def read_souls(data):
    """Maps a soul's guid to {"name", "kind"}, the kind being the soul archetype's name."""
    souls = {}
    with zipfile.ZipFile(data / "Tables.pak") as pak:
        archetypes = {}
        archetype_root = ElementTree.fromstring(read_member(pak, ARCHETYPE_TABLE))
        for node in archetype_root.iter("soul_archetype"):
            archetypes[node.get("soul_archetype_id")] = node.get("soul_archetype_name")
        for name in sorted(pak.namelist()):
            if not (name.startswith(SOUL_TABLES) and name.endswith(".xml")):
                continue
            for node in ElementTree.fromstring(read_member(pak, name)).iter("soul"):
                souls[node.get("soul_id").lower()] = {
                    "name": node.get("soul_name"),
                    "kind": archetypes.get(node.get("soul_archetype_id")),
                }
    return souls


def read_prefab_defaults(data):
    """Maps a prefab path (Animal/deerSpawner) to the properties its AnimalSpawner sets."""
    defaults = {}
    with zipfile.ZipFile(data / "IPL_GameData.pak") as pak:
        for name in sorted(pak.namelist()):
            if not (name.startswith(PREFAB_FOLDER) and name.endswith(".xml")):
                continue
            for node in ElementTree.fromstring(read_member(pak, name)).iter("Object"):
                properties = node.find("Properties")
                if node.get("EntityClass") == SPAWNER_CLASS and properties is not None:
                    path = name[len("Prefabs/") : -len(".xml")]
                    defaults.setdefault(path, dict(properties.attrib))
    return defaults


def read_areas(pak):
    """Maps an area's guid to {"height", "points"}, from the level's trigger area file."""
    if AREA_FILE not in pak.namelist():
        return {}
    data = read_member(pak, AREA_FILE)
    version, _checksum, count = struct.unpack_from("<III", data, 0)
    if version != AREA_VERSION:
        raise SystemExit(f"{AREA_FILE}: version {version}, expected {AREA_VERSION}")
    areas = {}
    offset = 12
    for _index in range(count):
        first, second, third = struct.unpack_from("<IHH", data, offset)
        height, point_count = struct.unpack_from("<fI", data, offset + 8)
        offset += 16
        points = []
        for _point in range(point_count):
            points.append(rounded(struct.unpack_from("<fff", data, offset)))
            offset += 12
        areas[f"{first:08x}-{second:04x}-{third:04x}"] = {
            "height": round(height, 1),
            "points": points,
        }
    if offset != len(data):
        raise SystemExit(f"{AREA_FILE}: {len(data) - offset} bytes left after {count} areas")
    return areas


def describe(element):
    """The parts of an entity element the extraction needs, as a plain record."""
    properties = element.find("Properties")
    boid = element.find("Properties/Boid")
    movement = element.find("Properties/Movement")
    options = element.find("Properties/Options")
    radius = None
    if movement is not None and movement.get("MaxDistFromOrigin"):
        radius = float(movement.get("MaxDistFromOrigin"))
    elif options is not None and options.get("Radius"):
        radius = float(options.get("Radius"))
    links = {}
    for link in element.findall("EntityLinks/Link"):
        links.setdefault(link.get("Name"), link.get("TargetId"))
    return {
        "class": element.get("EntityClass"),
        "id": element.get("EntityId"),
        "guid": element.get("EntityGuid"),
        "name": element.get("Name"),
        "pos": parse_vector(element.get("Pos")),
        "layer": element.get("EditorLayer", ""),
        "properties": dict(properties.attrib) if properties is not None else {},
        "boid_count": int(boid.get("nCount")) if boid is not None and boid.get("nCount") else None,
        "radius": radius,
        "links": links,
    }


def read_entities(data, classes):
    """The entities of the wanted classes of one entity file, one record each."""
    records = []
    for _event, element in ElementTree.iterparse(io.BytesIO(data)):
        # An entity element also stands for a reference inside an area, which has no class.
        if element.tag != "Entity" or element.get("EntityClass") is None:
            continue
        if element.get("EntityClass") in classes:
            records.append(describe(element))
        element.clear()
    return records


def read_level(game, level):
    """Every record of the level's world and layers, and the level's area polygons."""
    classes = {SPAWNER_CLASS, *PLACED_CLASSES, *FLOCK_CLASSES}
    needles = [f'EntityClass="{name}"'.encode() for name in sorted(classes)]
    records = []
    with zipfile.ZipFile(Path(game) / "Data/Levels" / level / "level.pak") as pak:
        for name in sorted(pak.namelist()):
            is_layer = name.startswith(LAYER_FOLDER) and name.endswith(".xml")
            if name != WORLD_FILE and not is_layer:
                continue
            data = read_member(pak, name)
            if name != WORLD_FILE and not any(needle in data for needle in needles):
                continue
            for record in read_entities(data, classes | {AREA_CLASS}):
                record["streamed"] = name != WORLD_FILE
                records.append(record)
        return records, read_areas(pak)


def is_set(value):
    return str(value).lower() in ("1", "true")


def spawner_properties(record, prefab_defaults):
    """The spawner's properties (the entity's, else its prefab's, else the entity class's) and
    the names of those that only the entity class gave."""
    properties = {}
    match = PREFAB_NAME.search(record["name"] or "")
    if match:
        properties.update(prefab_defaults.get(match.group("path"), {}))
    properties.update(record["properties"])
    defaulted = sorted(name for name in SPAWNER_DEFAULTS if name not in properties)
    for name in defaulted:
        properties[name] = SPAWNER_DEFAULTS[name]
    return properties, defaulted


def base_entry(level, kind_type, record):
    entry = {
        "level": level,
        "type": kind_type,
        "kind": record["class"],
        "pos": rounded(record["pos"]),
        "name": record["name"],
        "guid": record["guid"],
    }
    if record["layer"]:
        entry["layer"] = record["layer"]
    if record["streamed"]:
        entry["streamed"] = True
    return entry


def area_of(record, records_by_id, polygons):
    """The spawn area of a spawner: its linked area entity with the area's polygon."""
    target = records_by_id.get(record["links"].get("spawnArea"))
    if target is None:
        return None
    area = {"name": target["name"], "guid": target["guid"], "pos": rounded(target["pos"])}
    polygon = polygons.get(target["guid"])
    if polygon is not None:
        area["height"] = polygon["height"]
        area["points"] = polygon["points"]
    return area


def spawner_entry(level, record, records_by_id, polygons, souls, prefab_defaults):
    properties, defaulted = spawner_properties(record, prefab_defaults)
    guid = properties.get("guidSoul", "").lower()
    soul = souls.get(guid)
    entry = base_entry(level, "spawner", record)
    entry["kind"] = soul["kind"] if soul and soul["kind"] else "unknown"
    entry["soul"] = soul["name"] if soul else guid
    # The soul name came from this guid, so the guid is the name's soul_id.
    if SOUL_GUID.fullmatch(guid):
        entry["soulGuid"] = guid
    entry["count"] = int(properties["AnimalCount"])
    entry["respawnDays"] = int(properties["RespawnDays"])
    entry["flock"] = is_set(properties["SpawnInFlock"])
    if defaulted:
        entry["defaults"] = defaulted
    master = records_by_id.get(record["links"].get("masterSpawner"))
    if master is not None:
        entry["master"] = master["guid"]
    area = area_of(record, records_by_id, polygons)
    if area is not None:
        entry["area"] = area
    return entry


def flock_entry(level, record):
    entry = base_entry(level, "flock", record)
    if record["boid_count"] is not None:
        entry["count"] = record["boid_count"]
    if record["radius"] is not None:
        entry["radius"] = record["radius"]
    return entry


def level_entries(game, level, souls, prefab_defaults):
    records, polygons = read_level(game, level)
    records_by_id = {record["id"]: record for record in records}
    entries = []
    for record in records_by_id.values():
        if PRECACHE_LAYER in record["layer"]:
            continue
        entity_class = record["class"]
        if entity_class == SPAWNER_CLASS:
            entries.append(
                spawner_entry(level, record, records_by_id, polygons, souls, prefab_defaults)
            )
        elif entity_class in PLACED_CLASSES:
            entry = base_entry(level, "placed", record)
            entry["count"] = 1
            entries.append(entry)
        elif entity_class in FLOCK_CLASSES:
            entries.append(flock_entry(level, record))
    return entries


def sort_key(entry):
    return (entry["level"], entry["type"], entry["kind"], entry["pos"], entry["guid"])


def summarize(entries):
    """Per level and type: how many entries there are and how many have a soulGuid, and per kind
    how many animals they spawn."""
    summary = {}
    for entry in entries:
        level = summary.setdefault(entry["level"], {"entries": {}, "kinds": {}, "soulGuids": {}})
        level["entries"][entry["type"]] = level["entries"].get(entry["type"], 0) + 1
        counts = level["soulGuids"].setdefault(entry["type"], {"resolved": 0, "unresolved": 0})
        counts["resolved" if "soulGuid" in entry else "unresolved"] += 1
        kind = level["kinds"].setdefault(entry["kind"], {"entries": 0, "animals": 0})
        kind["entries"] += 1
        kind["animals"] += entry.get("count", 0)
    return summary


def default_soul_guids(entries):
    """Per kind the soulGuid most entries of the kind have (of several with as many entries the
    one that sorts first), over all levels. A kind without any soulGuid is left out."""
    counts = {}
    for entry in entries:
        if "soulGuid" in entry:
            guids = counts.setdefault(entry["kind"], {})
            guids[entry["soulGuid"]] = guids.get(entry["soulGuid"], 0) + 1
    defaults = {}
    for kind, guids in sorted(counts.items()):
        defaults[kind] = min(guids, key=lambda guid: (-guids[guid], guid))
    return defaults


def render(catalog):
    """The catalog as JSON with the header indented and one entry on each line."""
    lines = []
    for entry in catalog["entries"]:
        lines.append("    " + json.dumps(entry, sort_keys=True, separators=(",", ":")))
    header = {key: value for key, value in catalog.items() if key != "entries"}
    text = json.dumps(header, indent=2, sort_keys=True)
    return text[: -len("\n}")] + ',\n  "entries": [\n' + ",\n".join(lines) + "\n  ]\n}\n"


def build(game, levels):
    data = Path(game) / "Data"
    souls = read_souls(data)
    prefab_defaults = read_prefab_defaults(data)
    entries = []
    for level in levels:
        entries.extend(level_entries(game, level, souls, prefab_defaults))
    entries.sort(key=sort_key)
    return {
        "levels": sorted(levels),
        "summary": summarize(entries),
        "defaultSoulGuids": default_soul_guids(entries),
        "entries": entries,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--game", default=DEFAULT_GAME, help="the game's installation folder")
    parser.add_argument("--levels", nargs="+", default=list(LEVELS), help="the levels to read")
    parser.add_argument("--output", type=Path, default=OUTPUT, help="the JSON file to write")
    args = parser.parse_args()
    catalog = build(args.game, args.levels)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(render(catalog), encoding="utf8", newline="\n")
    for level, summary in catalog["summary"].items():
        print(f"{level}: {summary['entries']}")
        print(f"  soul guids: {summary['soulGuids']}")
        for kind, counts in sorted(summary["kinds"].items()):
            print(f"  {kind}: {counts['entries']} entries, {counts['animals']} animals")
    for kind, guid in catalog["defaultSoulGuids"].items():
        print(f"default soul of {kind}: {guid}")


if __name__ == "__main__":
    main()
