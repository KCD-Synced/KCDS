"""Builds the server's appearance catalog from a game installation.

The catalog is the game's character-component tree (Libs/Tables/Character/CharacterComponent.xml
in Data/Tables.pak) for the two human genders: every body, head, hair and beard a player's body
can be given, and for every head the beards modelled for it. A beard's model is stored under its
head's folder, so a head can wear a beard only when the head's folder carries that model.

    python Dev/tools/appearance/build_catalog.py "<game folder>"

writes Backend/scripting/runtimes/js/appearance/catalog.json.
"""

import argparse
import json
import xml.etree.ElementTree as ElementTree
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/appearance/catalog.json"
TABLE = "Libs/Tables/Character/CharacterComponent.xml"
PARTS = ("body", "head", "hair", "beard")
# The node kind of each part and the root node of each gender's subtree of that kind.
KINDS = {"body": "Body", "head": "Head", "hair": "Hair", "beard": "Beard"}
GENDERS = {"male": "Male", "female": "Female"}


def read_table(data):
    with zipfile.ZipFile(data / "Tables.pak") as pak:
        return ElementTree.fromstring(pak.read(TABLE))


def model_files(data):
    """Every file the head and character paks carry, lower case."""
    files = set()
    for pak_path in sorted(data.glob("*.pak")):
        if "Head" not in pak_path.name and "Character" not in pak_path.name:
            continue
        with zipfile.ZipFile(pak_path) as pak:
            for name in pak.namelist():
                files.add(name.lower())
    return files


def gender_root(table, gender):
    components = table.find("CharacterComponents")
    for component in components.findall("Component"):
        if component.get("Race") == "Human" and component.get("Name") == GENDERS[gender]:
            return component
    raise SystemExit(f"{TABLE}: no {gender} human")


# The parts of the body a player's body must draw. A body that sets one of them to the dummy model
# (hide_male_body, for clothes that cover all of it; male_body_experimental) leaves that part of
# the person out: the player showed as a head over a pair of trousers.
DRAWN_BODY_PARTS = ("torso", "arms", "legs")


def hides_body(node):
    """Whether the body's own elements draw a part of the person as nothing."""
    elements = node.find("Elements")
    if elements is None:
        return False
    for element in elements.findall("SkinElement"):
        if element.get("EquipmentPart") in DRAWN_BODY_PARTS and element.get("Model") == "dummy":
            return True
    return False


def options(root, part):
    """The part's options in the table's order: (name, group, folder, node).

    The gender's top node of the part's kind is the abstract base every option derives from; it
    is not an option. An option's group is the option it derives from, or None for one derived
    from the base. Its folder is the FilePath it or its nearest ancestor names. A body that hides
    part of the person (hides_body) is not an option, nor is anything derived from it.
    """
    kind = KINDS[part]
    found = []

    def walk(node, folder, group):
        for derived in node.findall("DerivedComponents"):
            for child in derived:
                if child.tag != kind:
                    continue
                if part == "body" and hides_body(child):
                    continue
                child_folder = child.get("FilePath") or folder
                found.append((child.get("Name"), group, child_folder, child))
                walk(child, child_folder, child.get("Name"))

    for derived in root.findall("DerivedComponents"):
        for base in derived:
            if base.tag == kind:
                walk(base, base.get("FilePath") or "", None)
    return found


def beard_model(node):
    for element in node.iter("SkinElement"):
        if element.get("EquipmentPart") == "beard" and element.get("Model"):
            return element.get("Model")
    return None


def head_beards(head_folder, beards, files):
    """Indexes of the beards whose model the head's folder carries."""
    folder = head_folder.replace("\\", "/").strip("/")
    wearable = []
    for index, (_, _, _, node) in enumerate(beards):
        model = beard_model(node)
        if not model or not model.startswith("beards/"):
            continue
        if f"objects/characters/{folder}/{model}".lower() in files:
            wearable.append(index)
    return wearable


def build(game):
    data = Path(game) / "Data"
    table = read_table(data)
    files = model_files(data)
    catalog = {"source": TABLE}
    for gender in GENDERS:
        root = gender_root(table, gender)
        parts = {part: options(root, part) for part in PARTS}
        entry = {}
        for part in PARTS:
            rows = []
            for name, group, folder, _ in parts[part]:
                row = [name, group]
                if part == "head":
                    row.append(head_beards(folder, parts["beard"], files))
                rows.append(row)
            entry[part] = rows
        catalog[gender] = entry
    return catalog


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("game", help="the game's installation folder")
    args = parser.parse_args()
    catalog = build(args.game)
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(catalog, separators=(",", ":"))
    OUTPUT.write_text(text + "\n", encoding="utf8")
    for gender in GENDERS:
        counts = ", ".join(f"{len(catalog[gender][part])} {part}" for part in PARTS)
        print(f"{gender}: {counts}")


if __name__ == "__main__":
    main()
