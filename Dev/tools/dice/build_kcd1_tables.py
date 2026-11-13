#!/usr/bin/env python3
"""Builds a KCDS server's dice tables for the KCD1 map (kcd1_rataje) from a KCD1 installation.

The game client puts one of KCD2's dice tables (klaster's, Client/runtime/kcd1/dice_tables.hpp) on
each of KCD1's: turned and moved so that its two seats stand where KCD1's two seats do, each entity
with the GUID FNV-1a 64 of "<instance>/<template GUID>", where the instance is the GUID in KCD1's
prefab names (dice_cup_<instance>, seat1_<instance>, seat2_<instance>). This tool names the same
tables in the dice catalog's form (dice/tables.json, see build_tables.py): the interactor's and the
behavior's GUIDs, the two seats' GUIDs and where they stand.

    python Dev/tools/dice/build_kcd1_tables.py "<KCD1 folder>" "<server resources folder>"

writes <resources>/dice-tables.json, read by the server's dice module (dice/dice.cjs). It is made
from the player's own KCD1 files and stays on that server.
"""

import argparse
import json
import math
import re
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CATALOG = ROOT / "Backend/scripting/runtimes/js/dice/tables.json"
LEVEL = "kcd1_rataje"
# The KCD2 table every KCD1 table is made of: its DiceInteractor (the catalog's id).
TEMPLATE = "0679658da25d0a80"
ENTITY = re.compile(r"<Entity\s[^>]*>")
FIELD = re.compile(r'(\w+)="([^"]*)"')
CUP = "dice_cup_"


def table_guid(instance, template_guid):
    """FNV-1a 64 of "<instance>/<template GUID>" as sixteen hex digits (dice_tables.hpp)."""
    value = 0xCBF29CE484222325
    text = instance.lower() + "/" + template_guid.replace("-", "").lower()
    for byte in text.encode("ascii"):
        value ^= byte
        value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"{value:016x}"


def read_entry(path, name):
    with zipfile.ZipFile(path) as pak:
        for info in pak.infolist():
            if info.filename.replace("\\", "/").lower() == name:
                try:
                    return pak.read(info)
                except zipfile.BadZipFile:
                    info.orig_filename = info.filename.replace("/", "\\")
                    with pak.open(info) as entry:
                        return entry.read()
    raise FileNotFoundError(f"{name} is not in {path}")


def position(head):
    values = [float(part) for part in head.get("Pos", "").split(",") if part]
    return values if len(values) == 3 else None


def kcd1_tables(objects):
    """{ instance: (seat1, seat2) } of every KCD1 table with a cup and both seats."""
    heads = [dict(FIELD.findall(match.group(0))) for match in ENTITY.finditer(objects)]
    instances = {
        head["Name"][len(CUP) :].lower()
        for head in heads
        if head.get("EntityClass") == "DiceMinigameCup" and head.get("Name", "").startswith(CUP)
    }
    seats = {}
    for head in heads:
        if head.get("EntityClass") != "SmartObjectHolder":
            continue
        name = head.get("Name", "").lower()
        for number in (1, 2):
            prefix = f"seat{number}_"
            if name.startswith(prefix) and name[len(prefix) :] in instances:
                at = position(head)
                if at:
                    seats.setdefault(name[len(prefix) :], {})[number] = at
    return {
        instance: (found[1], found[2])
        for instance, found in seats.items()
        if 1 in found and 2 in found
    }


def heading(first, second):
    return math.atan2(second[1] - first[1], second[0] - first[0])


def placed(template, instance, seats):
    """The catalog row of KCD1 table `instance`, made from the template table's row."""
    first, second = seats
    from_first = [template["chairs"][0][axis] for axis in "xyz"]
    from_second = [template["chairs"][1][axis] for axis in "xyz"]
    from_middle = [(a + b) / 2 for a, b in zip(from_first, from_second)]
    to_middle = [(a + b) / 2 for a, b in zip(first, second)]
    angle = math.remainder(heading(first, second) - heading(from_first, from_second), 2 * math.pi)
    c = math.cos(angle)
    s = math.sin(angle)

    def moved(at):
        dx = at[0] - from_middle[0]
        dy = at[1] - from_middle[1]
        return {
            "x": round(to_middle[0] + c * dx - s * dy, 3),
            "y": round(to_middle[1] + s * dx + c * dy, 3),
            "z": round(to_middle[2] + at[2] - from_middle[2], 3),
        }

    return {
        "id": table_guid(instance, template["id"]),
        "behavior": table_guid(instance, template["behavior"]),
        "level": LEVEL,
        "position": {axis: round(value, 3) for axis, value in zip("xyz", to_middle)},
        "seats": [table_guid(instance, seat) for seat in template["seats"]],
        "chairs": [moved(from_first), moved(from_second)],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("kcd1", type=Path, help="the KCD1 installation folder")
    parser.add_argument("resources", type=Path, help="the server's resources folder")
    args = parser.parse_args()

    template = next(
        row for row in json.loads(CATALOG.read_text(encoding="utf-8")) if row["id"] == TEMPLATE
    )
    level_pack = args.kcd1 / "Data" / "Levels" / "rataje" / "level.pak"
    objects = read_entry(level_pack, "objects_mission0.xml").decode("utf-8", "replace")
    tables = [
        placed(template, instance, seats)
        for instance, seats in sorted(kcd1_tables(objects).items())
    ]
    lines = ["["]
    lines.append(",\n".join("  " + json.dumps(table, sort_keys=True) for table in tables))
    lines.append("]")
    output = args.resources / "dice-tables.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(tables)} dice tables -> {output}")


if __name__ == "__main__":
    main()
