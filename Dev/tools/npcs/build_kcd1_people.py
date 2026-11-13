#!/usr/bin/env python3
"""Builds a KCDS server's people for the KCD1 map (kcd1_rataje) from a KCD1 installation.

KCD1's level places its people as NPC and NPC_Female entities in objects_mission0.xml, each with its
"brain": where it stands and faces, its name, and its work (EntityLinks "Work[...]": guard, farmer,
merchant, housework, wanderer...). KCD1's souls are its own database's, so each person gets a body
and soul of KCD2's: a soul of the same work from KCD2's own placements (Npc.placements' catalog) when
one fits, else a guard, townsman or townswoman role (roles.json), the same one for a person on every
run. A quest's or a cutscene's people are left out.

    python Dev/tools/npcs/build_kcd1_people.py "<KCD1 folder>" "<server resources folder>"

writes <resources>/people-placements.json, level-places.json (KCD1's beds) and, when the server has
none, people-population.json (a 40 % share, see main): { "kcd1_rataje": [placement, ...] }, read by the server's
people module (npcs/people.cjs). It is made from the player's own KCD1 files and stays on that server.
"""

import argparse
import gzip
import hashlib
import json
import math
import re
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CATALOG = ROOT / "Backend/scripting/runtimes/js/npcs/placements.json.gz"
ROLES = ROOT / "Backend/scripting/runtimes/js/npcs/roles.json"
OUTFIT_CATALOG = ROOT / "Backend/scripting/runtimes/js/inventory/catalog.json"
# What a person of a work wears: one of the game's numbered outfit sets (<set>_01, _02...), for a
# man and for a woman. KCD2 dresses its own people by their level's schedules, which have none of
# KCD1's people, so the server dresses them (Npc.create outfit).
WORK_OUTFITS = {
    "guard": ("guard_raborsch", "f_villager"),
    "bailiff": ("guard_raborsch", "f_burger"),
    "bartender": ("innkeeper", "f_wench"),
    "pubvisitor": ("villager", "f_wench"),
    "farmer": ("labourer", "f_villager"),
    "collier": ("labourer", "f_villager"),
    "lumberjack": ("labourer", "f_villager"),
    "carpenter": ("labourer", "f_villager"),
    "tanner": ("labourer", "f_villager"),
    "gardener": ("labourer", "f_villager"),
    "merchant": ("burgher", "f_burger"),
    "miller": ("miller", "f_villager"),
    "beggar_work": ("beggar", "f_beggar"),
    "barber": ("barber", "f_bathmaid"),
    "scribe": ("scribe", "f_burger"),
    "horsehandler_work": ("stableman", "f_villager"),
    "bandit": ("bohutovaVlozka_bandit", "f_villager"),
    "cuman": ("cuman_civilian", "f_villager"),
}
DEFAULT_OUTFITS = ("villager", "f_villager")
LEVEL = "kcd1_rataje"
# Percent of KCD1's people who get bodies (people-population.json share).
KCD1_SHARE = 40
ENTITY = re.compile(
    r'<Entity [^>]*EntityClass="(NPC|NPC_Female)"[^>]*>.*?</Entity>', re.DOTALL
)
FIELD = re.compile(r'(\w+)="([^"]*)"')
WORK = re.compile(r"Work\[\(?'?([A-Za-z_]+)")
# A quest's, a cutscene's or a scene's people: KCD1 shows them only then.
STAGED = re.compile(r"^q_|quest|cutscene|_cs_|scene|dead|corpse|test", re.IGNORECASE)
# KCD1's works and the words KCD2's placements of that work have in their names or layers.
WORK_WORDS = {
    "guard": ("guard", "soldier", "watchman"),
    "bailiff": ("guard", "soldier"),
    "farmer": ("farmer", "peasant", "field"),
    "merchant": ("merchant", "trader", "shop"),
    "bartender": ("innkeeper", "tavern", "bartender", "pub"),
    "pubvisitor": ("tavern", "pub", "drunk"),
    "miller": ("miller", "mill"),
    "tanner": ("tanner",),
    "carpenter": ("carpenter",),
    "collier": ("charcoal", "collier"),
    "lumberjack": ("woodcutter", "lumberjack"),
    "barber": ("bath", "barber"),
    "scribe": ("scribe", "priest", "monk"),
    "beggar_work": ("beggar",),
    "horsehandler_work": ("stable", "groom", "horse"),
    "bandit": ("bandit",),
    "cuman": ("cuman",),
    "gardener": ("garden",),
}


