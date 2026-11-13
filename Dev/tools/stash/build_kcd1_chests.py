#!/usr/bin/env python3
"""Builds a KCDS server's level chests for the KCD1 map (kcd1_rataje) from a KCD1 installation.

The game client keeps each of KCD1's Stash entities (a chest, a sack, a shelf, ...) as KCD2's Stash
with KCD1's own level GUID (Client/runtime/kcd1/entities.hpp), so a server can adopt it as it adopts
the KCD2 levels' chests (Stash.levelChests, Stash.adopt). This tool lists them in the level chest
catalog's form (stash/level_chests.json, see build_level_chests.py), from KCD1's objects
(objects_mission0.xml of Data/Levels/rataje/level.pak), the file the client translates.

    python Dev/tools/stash/build_kcd1_chests.py "<KCD1 folder>" "<server resources folder>"

writes <resources>/level-chests.json, read by the server's stash module (stash/stashes.cjs). It is
made from the player's own KCD1 files and stays on that server.
"""

import argparse
import json
import sys
import zipfile
from pathlib import Path

# The KCD2 catalog's builder, beside this one, reads a level's chests the same way.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_level_chests import chests_of  # noqa: E402

LEVEL = "kcd1_rataje"
OBJECTS = "objects_mission0.xml"


class ObjectsOnly:
    """The level pack with only its objects file listed: the client translates no layer files."""

    def __init__(self, pak):
        self.pak = pak

    def namelist(self):
        return [name for name in self.pak.namelist() if name.replace("\\", "/").lower() == OBJECTS]

    def read(self, name):
        return self.pak.read(name)

    def getinfo(self, name):
        return self.pak.getinfo(name)

    def open(self, info):
        return self.pak.open(info)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("kcd1", type=Path, help="the KCD1 installation folder")
    parser.add_argument("resources", type=Path, help="the server's resources folder")
    args = parser.parse_args()

    level_pack = args.kcd1 / "Data" / "Levels" / "rataje" / "level.pak"
    with zipfile.ZipFile(level_pack) as pak:
        chests = chests_of(LEVEL, ObjectsOnly(pak))
    lines = ["["]
    lines.append(",\n".join("  " + json.dumps(row) for row in chests))
    lines.append("]")
    output = args.resources / "level-chests.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(chests)} chests -> {output}")


if __name__ == "__main__":
    main()
