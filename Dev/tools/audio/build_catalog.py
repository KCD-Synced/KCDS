"""Builds the audio trigger catalog from a game installation.

The catalog is what the server's Audio.list answers: the names of the game's sound triggers
(the ATL triggers of Libs/GameAudio/*.xml in the game's IPL_GameData.pak), sorted. Names only:
nothing of the sounds themselves.

Triggers of the cinematics (cin_*, most of the game's triggers) play without an error outside
their scene but are not heard there, because their sound bank is loaded only with the scene, so
they are left out; the docs say so.

    python Dev/tools/audio/build_catalog.py "<game folder>"

writes Backend/scripting/runtimes/js/audio/catalog.json.
"""

import argparse
import json
import re
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/audio/catalog.json"
PAK = "Data/IPL_GameData.pak"
FOLDER = "libs/gameaudio/"
TRIGGER = re.compile(r'<ATLTrigger\s+atl_name="([^"]+)"')
CINEMATIC = "cin_"


def triggers(pak):
    """The trigger names of every Libs/GameAudio XML file in the pak, without the cinematics'."""
    names = set()
    with zipfile.ZipFile(pak) as archive:
        for entry in archive.namelist():
            lower = entry.lower()
            if lower.startswith(FOLDER) and lower.endswith(".xml"):
                text = archive.read(entry).decode("utf-8", "replace")
                for match in TRIGGER.finditer(text):
                    if not match.group(1).startswith(CINEMATIC):
                        names.add(match.group(1))
    return sorted(names)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("game", type=Path, help="the game's folder (KingdomComeDeliverance2)")
    args = parser.parse_args()
    pak = args.game / PAK
    if not pak.is_file():
        parser.error(f"{pak} not found")
    names = triggers(pak)
    catalog = {"source": "Libs/GameAudio/*.xml (ATLTrigger atl_name)", "triggers": names}
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(json.dumps(catalog, indent=0) + "\n", encoding="utf-8")
    print(f"{len(names)} triggers -> {OUTPUT}")


if __name__ == "__main__":
    main()
