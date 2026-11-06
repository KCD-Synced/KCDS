"""Builds the catalog of named places from a game installation.

World.setPlaceName renames a place by replacing the text of its localization keys. A place is a
settlement of a faction ("ui_fac_<region>_settlements_<id>") or a quest location ("location_<id>"),
both in the game's own text tables. Its id is the part after the prefix, and its name the English
text.

    python Dev/tools/world/build_places.py "<game folder>"

writes Backend/scripting/runtimes/js/world/places.json.
"""

import argparse
import json
import re
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/world/places.json"
ROW = re.compile(r"<Row><Cell>([^<]+)</Cell><Cell>[^<]*</Cell><Cell>([^<]*)</Cell>")
SETTLEMENT = re.compile(r"^ui_fac_[A-Za-z]+_settlements_([A-Za-z0-9]+)$")
LOCATION = re.compile(r"^location_([A-Za-z0-9]+)$")
# A place's name, not a sentence the same prefix also holds.
LONGEST_NAME = 40
NOT_PLACES = {"anywhere", "test", "testpoi"}


def rows_of(game):
    with zipfile.ZipFile(Path(game) / "Localization/English_xml.pak") as pak:
        for name in pak.namelist():
            yield from ROW.findall(pak.read(name).decode("utf-8", errors="replace"))


def places_of(rows):
    places = {}
    for key, text in rows:
        match = SETTLEMENT.match(key) or LOCATION.match(key)
        if not match or not text or len(text) > LONGEST_NAME or text.endswith("."):
            continue
        place = match.group(1)
        if place.lower() in NOT_PLACES:
            continue
        entry = places.setdefault(place, {"id": place, "name": text, "keys": []})
        entry["keys"].append(key)
    for entry in places.values():
        entry["keys"].sort()
    return [places[place] for place in sorted(places)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game", help="the game's folder")
    arguments = parser.parse_args()
    places = places_of(rows_of(arguments.game))
    OUTPUT.write_text(json.dumps(places, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"{len(places)} places")


if __name__ == "__main__":
    main()
