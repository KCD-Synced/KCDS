"""Builds the server's horse-look catalog from a game installation.

A horse's body is built from character components as a person's is (Libs/Tables/Character/
CharacterComponent.xml in Data/Tables.pak, the subtree of race Horse): a body (the coat), a head
(the breed: work, travel, war, draft, noble or female), hair (the mane) and a beard (the tail).
The catalog lists every name of each part, and the common ones a horse a resource did not
describe is given (the solid, dappled, grulla and marked coats; the plain manes and tails), so
the server chooses one look and every client builds the horse the same.

    python Dev/tools/horses/build_looks.py "<game folder>"

writes Backend/scripting/runtimes/js/horses/looks.json.
"""

import argparse
import json
import xml.etree.ElementTree as ElementTree
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/horses/looks.json"
TABLE = "Libs/Tables/Character/CharacterComponent.xml"
# The node kind of each part of a horse.
KINDS = {"body": "Body", "head": "Head", "hair": "Hair", "tail": "Beard"}
# The groups whose coats, and the names whose manes and tails, a horse gets when nothing is asked.
COMMON_COAT_GROUPS = ("Horse_Solid", "Horse_Dapple", "Horse_Grulla", "Horse_Body_Marking")
COMMON_HEADS = ("Work_horse", "Travel_horse", "War_horse", "Draft_horse", "Noble_horse")
COMMON_HAIR_PREFIX = "horse_hair_000_"
COMMON_TAILS = ("horse_tail_001", "horse_tail_001_shorter", "horse_tail_001_longer")


def read_table(data):
    with zipfile.ZipFile(data / "Tables.pak") as pak:
        return ElementTree.fromstring(pak.read(TABLE))


def horse_root(table):
    for node in table.iter("Component"):
        if node.get("Name") == "Horse" and node.get("Race") == "Horse":
            return node
    raise SystemExit("no Horse component in " + TABLE)


def children(node):
    derived = node.find("DerivedComponents")
    return list(derived) if derived is not None else []


def names_of(root, kind):
    """Every name of that kind under the root, each once, in the table's order."""
    names = []
    for node in root.iter(kind):
        name = node.get("Name")
        if name and name not in names:
            names.append(name)
    return names


def leaves_under(root, kind, group):
    """The names of that kind under the group's node that have none of that kind under them."""
    leaves = []
    for node in root.iter(kind):
        if node.get("Name") != group:
            continue
        for inner in node.iter(kind):
            if inner is not node and not any(child.tag == kind for child in children(inner)):
                leaves.append(inner.get("Name"))
    return leaves


def build(game):
    root = horse_root(read_table(Path(game) / "Data"))
    looks = {"source": TABLE}
    for part, kind in KINDS.items():
        looks[part] = names_of(root, kind)
    coats = []
    for group in COMMON_COAT_GROUPS:
        coats.extend(leaves_under(root, "Body", group))
    looks["common"] = {
        "body": coats,
        "head": [name for name in COMMON_HEADS if name in looks["head"]],
        "hair": [name for name in looks["hair"] if name.startswith(COMMON_HAIR_PREFIX)],
        "tail": [name for name in COMMON_TAILS if name in looks["tail"]],
    }
    return looks


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("game", help="the game's installation folder")
    args = parser.parse_args()
    looks = build(args.game)
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(json.dumps(looks, separators=(",", ":")) + "\n", encoding="utf8")
    for part in KINDS:
        print(f"{part}: {len(looks[part])} ({len(looks['common'][part])} common)")


if __name__ == "__main__":
    main()
