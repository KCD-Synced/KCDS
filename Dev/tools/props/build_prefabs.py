"""Builds the server's catalog of the game's prefabs from a game installation.

A prefab is one of the game's building templates (IPL_GameData.pak Prefabs/**.xml: a house, a
wall, a well), a set of meshes, lights and the like the game lays out together. Prop.spawn takes
one by its path under Prefabs/. A template may hold other prefabs (a house its doors):
the client builds those from their own templates, so a template is listed apart, with why, only
when a prefab it holds (at any depth) is one the game does not have.

    python Dev/tools/props/build_prefabs.py "<game folder>"

writes Backend/scripting/runtimes/js/props/prefabs.json.
"""

import argparse
import json
import xml.etree.ElementTree as ElementTree
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/props/prefabs.json"
PAK = "Data/IPL_GameData.pak"
MISSING = "holds a prefab the game does not have"


def templates(game):
    """(path key, GUID, GUIDs of the prefabs it holds) of every template."""
    with zipfile.ZipFile(Path(game) / PAK) as pak:
        for name in sorted(pak.namelist()):
            path = name.replace("\\", "/").lower()
            if not (path.startswith("prefabs/") and path.endswith(".xml")):
                continue
            root = ElementTree.fromstring(pak.read(name))
            if root.tag != "Prefab" or not root.get("Id"):
                continue
            objects = root.find("Objects")
            held = []
            if objects is not None:
                for item in objects.findall("Object"):
                    if item.get("Type") == "Prefab":
                        held.append((item.get("PrefabTemplate") or "").lower())
            key = path[len("prefabs/") : -len(".xml")]
            yield key, root.get("Id").lower(), held


def build(game):
    found = list(templates(game))
    held_by_guid = {guid: held for _, guid, held in found}

    def complete(guid, seen):
        if guid not in held_by_guid or guid in seen:
            return guid in held_by_guid
        return all(complete(inner, seen | {guid}) for inner in held_by_guid[guid])

    prefabs = {}
    blocked = {}
    for key, guid, _ in found:
        if complete(guid, frozenset()):
            prefabs[key] = guid
        else:
            blocked[key] = MISSING
    return {"source": f"{PAK} Prefabs/**/*.xml", "prefabs": prefabs, "blocked": blocked}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("game", help="the game's installation folder")
    args = parser.parse_args()
    catalog = build(args.game)
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(json.dumps(catalog, indent=0, sort_keys=True) + "\n", encoding="utf8")
    print(f"{len(catalog['prefabs'])} prefabs, {len(catalog['blocked'])} blocked")


if __name__ == "__main__":
    main()
