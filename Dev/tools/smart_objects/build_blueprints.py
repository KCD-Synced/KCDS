"""Builds the catalog of the prefabs SmartObject.spawn places from a game installation.

Every prefab of the game (Prefabs/*.xml in Data/IPL_GameData.pak) is a runtime template the game
builds whole at an anchor entity (Game.SpawnPrefab, PrefabTemplateLibrary): its furniture, its
solids and the entities its interaction runs on. A row is one a player can use, by what it holds:

  dice      a DiceInteractor (the dice minigame's table)
  alchemy   an AlchemyTable
  smithing  a Smithery
  drying    a FoodProcessingTrigger of foodProcessingType:drying
  smoking   a FoodProcessingTrigger of foodProcessingType:smoking
  bed       otherwise, smart objects of a Bed_ helper class (soclass_SmartObjectHelpers)
  seat      otherwise, smart objects of a Sit_ helper class

and keeps its name (the path under Prefabs/ without .xml), its template Id (a GUID), the places in
the game's build of the smart objects a body sits or lies on (their number is how many bodies it
takes at once) and the place of its station entity (null for a seat or a bed). A client gives the
entity at place p of smart object id the EntityGuid smart_object_guid(id, p)
(Client/runtime/smart_objects/placed_smart_objects.hpp), so the server names them the same.

Its last field is where the station stands in the prefab, for the server's station catalogs (null
for a seat or a bed): { at: [x, y, z] } of the station entity, in the prefab's own frame (each
object's Pos through its Parent's Pos and Rotate, w x y z). A dice table also has its behavior
place (the SmartObjectHolder of AI category diceTable, the entity the dice minigame starts at),
its seats in the game's order (seat1_SO, seat2_SO), and `at` is their midpoint, as
Dev/tools/dice/build_tables.py places the level's tables.

    python Dev/tools/smart_objects/build_blueprints.py "<game folder>"

writes Backend/scripting/runtimes/js/smart_objects/blueprints.json: one row a line,
[name, templateId, kind, seatPlaces, stationPlace, station, corrections].

`corrections` are the built entities that have a brush among their parents, which the game's
runtime build would place by their own Pos only: [place, [x, y, z], [x, y, z, w]] each, where they
stand in the prefab's frame and their turn.
"""

import argparse
import json
import xml.etree.ElementTree as ElementTree
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/smart_objects/blueprints.json"
STATION_CLASSES = {"DiceInteractor": "dice", "AlchemyTable": "alchemy", "Smithery": "smithing"}
FOOD_KINDS = {"foodProcessingType:drying": "drying", "foodProcessingType:smoking": "smoking"}
DICE_SEATS = ("seat1_SO", "seat2_SO")


def built(objects):
    """The objects the game builds as entities, in the order it builds them: the template's
    entities and tag points as listed, without its PrefabPorts (a bench's 8, the
    dice table's 28)."""
    return [
        item
        for item in objects
        if item.get("Type") in ("Entity", "TagPoint") and item.get("EntityClass") != "PrefabPort"
    ]


def numbers(text, count, default):
    if not text:
        return list(default)
    values = [float(part) for part in text.split(",")]
    return values if len(values) == count else list(default)


def rotate(q, v):
    """v turned by the quaternion q (w, x, y, z)."""
    w, x, y, z = q
    tx = 2 * (y * v[2] - z * v[1])
    ty = 2 * (z * v[0] - x * v[2])
    tz = 2 * (x * v[1] - y * v[0])
    return [
        v[0] + w * tx + (y * tz - z * ty),
        v[1] + w * ty + (z * tx - x * tz),
        v[2] + w * tz + (x * ty - y * tx),
    ]


def multiply(a, b):
    aw, ax, ay, az = a
    bw, bx, by, bz = b
    return [
        aw * bw - ax * bx - ay * by - az * bz,
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
    ]


def frame_of(item, by_id, depth=0):
    """Where an object stands in the prefab's frame, and its turn: its Pos and Rotate through its
    parents'."""
    position = numbers(item.get("Pos"), 3, (0.0, 0.0, 0.0))
    turn = numbers(item.get("Rotate"), 4, (1.0, 0.0, 0.0, 0.0))
    parent = by_id.get(item.get("Parent", ""))
    if parent is None or depth > 16:
        return position, turn
    parent_at, parent_turn = frame_of(parent, by_id, depth + 1)
    at = rotate(parent_turn, position)
    return [parent_at[i] + at[i] for i in range(3)], multiply(parent_turn, turn)


