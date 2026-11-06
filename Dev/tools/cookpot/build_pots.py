"""Builds the cooking pot catalog from a game installation.

The catalog is what CookPot answers: every stew pot placed in the game's levels, the meals a pot
serves and the poisons a pot takes.

- pots: each KettleActionTrigger in a level's objects (objects_mission0.xml) and layers
  (layers/*.xml) of Data/Levels/<level>/level.pak names its fireplace by the GUID in brackets
  ("KettleActionTrigger1[Fireplace/fireplace_home5_29410bf0-...]"), the same GUID the player's
  client reports a pot meal by. A pot is that fireplace; its position is its first trigger's.
  The trigger links the fireplace's FireplaceSmartObject: its meal (Fireplace
  esFireplaceCauldronType, the script's goulash when not set), whether it starts full (bFullOnInit,
  full when not set) and its cauldron (the RuntimePrefab it links as proceduralObject['cauldron'],
  by its level GUID as sixteen lowercase hex digits; a cauldron_camp prefab is a camp pot, else a
  home pot) and the FireplaceSmartObject's own level GUID (fireplace, its kettle behaviour's
  entity). The game shows a pot's fill as that cauldron's prefab phase.
- foods: the game's three pot meals (Libs/Tables/item/item*.xml, kettleFood_*Mash) by the name
  CookPot uses, with their item class and nutrition.
- poisons: every Poison item class and the buff it gives when ingested (BuffDefinitionId).

    python Dev/tools/cookpot/build_pots.py "<game folder>"

writes Backend/scripting/runtimes/js/cookpot/pots.json.
"""

import argparse
import json
import re
import xml.etree.ElementTree as ElementTree
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/cookpot/pots.json"
LEVEL_FILES = re.compile(r"^(objects_mission0\.xml|layers/[^/]+\.xml)$", re.IGNORECASE)
TRIGGER_NAME = re.compile(
    r"^KettleActionTrigger\d*\[Fireplace/[^\]]*?"
    r"([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})\]$",
    re.IGNORECASE,
)
CAULDRON_LINK = "proceduralObject['cauldron']"
# The game's pot meals by the fireplace's esFireplaceCauldronType (FireplaceSmartObject.lua).
LEVEL_MEALS = ("goulash", "lentil", "soup")
ITEM_TABLES = re.compile(r"^Libs/Tables/item/item(__[^/]+)?\.xml$", re.IGNORECASE)
# The pot meals as CookPot names them, by the game's item name.
FOODS = {
    "goulash": "kettleFood_richmadeMash",
    "soup": "kettleFood_homemadeMash",
    "lentil": "kettleFood_campmadeMash",
}


def numbers(text, count):
    values = [float(part) for part in text.split(",")]
    if len(values) != count:
        raise ValueError(f"expected {count} numbers in {text!r}")
    return values


def read_entry(pak, name):
    # Some entries' local headers spell the path with backslashes.
    try:
        return pak.read(name)
    except zipfile.BadZipFile:
        info = pak.getinfo(name)
        info.orig_filename = name.replace("/", "\\")
        with pak.open(info) as entry:
            return entry.read()


def fireplace_of(trigger, by_id):
    """The FireplaceSmartObject a pot's trigger links, or None."""
    for link in trigger.iter("Link"):
        target = by_id.get(link.get("TargetId"))
        if target is not None and target.get("EntityClass") == "FireplaceSmartObject":
            return target
    return None


def look_of(fireplace, by_id):
    """A pot's meal, start and cauldron from its FireplaceSmartObject, or {} when not found."""
    if fireplace is None:
        return {}
    cauldron = None
    for link in fireplace.iter("Link"):
        if link.get("Name") == CAULDRON_LINK:
            cauldron = by_id.get(link.get("TargetId"))
    if cauldron is None or not cauldron.get("EntityGuid"):
        return {}
    settings = fireplace.find("Properties/Fireplace")
    meal = settings.get("esFireplaceCauldronType") if settings is not None else None
    full = settings.get("bFullOnInit") if settings is not None else None
    return {
        "cauldron": cauldron.get("EntityGuid").replace("-", "").lower(),
        "fireplace": (fireplace.get("EntityGuid") or "").replace("-", "").lower(),
        "camp": "cauldron_camp" in cauldron.get("Name", ""),
        "meal": meal if meal in LEVEL_MEALS else "goulash",
        "full": full != "0",
    }


def pots_of(level, pak):
    found = {}
    for name in sorted(pak.namelist()):
        if not LEVEL_FILES.match(name):
            continue
        text = read_entry(pak, name).decode("utf-8", errors="replace")
        if "KettleActionTrigger" not in text:
            continue
        tree = ElementTree.fromstring(text)
        by_id = {entity.get("EntityId"): entity for entity in tree.iter("Entity")}
        for entity in tree.iter("Entity"):
            if entity.get("EntityClass") != "KettleActionTrigger" or not entity.get("Pos"):
                continue
            match = TRIGGER_NAME.match(entity.get("Name", ""))
            if not match:
                continue
            guid = match.group(1).lower()
            if guid in found:
                continue
            x, y, z = numbers(entity.get("Pos"), 3)
            found[guid] = {
                "guid": guid,
                "level": level,
                "position": {"x": round(x, 3), "y": round(y, 3), "z": round(z, 3)},
                **look_of(fireplace_of(entity, by_id), by_id),
            }
    return list(found.values())


def item_elements(game):
    with zipfile.ZipFile(game / "Data" / "Tables.pak") as pak:
        for name in sorted(pak.namelist()):
            if ITEM_TABLES.match(name):
                yield from ElementTree.fromstring(pak.read(name)).iter()


def items_of(game):
    foods = {}
    poisons = {}
    for element in item_elements(game):
        name = element.get("Name", "")
        item = (element.get("Id") or "").lower()
        if element.tag == "Food":
            for food, game_name in FOODS.items():
                if name == game_name:
                    foods[food] = {
                        "item": item,
                        "nutrition": int(float(element.get("NutritionBenefit", "0"))),
                    }
        elif element.tag == "Poison" and element.get("BuffDefinitionId"):
            poisons[item] = element.get("BuffDefinitionId").lower()
    missing = sorted(set(FOODS) - set(foods))
    if missing:
        raise SystemExit(f"pot meals not found: {missing}")
    return foods, poisons


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("game", type=Path, help="the game's installation folder")
    game = parser.parse_args().game
    pots = []
    for level_pak in sorted((game / "Data/Levels").glob("*/level.pak")):
        with zipfile.ZipFile(level_pak) as pak:
            pots.extend(pots_of(level_pak.parent.name, pak))
    pots.sort(key=lambda pot: (pot["level"], pot["guid"]))
    foods, poisons = items_of(game)
    lines = ["{"]
    lines.append('  "foods": ' + json.dumps(foods, sort_keys=True) + ",")
    lines.append('  "poisons": ' + json.dumps(poisons, sort_keys=True) + ",")
    lines.append('  "pots": [')
    lines.append(",\n".join("    " + json.dumps(pot, sort_keys=True) for pot in pots))
    lines.append("  ]")
    lines.append("}")
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(pots)} pots, {len(foods)} meals, {len(poisons)} poisons")


if __name__ == "__main__":
    main()
