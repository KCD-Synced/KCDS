"""Builds the progression catalog from a game installation.

The catalog is what Progression.tracks, perkNamed, perks and xpNeeded answer, on the server and
in every client's resource runtime, and what the native code numbers tracks and perks by:

- tracks: the skills the character sheet shows (rpg/skill.xml, not hidden) and the stats
  (rpg/stat.xml), with the game's own ids, level cap and XP curve;
- perks: every perk of the perk tables (rpg/perk*.xml) with its tree, level, parent, visibility,
  what it does (the perk_* side tables) and whether the game learns it by itself.

The XP curve: reaching level n costs base + diff * n XP. Base and diff are the game's RPG
parameters (rpg/rpg_param.xml overrides the defaults compiled into WHGame.dll; the defaults below
are WHGame 1.5.5's, read from its parameter table, fixed point / 128). The level cap of skills and
stats is the game's own 30 (the parameter struct's maximum levels).

    python Dev/tools/progression/build_catalog.py "<game folder>"

writes Backend/scripting/runtimes/js/progression/catalog.json.
"""

import argparse
import json
import xml.etree.ElementTree as ElementTree
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/progression/catalog.json"
TABLES = "Libs/Tables/rpg/"
PERK_TABLES = (
    "perk.xml",
    "perk__combat.xml",
    "perk__dlc2.xml",
    "perk__hardcore.xml",
    "perk__kcd2.xml",
    "perk__recipe.xml",
)
# The side tables that say what an owned perk does, and the kind each one stands for.
KIND_TABLES = (
    ("perk_buff.xml", "buff"),
    ("perk_script.xml", "script"),
    ("perk_combat_technique.xml", "technique"),
    ("perk_soul_ability.xml", "ability"),
    ("perk_rpg_param_override.xml", "param"),
    ("perk_companion.xml", "companion"),
    ("perk_recipe.xml", "recipe"),
    ("perk_codex.xml", "codex"),
)
VISIBILITY = {"0": "system", "1": "hidden", "2": "visible", "3": "obsolete"}
LEVEL_CAP = 30
# The skill the game derives from the weapon skills instead of earning it.
INDUCED = {"fencing"}
# WHGame 1.5.5's compiled-in XP curve parameters; rpg_param.xml overrides some of them.
CURVE_DEFAULTS = {
    "SkillXPLevelBase": 20,
    "SkillXPLevelDiff": 30,
    "AgilityXPLevelBase": 20,
    "AgilityXPLevelDiff": 30,
    "StrengthXPLevelBase": 20,
    "StrengthXPLevelDiff": 30,
    "VitalityXPLevelBase": 20,
    "VitalityXPLevelDiff": 30,
    "SpeechXPLevelBase": 20,
    "SpeechXPLevelDiff": 30,
    "PrestigeXPLevelBase": 20,
    "PrestigeXPLevelDiff": 30,
}
# The parameter pair each stat's curve reads (the game's stat-curve switch); skills share Skill*.
STAT_CURVES = {
    "strength": "Strength",
    "agility": "Agility",
    "vitality": "Vitality",
    "speech": "Speech",
    "prestige": "Prestige",
}


def read(pak, name):
    return ElementTree.fromstring(pak.read(TABLES + name))


def rows(table, tag):
    return [element.attrib for element in table.iter(tag)]


def curve_parameters(pak):
    parameters = dict(CURVE_DEFAULTS)
    for row in rows(read(pak, "rpg_param.xml"), "rpg_param"):
        if row["rpg_param_key"] in parameters:
            parameters[row["rpg_param_key"]] = float(row["rpg_param_value"])
    return parameters


def number(value):
    return int(value) if float(value).is_integer() else float(value)


def tracks(pak):
    parameters = curve_parameters(pak)
    result = []
    for row in rows(read(pak, "skill.xml"), "skill"):
        if row.get("hidden") == "true":
            continue
        result.append(
            {
                "name": row["skill_name"],
                "kind": "skill",
                "id": int(row["skill_id"]),
                "cap": LEVEL_CAP,
                "induced": row["skill_name"] in INDUCED,
                "base": number(parameters["SkillXPLevelBase"]),
                "diff": number(parameters["SkillXPLevelDiff"]),
            }
        )
    for row in rows(read(pak, "stat.xml"), "stat"):
        curve = STAT_CURVES[row["stat_name"]]
        result.append(
            {
                "name": row["stat_name"],
                "kind": "stat",
                "id": int(row["stat_id"]),
                "cap": LEVEL_CAP,
                "induced": False,
                "base": number(parameters[curve + "XPLevelBase"]),
                "diff": number(parameters[curve + "XPLevelDiff"]),
            }
        )
    return result


def perks(pak, track_list):
    skills = {track["id"]: track["name"] for track in track_list if track["kind"] == "skill"}
    stats = {track["id"]: track["name"] for track in track_list if track["kind"] == "stat"}
    kinds = {}
    for table, kind in KIND_TABLES:
        for row in rows(read(pak, table), table[:-4]):
            kinds.setdefault(row["perk_id"], set()).add(kind)
    table_rows = []
    for table in PERK_TABLES:
        table_rows.extend(rows(read(pak, table), "perk"))
    names = {row["perk_id"]: row["perk_name"] for row in table_rows}
    result = []
    for row in table_rows:
        level = int(row.get("level", "0"))
        if "skill_selector" in row:
            track = skills[int(row["skill_selector"])]
        elif "stat_selector" in row:
            track = stats[int(row["stat_selector"])]
        elif level > 0:
            # A perk with a level and no skill or stat tree is paid from the main-level bank.
            track = "main"
        else:
            track = None
        result.append(
            {
                "name": row["perk_name"],
                "id": row["perk_id"],
                "uiName": row.get("perk_ui_name", ""),
                "track": track,
                "level": level,
                "parent": names.get(row.get("parent_id")),
                "visibility": VISIBILITY[row["visibility"]],
                "kinds": sorted(kinds.get(row["perk_id"], ())),
                "autolearnable": row["autolearnable"] == "true",
            }
        )
    result.sort(key=lambda perk: perk["name"])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("game", type=Path, help="the game's installation folder")
    args = parser.parse_args()
    with zipfile.ZipFile(args.game / "Data" / "Tables.pak") as pak:
        track_list = tracks(pak)
        catalog = {"tracks": track_list, "perks": perks(pak, track_list)}
    # One perk per line keeps a table change a readable diff.
    lines = ["{", '  "tracks": [']
    lines.append(",\n".join("    " + json.dumps(track) for track in catalog["tracks"]))
    lines.append("  ],")
    lines.append('  "perks": [')
    lines.append(
        ",\n".join("    " + json.dumps(perk, ensure_ascii=False) for perk in catalog["perks"])
    )
    lines.append("  ]")
    lines.append("}")
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(("\n".join(lines) + "\n").encode("utf-8"))
    print(f"{len(catalog['tracks'])} tracks, {len(catalog['perks'])} perks -> {OUTPUT}")


if __name__ == "__main__":
    main()