def work_of(element, name):
    """A person's work: the first of its Work links KCD2 has people of, else one its name says,
    else its first Work link, else wandering."""
    works = WORK.findall(element)
    for work in works:
        if work.lower() in WORK_WORDS:
            return work.lower()
    lower = name.lower()
    for work, words in WORK_WORDS.items():
        if work.split("_")[0] in lower or any(word in lower for word in words):
            return work
    return works[0].lower() if works else "wanderer"


def read_entry(path, name):
    # KCD1's level pack spells the paths of some local headers with backslashes.
    with zipfile.ZipFile(path) as pak:
        info = next(
            i for i in pak.infolist() if i.filename.lower().replace("\\", "/") == name
        )
        info.orig_filename = info.filename
        try:
            return pak.read(info)
        except zipfile.BadZipFile:
            info.orig_filename = info.filename.replace("/", "\\")
            with pak.open(info) as entry:
                return entry.read()


def pick(candidates, key):
    digest = hashlib.sha1(key.encode()).digest()
    return candidates[int.from_bytes(digest[:4], "little") % len(candidates)]


def kcd2_people():
    """KCD2's placed people (both maps): (actorClass, soul, words of its name and layer)."""
    catalog = json.loads(gzip.open(CATALOG).read())
    people = []
    for rows in catalog.values():
        for row in rows:
            words = (row["name"] + " " + row["layer"]).lower()
            people.append((row["actorClass"], row["soul"], words))
    return people


def soul_for(work, actor_class, key, people, roles):
    if work == "bandit" and actor_class == "NPC":
        return pick([roles["bandit"], roles["bandit_alt"]], key)
    if work == "cuman" and actor_class == "NPC":
        return roles["cuman"]
    words = WORK_WORDS.get(work.lower(), ())
    fitting = [
        soul
        for cls, soul, text in people
        if cls == actor_class and any(w in text for w in words)
    ]
    if fitting:
        return pick(sorted(set(fitting)), key)
    if work.lower() in ("guard", "bailiff") and actor_class == "NPC":
        return pick([roles["guard"], roles["guard_alt"]], key)
    if actor_class == "NPC_Female":
        return pick([roles["townswoman"], roles["townswoman_alt"]], key)
    return pick([roles["townsman"], roles["townsman_alt"]], key)


def outfit_sets():
    """The game's numbered outfit sets: set name -> its outfits' names (two pieces or more)."""
    catalog = json.loads(OUTFIT_CATALOG.read_text(encoding="utf-8"))
    column = {name: index for index, name in enumerate(catalog["outfitColumns"])}
    sets = {}
    for row in catalog["outfits"]:
        name = row[column["name"]]
        match = re.fullmatch(r"(.+)_\d+", name)
        if match and len(row[column["items"]]) >= 2:
            sets.setdefault(match.group(1), []).append(name)
    return {key: sorted(names) for key, names in sets.items()}


def outfit_for(work, actor_class, key, sets):
    man, woman = WORK_OUTFITS.get(work.lower(), DEFAULT_OUTFITS)
    wanted = woman if actor_class == "NPC_Female" else man
    fallback = DEFAULT_OUTFITS[1] if actor_class == "NPC_Female" else DEFAULT_OUTFITS[0]
    names = sets.get(wanted) or sets.get(fallback) or []
    return pick(names, key + "/outfit") if names else None


def numbers(text, count, default):
    try:
        values = [float(v) for v in text.split(",")]
    except ValueError:
        return default
    return values if len(values) == count else default