def rounded(values):
    return [round(value, 4) for value in values]


def brush_parented(item, by_id):
    """Whether a brush is among an object's parents: the game's build of a prefab at runtime places
    such an entity by its own Pos only (the dice table's dice_behaviors stood on
    the ground beside the table), as only entities are parents there."""
    parent = by_id.get(item.get("Parent", ""))
    for _ in range(16):
        if parent is None:
            return False
        if parent.get("Type") == "Brush":
            return True
        parent = by_id.get(parent.get("Parent", ""))
    return False


def corrections_of(objects):
    """[place, [x, y, z], [x, y, z, w]] of each built entity with a brush among its parents: where
    it stands in the prefab's frame and its turn, which the client gives it as it is built."""
    by_id = {item.get("Id", ""): item for item in objects}
    rows = []
    for place, item in enumerate(built(objects)):
        if brush_parented(item, by_id):
            at, (w, x, y, z) = frame_of(item, by_id)
            rows.append([place, rounded(at), rounded([x, y, z, w])])
    return rows


def kind_of(objects):
    """What a prefab is for, the places of its seats (or beds) and of its station entity in the
    build, and where its station stands; or None."""
    by_id = {item.get("Id", ""): item for item in objects}
    seats = {"Bed": [], "Seat": []}
    station = None
    station_place = None
    station_item = None
    behavior_place = None
    named = {}
    for place, item in enumerate(built(objects)):
        entity_class = item.get("EntityClass", "")
        properties = item.find("Properties")
        kind = STATION_CLASSES.get(entity_class)
        if entity_class == "FoodProcessingTrigger":
            script = item.find("Properties/Script")
            kind = FOOD_KINDS.get(script.get("Misc", "") if script is not None else "")
        if kind and not station:
            station, station_place, station_item = kind, place, item
        category = properties.get("sWH_AI_EntityCategory", "") if properties is not None else ""
        if entity_class == "SmartObjectHolder" and category == "diceTable":
            behavior_place = place
        named[item.get("Name", "")] = (place, item)
        # A place a body sits or lies on: a smart object of a Sit_ or Bed_ helper class (the dice
        # table's seats are SmartObjectHolders, a bench's StanceSmartObjects).
        helpers = properties.get("soclass_SmartObjectHelpers", "") if properties is not None else ""
        if "SmartObject" in entity_class and helpers.startswith("Sit_"):
            seats["Seat"].append(place)
        elif "SmartObject" in entity_class and helpers.startswith("Bed_"):
            seats["Bed"].append(place)
    places = sorted(seats["Bed"] + seats["Seat"])
    if station == "dice":
        if behavior_place is None or not all(name in named for name in DICE_SEATS):
            return None
        chairs = [named[name] for name in DICE_SEATS]
        ends = [frame_of(item, by_id)[0] for _, item in chairs]
        middle = [(ends[0][i] + ends[1][i]) / 2 for i in range(3)]
        about = {"at": rounded(middle), "behavior": behavior_place}
        return station, [place for place, _ in chairs], station_place, about
    if station:
        return station, places, station_place, {"at": rounded(frame_of(station_item, by_id)[0])}
    if seats["Bed"]:
        return "bed", places, None, None
    if seats["Seat"]:
        return "seat", places, None, None
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("game", type=Path, help="the game's installation folder")
    game = parser.parse_args().game
    rows = []
    with zipfile.ZipFile(game / "Data/IPL_GameData.pak") as pak:
        for name in sorted(pak.namelist()):
            if not name.startswith("Prefabs/") or not name.endswith(".xml"):
                continue
            root = ElementTree.fromstring(pak.read(name).decode("utf-8", errors="replace"))
            template = root.get("Id", "").lower()
            if root.tag != "Prefab" or len(template) != 36:
                continue
            found = kind_of(list(root.iter("Object")))
            if found:
                kind, seats, station, about = found
                corrections = corrections_of(list(root.iter("Object")))
                rows.append(
                    [
                        name[len("Prefabs/") : -len(".xml")],
                        template,
                        kind,
                        seats,
                        station,
                        about,
                        corrections,
                    ]
                )
    lines = ["["]
    lines.append(",\n".join("  " + json.dumps(row) for row in rows))
    lines.append("]")
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"{OUTPUT}: {len(rows)} blueprints")


if __name__ == "__main__":
    main()
