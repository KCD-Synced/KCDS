"""Builds the catalog of the levels' own chests from a game installation.

The catalog is what Stash.levelChests lists and Stash.adopt takes: every Stash entity (a chest, a
sack, a barrel, a wardrobe, ...) placed in the game's levels, in a level's objects
(objects_mission0.xml) and layers (layers/*.xml) of Data/Levels/<level>/level.pak. A row keeps the
chest's level GUID (sixteen lowercase hex digits, as the client finds level entities), where it
stands, its model, whether the level locks it (Properties/Lock bLocked), whether its lock can be
picked (bCanLockPick, the game's default when not set: yes) and the lock's difficulty when the
level sets one (fLockDifficulty with bLockDifficultyOverride; null where the game makes one up
itself as the chest resets, AnimStash.lua GenerateLockData).

    python Dev/tools/stash/build_level_chests.py "<game folder>"

writes Backend/scripting/runtimes/js/stash/level_chests.json: one row a line,
[level, guid, x, y, z, model, locked, canBePicked, difficulty].
"""

import argparse
import json
import re
import xml.etree.ElementTree as ElementTree
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/stash/level_chests.json"
LEVEL_FILES = re.compile(r"^(objects_mission0\.xml|layers/[^/]+\.xml)$", re.IGNORECASE)
STASH_CLASS = "Stash"


def read_entry(pak, name):
    # Some entries' local headers spell the path with backslashes.
    try:
        return pak.read(name)
    except zipfile.BadZipFile:
        info = pak.getinfo(name)
        info.orig_filename = name.replace("/", "\\")
        with pak.open(info) as entry:
            return entry.read()


def flag(element, name, default):
    if element is None or element.get(name) is None:
        return default
    return element.get(name) not in ("0", "false")


def chests_of(level, pak):
    found = {}
    for name in pak.namelist():
        if not LEVEL_FILES.match(name):
            continue
        text = read_entry(pak, name).decode("utf-8", errors="replace")
        for entity in ElementTree.fromstring(text).iter("Entity"):
            if entity.get("EntityClass") != STASH_CLASS or not entity.get("EntityGuid"):
                continue
            guid = entity.get("EntityGuid").replace("-", "").lower()
            position = [round(float(part), 3) for part in entity.get("Pos", "0,0,0").split(",")]
            properties = entity.find("Properties")
            lock = properties.find("Lock") if properties is not None else None
            difficulty = None
            if flag(lock, "bLockDifficultyOverride", False) and lock.get("fLockDifficulty"):
                difficulty = round(float(lock.get("fLockDifficulty")), 3)
            found[guid] = [
                level,
                guid,
                *position,
                properties.get("object_Model", "") if properties is not None else "",
                flag(lock, "bLocked", False),
                flag(lock, "bCanLockPick", True),
                difficulty,
            ]
    return [found[guid] for guid in sorted(found)]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("game", help="the game's installation folder")
    args = parser.parse_args()
    game = Path(args.game)
    chests = []
    for level_pak in sorted((game / "Data/Levels").glob("*/level.pak")):
        with zipfile.ZipFile(level_pak) as pak:
            chests.extend(chests_of(level_pak.parent.name, pak))
    lines = ["["]
    lines.append(",\n".join("  " + json.dumps(row) for row in chests))
    lines.append("]")
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"{OUTPUT}: {len(chests)} chests")


if __name__ == "__main__":
    main()
