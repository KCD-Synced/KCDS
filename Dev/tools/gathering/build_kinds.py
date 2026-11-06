"""Builds the herb catalog from a game installation.

Every kind of pickable plant the game knows: Libs/Tables/item/pickable_area_desc.xml in
Data/Tables.pak, one row per pickable area id (the plant's kind) with the herb it gives
(guid_item_picked) and the game's own regrowth time in game hours (respawn_time).

    python Dev/tools/gathering/build_kinds.py "<game folder>"

writes Backend/scripting/runtimes/js/gathering/kinds.json.
"""

import argparse
import json
import xml.etree.ElementTree as ElementTree
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/gathering/kinds.json"
TABLE = "Libs/Tables/item/pickable_area_desc.xml"


def kinds_of(game):
    with zipfile.ZipFile(game / "Data/Tables.pak") as pak:
        text = pak.read(TABLE).decode("utf-8", errors="replace")
    kinds = []
    for row in ElementTree.fromstring(text).iter("pickable_area_desc"):
        kinds.append(
            {
                "id": int(row.get("id")),
                "name": row.get("name"),
                "item": row.get("guid_item_picked").lower(),
                "respawnHours": float(row.get("respawn_time")),
            }
        )
    kinds.sort(key=lambda kind: kind["id"])
    return kinds


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("game", type=Path, help="the game's installation folder")
    kinds = kinds_of(parser.parse_args().game)
    lines = ["["]
    lines.append(",\n".join("  " + json.dumps(kind, sort_keys=True) for kind in kinds))
    lines.append("]")
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(kinds)} herb kinds")


if __name__ == "__main__":
    main()