ANY_ENTITY = re.compile(r"<Entity [^>]*>")
BED = re.compile(r'<Entity [^>]*EntityClass="Bed"[^>]*>.*?</Entity>', re.DOTALL)
WORK_TARGET = re.compile(r'<Link TargetId="(\d+)"[^>]*Name="[^"]*Work\[')
PREFERRED_BED = re.compile(r'<Link TargetId="(\d+)"[^>]*Name="preferedBed"')
# KCD1's bed helpers and KCD2's of the same kind (as the client's translator names them).
BED_HELPERS = {
    "bed": "Bed_1Place_High",
    "sleepingbench": "Bed_1Place_Bench",
    "sleepingbench_left": "Bed_1Place_Bench",
    "sleepingbench_right": "Bed_1Place_Bench",
    "shelterbed": "Bed_1Place_Tent",
    "tentbed": "Bed_1Place_Tent",
}
# Where a person sleeps at most from their own place, metres, when KCD1 names no bed of theirs.
BED_FARTHEST = 80
# How far from their own place a person walks to work at most, metres.
WORK_FARTHEST = 60
# How far up or down from their own place a work place may be (on their floor), metres.
WORK_FLOOR = 2
AREA = re.compile(
    r'<Entity [^>]*EntityClass="SmartAreaShape"[^>]*>.*?</Entity>', re.DOTALL
)
AREA_POINT = re.compile(r'<Point Pos="([^"]*)"')
NAME_WORDS = {
    "guard": "Guard",
    "bailiff": "Bailiff",
    "bartender": "Innkeeper",
    "miller": "Miller",
    "merchant": "Merchant",
    "farmer": "Farmer",
    "collier": "Charcoal burner",
    "lumberjack": "Woodcutter",
    "tanner": "Tanner",
    "carpenter": "Carpenter",
    "barber": "Barber",
    "scribe": "Scribe",
    "beggar_work": "Beggar",
    "horsehandler_work": "Stableman",
    "gardener": "Gardener",
    "bandit": "Bandit",
    "cuman": "Cuman",
}


def entity_positions(objects):
    """Every entity's position by its EntityId; an area's (a SmartAreaShape, where KCD1's people
    work) is the middle of its points, as its own position is only its first corner."""
    found = {}
    for tag in ANY_ENTITY.finditer(objects):
        head = dict(FIELD.findall(tag.group(0)))
        if "EntityId" in head and "Pos" in head:
            found[head["EntityId"]] = tuple(numbers(head["Pos"], 3, [0.0, 0.0, 0.0]))
    for match in AREA.finditer(objects):
        element = match.group(0)
        head = dict(FIELD.findall(element[: element.index(">")]))
        points = [numbers(p, 3, [0.0, 0.0, 0.0]) for p in AREA_POINT.findall(element)]
        if "EntityId" not in head or not points:
            continue
        x, y, z = numbers(head.get("Pos", ""), 3, [0.0, 0.0, 0.0])
        found[head["EntityId"]] = (
            x + sum(p[0] for p in points) / len(points),
            y + sum(p[1] for p in points) / len(points),
            z,
        )
    return found


def entity_guids(objects):
    """Every entity's GUID (lower case) by its EntityId."""
    found = {}
    for tag in ANY_ENTITY.finditer(objects):
        head = dict(FIELD.findall(tag.group(0)))
        if "EntityId" in head and "EntityGuid" in head:
            found[head["EntityId"]] = head["EntityGuid"].lower()
    return found


def bed_places(objects):
    """KCD1's beds as level places: [guid, helper class, x, y, z, qx, qy, qz, qw], the place a
    quarter turned (-90 degrees about z) from the bed, as KCD2's helpers lie across KCD1's.
    """
    places = []
    half = math.sqrt(0.5)
    for match in BED.finditer(objects):
        element = match.group(0)
        head = dict(FIELD.findall(element[: element.index(">")]))
        helpers = (
            dict(FIELD.findall(element)).get("soclasses_SmartObjectHelpers", "").lower()
        )
        helper = BED_HELPERS.get(helpers, "Bed_1Place_Low")
        x, y, z = numbers(head.get("Pos", ""), 3, [0.0, 0.0, 0.0])
        w, qx, qy, qz = numbers(head.get("Rotate", ""), 4, [1.0, 0.0, 0.0, 0.0])
        # (w, x, y, z) * (cos -45, 0, 0, sin -45)
        tw = w * half + qz * half
        tx = qx * half - qy * half
        ty = qy * half + qx * half
        tz = qz * half - w * half
        guid = head.get("EntityGuid", "").lower()
        places.append([f"kcd1-bed-{guid}", helper, x, y, z, tx, ty, tz, tw])
    return places


def nearest_bed(free_beds, x, y):
    best = None
    best_gap = BED_FARTHEST
    for guid, place in free_beds.items():
        gap = math.hypot(place[2] - x, place[3] - y)
        if gap < best_gap:
            best, best_gap = guid, gap
    return best


