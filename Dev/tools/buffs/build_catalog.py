"""Builds the buff catalog from a game installation.

The catalog is what Buffs.find, Buffs.classes and Buffs.tags answer, on the server and in every
client's resource runtime, and what the native code numbers buff classes and families by:

- classes: the game's buff classes (rpg/buff_class.xml) with their ids, named as the API names
  them ("Testing Stat Buff" is testingStat, "Food Poison" is foodPoison), and whether a server can
  claim them (the kinds that carry state nobody can work out for themselves);
- tags: the families player.clearBuffs takes (rpg/buff_ai_tag.xml), with their ids;
- buffs: every buff of the buff tables (rpg/buff.xml and rpg/buff__*.xml): name, GUID, class,
  family and duration in real seconds.

Durations: the tables give seconds; the effects whose implementation runs on the world clock
(Cpp:WorldTimeTimed and its kin) count game seconds, which pass 15 times faster than real ones at
the game's default clock speed. A negative duration is an effect with no end.

    python Dev/tools/buffs/build_catalog.py "<game folder>"

writes Backend/scripting/runtimes/js/buffs/catalog.json.
"""

import argparse
import json
import xml.etree.ElementTree as ElementTree
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/buffs/catalog.json"
TABLES = "Libs/Tables/rpg/"
# The classes a server can claim: potions, poison, injury, drunkenness, illness,
# unconsciousness and their kin. The others shadow state that already replicates (perks, items)
# or are the game's own plumbing (system, script, testing).
CLAIMABLE = {
    "injury",
    "heal",
    "poison",
    "perception",
    "overeat",
    "alcohol",
    "potion",
    "foodPoison",
    "unconsciousness",
    "hangover",
    "satisfaction",
    "perfume",
    "punishment",
    "forcedDrunkenness",
    "plague",
}
# The implementations that time an effect by the world clock.
WORLD_CLOCK = {
    "Cpp:WorldTimeTimed",
    "Cpp:PoisoningWorldTimeTimed",
    "Cpp:AdjustableFadingWorldTimeTimed",
    "Cpp:Hangover",
    "Cpp:LastGaspShortTerm",
    "Cpp:ForgeWellRested",
}
# Game seconds per real second at the game's default clock speed.
WORLD_CLOCK_SCALE = 15


def read(pak, name):
    return ElementTree.fromstring(pak.read(TABLES + name))


def rows(table, tag):
    return [element.attrib for element in table.iter(tag)]


def class_name(table_name):
    """The API's name of a buff class: "Testing Stat Buff" is testingStat."""
    words = table_name.split()
    if len(words) > 1 and words[-1] == "Buff":
        words = words[:-1]
    joined = "".join(word[0].upper() + word[1:] for word in words)
    return joined[0].lower() + joined[1:]


def classes(pak):
    result = []
    for row in rows(read(pak, "buff_class.xml"), "buff_class"):
        name = class_name(row["buff_class_name"])
        result.append(
            {"name": name, "id": int(row["buff_class_id"]), "claimable": name in CLAIMABLE}
        )
    missing = CLAIMABLE - {entry["name"] for entry in result}
    if missing:
        raise SystemExit(f"claimable classes the tables do not carry: {sorted(missing)}")
    return result


def tags(pak):
    return [
        {"name": row["buff_ai_tag_name"], "id": int(row["buff_ai_tag_id"])}
        for row in rows(read(pak, "buff_ai_tag.xml"), "buff_ai_tag")
    ]


def real_seconds(row):
    duration = float(row["duration"])
    if duration < 0:
        return -1
    if row["implementation"] in WORLD_CLOCK:
        duration /= WORLD_CLOCK_SCALE
    duration = round(duration, 4)
    return int(duration) if duration.is_integer() else duration


def buffs(pak, class_list, tag_list):
    class_names = {entry["id"]: entry["name"] for entry in class_list}
    tag_names = {entry["id"]: entry["name"] for entry in tag_list}
    tables = sorted(
        name[len(TABLES) :]
        for name in pak.namelist()
        if name.startswith(TABLES + "buff") and name.endswith(".xml")
    )
    tables = [name for name in tables if name == "buff.xml" or name.startswith("buff__")]
    result = []
    for table in tables:
        for row in rows(read(pak, table), "buff"):
            tag = row.get("buff_ai_tag_id")
            result.append(
                {
                    "name": row["buff_name"],
                    "id": row["buff_id"],
                    "class": class_names[int(row["buff_class_id"])],
                    "aiTag": tag_names[int(tag)] if tag is not None else None,
                    "duration": real_seconds(row),
                }
            )
    result.sort(key=lambda buff: buff["name"])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("game", type=Path, help="the game's installation folder")
    args = parser.parse_args()
    with zipfile.ZipFile(args.game / "Data" / "Tables.pak") as pak:
        class_list = classes(pak)
        tag_list = tags(pak)
        buff_list = buffs(pak, class_list, tag_list)
    # One entry per line keeps a table change a readable diff.
    lines = ["{", '  "classes": [']
    lines.append(",\n".join("    " + json.dumps(entry) for entry in class_list))
    lines.append("  ],")
    lines.append('  "tags": [')
    lines.append(",\n".join("    " + json.dumps(entry) for entry in tag_list))
    lines.append("  ],")
    lines.append('  "buffs": [')
    lines.append(",\n".join("    " + json.dumps(buff, ensure_ascii=False) for buff in buff_list))
    lines.append("  ]")
    lines.append("}")
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(("\n".join(lines) + "\n").encode("utf-8"))
    print(f"{len(class_list)} classes, {len(tag_list)} tags, {len(buff_list)} buffs -> {OUTPUT}")


if __name__ == "__main__":
    main()
