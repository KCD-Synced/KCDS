#!/usr/bin/env python3
"""Writes the game's animal loot for Npc.lootOf.

build_animal_loot.py <game folder> <out.json>

Reads Tables.pak: an animal's soul (rpg/soul__animal.xml) by its soul_name "animal_x", its preset
(item/InventoryPreset__animals.xml) by "inventory_animal_x". A soul whose preset has another name is
matched by hand in ALIASES. The output is { soul guid: { soul, preset, items: [row] } }, and a row is
{ name, amount, fraction, variation }: the fraction and its variation are the preset's ModeValue and
ModeValueVariation, for all its rows.
"""
import json
import re
import sys
import xml.etree.ElementTree as ElementTree
import zipfile

SOULS = "Libs/Tables/rpg/soul__animal.xml"
PRESETS = "Libs/Tables/item/InventoryPreset__animals.xml"
# Soul name -> preset name, where the two do not follow the rule.
ALIASES = {
    "animal_cattle_bull": "inventory_animal_bull",
    "animal_cattle_cow": "inventory_animal_cow",
    "animal_legacyOfTheForge_cow1": "inventory_animal_cow",
    "animal_legacyOfTheForge_pig1": "inventory_animal_pig",
    "animal_legacyOfTheForge_pig2": "inventory_animal_pig",
    "animal_legacyOfTheForge_pig3": "inventory_animal_pig",
    "animal_red_deer_doe": "inventory_animal_deerDoe",
    "animal_red_deer_stag": "inventory_animal_redDeer",
    "animal_roe_deer_buck": "inventory_animal_roeBuckMale",
    "animal_roe_deer_hind": "inventory_animal_roeBuckFemale",
    "animal_sheep_ewe": "inventory_animal_sheep",
    "animal_sheep_ram": "inventory_animal_ram",
    "animal_wolf_kutnohorsko": "inventory_animal_wolf",
    "animal_wolf_trosecko": "inventory_animal_wolf",
    "bozenasPig": "inventory_animal_pig",
    "taborOdboje_wolf_1": "inventory_animal_wolf",
    "taborOdboje_wolf_2": "inventory_animal_wolf",
    "taborOdboje_wolf_3": "inventory_animal_wolf",
    "taborOdboje_wolf_4": "inventory_animal_wolf",
}


def read_presets(archive):
    root = ElementTree.fromstring(archive.read(PRESETS))
    presets = {}
    for preset in root.iter("InventoryPreset"):
        fraction = float(preset.get("ModeValue", "1"))
        variation = float(preset.get("ModeValueVariation", "0"))
        rows = []
        for item in preset.iter("PresetItem"):
            rows.append(
                {
                    "name": item.get("Name"),
                    "amount": int(item.get("Amount", "1")),
                    "fraction": fraction,
                    "variation": variation,
                }
            )
        presets[preset.get("Name")] = rows
    return presets


def read_souls(archive):
    root = ElementTree.fromstring(archive.read(SOULS))
    return {soul.get("soul_name"): soul.get("soul_id") for soul in root.iter("soul")}


def preset_of(soul_name, presets):
    if soul_name in ALIASES:
        return ALIASES[soul_name]
    wanted = re.sub(r"^animal_", "inventory_animal_", soul_name)
    return wanted if wanted in presets else None


def main():
    game, out = sys.argv[1], sys.argv[2]
    archive = zipfile.ZipFile(f"{game}/Data/Tables.pak")
    presets = read_presets(archive)
    souls = read_souls(archive)
    table = {}
    unmatched = []
    used = set()
    for soul_name, guid in sorted(souls.items()):
        preset = preset_of(soul_name, presets)
        if preset is None:
            unmatched.append(soul_name)
            continue
        used.add(preset)
        table[guid] = {"soul": soul_name, "preset": preset, "items": presets[preset]}
    with open(out, "w", encoding="utf-8", newline="\n") as file:
        json.dump(table, file, indent=2, sort_keys=True)
        file.write("\n")
    print(f"{len(table)} souls with loot")
    print("souls without a preset:", unmatched)
    print("presets no soul has:", sorted(set(presets) - used))


if __name__ == "__main__":
    main()