def readable_name(name, work):
    """What the HUD calls a person: their work, or KCD1's name without its place prefix."""
    if work in NAME_WORDS:
        return NAME_WORDS[work]
    parts = [part for part in name.split("_")[1:] if part and not part.isdigit()]
    words = re.sub(r"(?<=[a-z])(?=[A-Z])", " ", " ".join(parts))
    words = re.sub(r"\d+", "", words).strip()
    return words.title() if words else "Villager"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("kcd1", type=Path, help="the KCD1 installation folder")
    parser.add_argument("resources", type=Path, help="the server's resources folder")
    args = parser.parse_args()

    level_pack = args.kcd1 / "Data" / "Levels" / "rataje" / "level.pak"
    objects = read_entry(level_pack, "objects_mission0.xml").decode("utf-8", "replace")
    roles = {
        row["role"]: row["soul"]
        for row in json.loads(ROLES.read_text(encoding="utf-8"))
    }
    people = kcd2_people()
    sets = outfit_sets()
    positions = entity_positions(objects)
    beds = bed_places(objects)
    free_beds = {place[0]: place for place in beds}
    bed_of_entity = {place[0].removeprefix("kcd1-bed-"): place[0] for place in beds}
    id_to_guid = entity_guids(objects)

    rows = []
    left_out = 0
    for match in ENTITY.finditer(objects):
        element = match.group(0)
        head = dict(FIELD.findall(element[: element.index(">")]))
        name = head.get("Name", "")
        if STAGED.search(name):
            left_out += 1
            continue
        work = work_of(element, name)
        actor_class = match.group(1)
        guid = head.get("EntityGuid", name).lower()
        x, y, z = numbers(head.get("Pos", ""), 3, [0.0, 0.0, 0.0])
        w, rx, ry, rz = numbers(head.get("Rotate", ""), 4, [1.0, 0.0, 0.0, 0.0])
        row = {
            "guid": f"kcd1-{guid}",
            "name": readable_name(name, work),
            "interactable": True,
            "soul": soul_for(work, actor_class, guid, people, roles),
            "actorClass": actor_class,
            "layer": f"kcd1/{work}",
            "position": {"x": x, "y": y, "z": z},
            "rotation": {"w": w, "x": rx, "y": ry, "z": rz},
        }
        outfit = outfit_for(work, actor_class, guid, sets)
        if outfit:
            row["outfit"] = outfit
        # Their work place: where KCD1's Work link points.
        for target in WORK_TARGET.findall(element):
            if target in positions:
                tx, ty, tz = positions[target]
                same_floor = abs(tz - z) <= WORK_FLOOR
                if math.hypot(tx - x, ty - y) <= WORK_FARTHEST and same_floor:
                    row["work"] = {"x": tx, "y": ty, "z": tz}
                break
        # Their bed: KCD1's preferred one, else the nearest free one.
        preferred = PREFERRED_BED.search(element)
        bed = None
        if preferred:
            bed = bed_of_entity.get(id_to_guid.get(preferred.group(1), ""))
        if bed is None or bed not in free_beds:
            bed = nearest_bed(free_beds, x, y)
        if bed is not None:
            row["bed"] = bed
            free_beds.pop(bed, None)
        rows.append(row)

    args.resources.mkdir(parents=True, exist_ok=True)
    out = args.resources / "people-placements.json"
    out.write_text(json.dumps({LEVEL: rows}), encoding="utf-8")
    # How many of them get bodies: KCD1's town navmesh stacks several floors per tile, and with all of
    # its people the game served the paths of those nearest a player only (about 10 of 45 walked,
    # none beyond 25 m); with 40 % most of them walk, far ones too. A server's own file is kept.
    population = args.resources / "people-population.json"
    if not population.exists():
        population.write_text(json.dumps({"share": KCD1_SHARE}), encoding="utf-8")
    places = args.resources / "level-places.json"
    places.write_text(json.dumps({LEVEL: beds}), encoding="utf-8")
    with_bed = sum(1 for row in rows if "bed" in row)
    with_work = sum(1 for row in rows if "work" in row)
    print(
        f"{out}: {len(rows)} people ({with_work} with a work place, {with_bed} with a bed), "
        f"{left_out} of quests and scenes left out; {places}: {len(beds)} beds"
    )


if __name__ == "__main__":
    main()
