"""Builds the NPC roles: Npc.roles() and Npc.create({ soul: '<role>' }).

A role is a name for one real soul of the game's own tables, chosen so the body looks the part:
a guard in a guard's clothes, a townswoman in a commoner woman's. Each soul is checked against the
installation's Libs/Tables/rpg/soul*.xml (Data/Tables.pak, read only): it must exist and have the
look named here (its skald_character_name). The body follows the look: a WOMAN look is NPC_Female.

    python Dev/tools/npcs/build_roles.py "<game folder>"

writes Backend/scripting/runtimes/js/npcs/roles.json: [{ role, soul, actorClass, look }].
"""

import argparse
import json
import re
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/npcs/roles.json"

# role, soul GUID, the look the soul must have.
ROLES = [
    ("guard", "63daa80b-d1f4-46f5-8222-c22a541cae07", "char_GENERIC_MAN_GUARD_15"),
    ("guard_alt", "2e96ee86-7c95-4b57-91f6-6d3abce75d22", "char_GENERIC_MAN_GUARD_16"),
    ("knight", "fc4536b2-4964-4f79-af47-c6e7a3e5ec29", "char_GENERIC_MAN_GUARD_05"),
    (
        "bandit",
        "a081e02a-6bbf-4a17-bf26-e6a31aea10af",
        "char_GENERIC_MAN_ENEMY_BANDIT_02",
    ),
    (
        "bandit_alt",
        "02c59661-b14a-4f3c-9239-f31de1e2be57",
        "char_GENERIC_MAN_ENEMY_BANDIT_14",
    ),
    ("cuman", "47e6faf8-0387-367e-4c70-0003bf03dfa8", "char_GENERIC_MAN_CUMAN_04"),
    (
        "townsman",
        "51ea9c51-48b0-4f44-bedb-081f252b6ba4",
        "char_GENERIC_MAN_COMMONER_02",
    ),
    (
        "townsman_alt",
        "f61ce06c-93ca-4157-af2f-fc286b5ad83f",
        "char_GENERIC_MAN_COMMONER_03",
    ),
    (
        "townswoman",
        "f80c07aa-d033-4e94-a61e-ef4a24c10bc2",
        "char_GENERIC_WOMAN_COMMONER_10",
    ),
    (
        "townswoman_alt",
        "41c12ac6-ba41-4ffa-87a0-69574c80b633",
        "char_GENERIC_WOMAN_COMMONER_08",
    ),
]

SOUL_ROW = re.compile(r"<soul\s[^>]*>")
ATTRIBUTE = re.compile(r'(\w+)="([^"]*)"')


def souls_of(tables):
    """Every soul row of the tables: soul GUID -> its attributes."""
    souls = {}
    with zipfile.ZipFile(tables) as pak:
        for name in pak.namelist():
            lower = name.lower()
            if not (lower.startswith("libs/tables/rpg/soul") and lower.endswith(".xml")):
                continue
            # soul2perk and the other link tables hold no souls.
            if Path(lower).stem.startswith("soul2"):
                continue
            text = pak.read(name).decode("utf-8", "replace")
            for row in SOUL_ROW.findall(text):
                fields = dict(ATTRIBUTE.findall(row))
                soul = fields.get("soul_id", "").lower()
                if soul:
                    souls[soul] = fields
    return souls


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("game")
    options = parser.parse_args()
    souls = souls_of(Path(options.game) / "Data" / "Tables.pak")
    roles = []
    for role, soul, look in ROLES:
        fields = souls.get(soul)
        if fields is None:
            raise SystemExit(f"{role}: soul {soul} is not in the game's tables")
        if fields.get("skald_character_name") != look:
            found = fields.get("skald_character_name")
            raise SystemExit(f"{role}: soul {soul} looks like {found}, not {look}")
        actor_class = "NPC_Female" if "_WOMAN_" in look else "NPC"
        roles.append({"role": role, "soul": soul, "actorClass": actor_class, "look": look})
    OUTPUT.write_text(json.dumps(roles, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: {len(roles)} roles of {len(souls)} souls")


if __name__ == "__main__":
    main()
