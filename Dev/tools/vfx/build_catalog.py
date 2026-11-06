"""Builds the particle effect catalog from a game installation.

Vfx.list answers it, on the server and in client scripts: every effect the game's particle
libraries carry (Libs/Particles/*.xml in Data/IPL_GameData.pak), named as the game loads one,
"<library>.<group>.<name>" (the library's Name and each top-level Particles entry's Name; the
entries below those are an effect's own emitters, not effects of their own).

    python Dev/tools/vfx/build_catalog.py "<game folder>"

writes Backend/scripting/runtimes/js/vfx/catalog.json: { "source", "effects": [sorted names] }.
"""

import argparse
import json
import re
import xml.etree.ElementTree as ElementTree
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/vfx/catalog.json"
PAK = "IPL_GameData.pak"
# The names Vfx takes (vfx_values.cjs EFFECT_NAME): letters, digits, _ and - between the dots. A few
# cinematic effects spell themselves with brackets and cannot be asked for.
EFFECT_NAME = re.compile(r"^[A-Za-z0-9_-]+(?:\.[A-Za-z0-9_-]+){2,}$")
LIBRARIES = "Libs/Particles/"


def effects_of(pak):
    names = set()
    for entry in pak.namelist():
        if not entry.startswith(LIBRARIES) or not entry.lower().endswith(".xml"):
            continue
        library = ElementTree.fromstring(pak.read(entry))
        library_name = library.get("Name", "")
        if not library_name:
            continue
        for effect in library.findall("Particles"):
            name = effect.get("Name", "")
            # A loadable name is library.group.name (more dots may follow).
            full = f"{library_name}.{name}"
            if EFFECT_NAME.match(full):
                names.add(full)
    return sorted(names)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("game", type=Path, help="the game's installation folder")
    game = parser.parse_args().game
    with zipfile.ZipFile(game / "Data" / PAK) as pak:
        effects = effects_of(pak)
    catalog = {"source": f"Data/{PAK} {LIBRARIES}*.xml", "effects": effects}
    lines = ["{", f'  "source": {json.dumps(catalog["source"])},', '  "effects": [']
    lines.append(",\n".join("    " + json.dumps(name) for name in effects))
    lines.append("  ]")
    lines.append("}")
    OUTPUT.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"{OUTPUT}: {len(effects)} effects")


if __name__ == "__main__":
    main()
