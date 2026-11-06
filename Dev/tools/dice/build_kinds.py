"""Builds the dice kinds from a game installation.

The kinds are what Dice.kinds lists and what Dice.start's firstDice and secondDice take: every
die the game ships as an item of class Die, with the weight of each of its six sides (the server
deals a throw by them) and the mesh the players' games show it with.

- Libs/Tables/item/item.xml and item__dlc.xml (Tables.pak): each <Die> element. A kind's id is
  the item class GUID, its name the item's Name ("dieLucky"); its sides are SideValues (0 to 5
  the faces 1 to 6; 6 the game's joker face, on dieDevil, dieBalatro and die_Tengri) with their
  SideWeights; its mesh is Model under Objects/, with .cgf added when the table leaves it out.

    python Dev/tools/dice/build_kinds.py "<game folder>"

writes Backend/scripting/runtimes/js/dice/kinds.json.
"""

import argparse
import json
import xml.etree.ElementTree as ElementTree
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/dice/kinds.json"
TABLES = {
    "Libs/Tables/item/item.xml": False,
    "Libs/Tables/item/item__dlc.xml": True,
}
# A side's value: 0 to 5 the faces 1 to 6, 6 the joker.
JOKER = 6


def mesh_of(model):
    path = "objects/" + model.replace("\\", "/").lower()
    if not path.endswith(".cgf"):
        path += ".cgf"
    return path


def kinds_of(pak, table, dlc):
    kinds = []
    for die in ElementTree.fromstring(pak.read(table)).iter("Die"):
        sides = [int(value) for value in die.get("SideValues", "").split()]
        if len(sides) != 6 or min(sides) < 0 or max(sides) > JOKER:
            raise ValueError(f"{die.get('Name')}: sides {sides}")
        weights = [int(weight) for weight in die.get("SideWeights", "").split()]
        if len(weights) != 6 or min(weights) < 0 or sum(weights) <= 0:
            raise ValueError(f"{die.get('Name')}: weights {weights}")
        kinds.append(
            {
                "id": die.get("Id").lower(),
                "name": die.get("Name").strip(),
                "model": mesh_of(die.get("Model")),
                "sides": sides,
                "weights": weights,
                "dlc": dlc,
            }
        )
    return kinds


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game", type=Path)
    options = parser.parse_args()
    with zipfile.ZipFile(options.game / "Data" / "Tables.pak") as pak:
        kinds = []
        for table, dlc in TABLES.items():
            kinds.extend(kinds_of(pak, table, dlc))
    kinds.sort(key=lambda kind: kind["name"].lower())
    # One kind a line, as the recipe catalog.
    lines = ",\n".join("  " + json.dumps(kind) for kind in kinds)
    OUTPUT.write_text("[\n" + lines + "\n]\n", encoding="utf-8", newline="\n")
    print(f"{len(kinds)} dice kinds written to {OUTPUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
