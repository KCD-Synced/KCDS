"""Builds the dice table catalog from a game installation.

The catalog is what Dice.tables answers: every dice table placed in the game's levels.

- A table is a prefab instance, "Table/table_dice<N>_<GUID>" in the brackets of its entities'
  names, in a level's objects (objects_mission0.xml) and layers (layers/*.xml) of
  Data/Levels/<level>/level.pak. Its id is its DiceInteractor entity's level GUID as sixteen
  lowercase hex digits; its seats are its two SmartObjectHolder entities seat1_SO and seat2_SO (the
  native seat order), by their level GUIDs; its position is the midpoint between them, and its
  chairs are where those two stand.
- Its behavior is the SmartObjectHolder the interactor links to (dice_behaviors), by its level
  GUID: the entity the game's dice minigame is started at (Minigame.StartDice's table).

    python Dev/tools/dice/build_tables.py "<game folder>"

writes Backend/scripting/runtimes/js/dice/tables.json.
"""

import argparse
import json
import re
import xml.etree.ElementTree as ElementTree
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/dice/tables.json"
LEVEL_FILES = re.compile(r"^(objects_mission0\.xml|layers/[^/]+\.xml)$", re.IGNORECASE)
# The prefab instance an entity belongs to, from the brackets of its name.
INSTANCE = re.compile(r"\[Table/(table_dice[^\]]*)\]$", re.IGNORECASE)
SEATS = ("seat1_SO", "seat2_SO")


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


def level_guid(entity):
    return (entity.get("EntityGuid") or "").replace("-", "").lower()


def tables_of(level, pak):
    # Prefab instance -> { interactor, seats by name }
    instances = {}
    for name in sorted(pak.namelist()):
        if not LEVEL_FILES.match(name):
            continue
        text = read_entry(pak, name).decode("utf-8", errors="replace")
        if "DiceInteractor" not in text and "seat1_SO" not in text:
            continue
        for entity in ElementTree.fromstring(text).iter("Entity"):
            match = INSTANCE.search(entity.get("Name", ""))
            if not match:
                continue
            instance = instances.setdefault(match.group(1).lower(), {"seats": {}})
            if entity.get("EntityClass") == "DiceInteractor":
                instance["interactor"] = level_guid(entity)
                link = entity.find("EntityLinks/Link")
                if link is not None and link.get("TargetGuid"):
                    instance["behavior"] = link.get("TargetGuid").replace("-", "").lower()
            for seat in SEATS:
                if entity.get("Name", "").startswith(seat + "[") and entity.get("Pos"):
                    instance["seats"][seat] = (level_guid(entity), numbers(entity.get("Pos"), 3))
    tables = []
    for instance in instances.values():
        if (
            "interactor" not in instance
            or "behavior" not in instance
            or len(instance["seats"]) != len(SEATS)
        ):
            continue
        (first, first_at), (second, second_at) = (instance["seats"][seat] for seat in SEATS)
        middle = [(a + b) / 2 for a, b in zip(first_at, second_at)]
        tables.append(
            {
                "id": instance["interactor"],
                "behavior": instance["behavior"],
                "level": level,
                "position": {
                    "x": round(middle[0], 3),
                    "y": round(middle[1], 3),
                    "z": round(middle[2], 3),
                },
                "seats": [first, second],
                "chairs": [
                    {"x": round(at[0], 3), "y": round(at[1], 3), "z": round(at[2], 3)}
                    for at in (first_at, second_at)
                ],
            }
        )
    return tables


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("game", type=Path, help="the game's installation folder")
    game = parser.parse_args().game
    tables = []
    for level_pak in sorted((game / "Data/Levels").glob("*/level.pak")):
        with zipfile.ZipFile(level_pak) as pak:
            tables.extend(tables_of(level_pak.parent.name, pak))
    tables.sort(key=lambda table: (table["level"], table["id"]))
    lines = ["["]
    lines.append(",\n".join("  " + json.dumps(table, sort_keys=True) for table in tables))
    lines.append("]")
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(tables)} dice tables")


if __name__ == "__main__":
    main()
