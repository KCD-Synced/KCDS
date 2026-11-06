"""Builds the dice badges from a game installation.

The badges are what Dice.badges lists and what Dice.start's firstBadge and secondBadge take: every
badge the game ships as an item of class DiceBadge, with its effect and its tier (the server plays
the effect; both players' games show the badge and play it with the game's own badge rules).

- Libs/Tables/item/item.xml and item__dlc.xml (Tables.pak): each <DiceBadge> element. A badge's
  id is the item class GUID, its name the item's Name ("HeadStartDiceBadgePlumb"); its effect is
  SubType (dice_badge_subtype.xml: 0 Headstart to 12 Balatro), its tier Type (dice_badge_type.xml:
  0 tin, 1 silver, 2 gold); its mesh is Model under Objects/, with .cgf added when the table leaves
  it out.

    python Dev/tools/dice/build_badges.py "<game folder>"

writes Backend/scripting/runtimes/js/dice/badges.json.
"""

import argparse
import json
import xml.etree.ElementTree as ElementTree
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/dice/badges.json"
TABLES = {
    "Libs/Tables/item/item.xml": False,
    "Libs/Tables/item/item__dlc.xml": True,
}
# The game's badge effects (dice_badge_subtype.xml), by SubType.
EFFECTS = [
    "headStart",
    "formations",
    "null",
    "extraValue",
    "antiBust",
    "doubleTake",
    "multiplier",
    "extraDice",
    "rerollDice",
    "setDice",
    "rerollPips",
    "boostSixes",
    "balatro",
]
# The game's badge tiers (dice_badge_type.xml), by Type.
TIERS = ["tin", "silver", "gold"]


def mesh_of(model):
    path = "objects/" + model.replace("\\", "/").lower()
    if not path.endswith(".cgf"):
        path += ".cgf"
    return path


def badges_of(pak, table, dlc):
    badges = []
    for badge in ElementTree.fromstring(pak.read(table)).iter("DiceBadge"):
        effect = int(badge.get("SubType", "-1"))
        tier = int(badge.get("Type", "-1"))
        if effect < 0 or effect >= len(EFFECTS) or tier < 0 or tier >= len(TIERS):
            raise ValueError(f"{badge.get('Name')}: effect {effect}, tier {tier}")
        badges.append(
            {
                "id": badge.get("Id").lower(),
                "name": badge.get("Name").strip(),
                "effect": EFFECTS[effect],
                "tier": TIERS[tier],
                "model": mesh_of(badge.get("Model")),
                "dlc": dlc,
            }
        )
    return badges


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game", type=Path)
    options = parser.parse_args()
    with zipfile.ZipFile(options.game / "Data" / "Tables.pak") as pak:
        badges = []
        for table, dlc in TABLES.items():
            badges.extend(badges_of(pak, table, dlc))
    badges.sort(key=lambda badge: badge["name"].lower())
    # One badge a line, as the dice kinds.
    lines = ",\n".join("  " + json.dumps(badge) for badge in badges)
    OUTPUT.write_text("[\n" + lines + "\n]\n", encoding="utf-8", newline="\n")
    print(f"{len(badges)} dice badges written to {OUTPUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
