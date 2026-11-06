"""Builds the animation catalog from a game installation.

The catalog is what Animations.list and Animations.props answer: every animation the game plays
on a human body, as rows of its own actor_anim_action table, and the tools it puts in people's
hands.

- animations: Libs/Tables/action/actor_anim_action.xml, the rows of the human male (actor class
  hash 1578932418) and female (-116224680) animation databases, one per fragment and tags. A row
  keeps its duration, whether it ends by itself and takes the whole body, what each hand holds
  (the manipulation type's tag, Libs/Tables/item/ItemManipulationType.xml), which bodies have it,
  and whether it animates a second character (a secondary alignment other than the identity).
  A row is a player's when the player's own Mannequin fragment ids have its fragment:
  every player body, a woman's too, plays with the player's controller definition
  (Scripts/Entities/actor/player.lua ActionController, kcd_male_controllerdefs.xml), so it plays
  the male NPCs' fragments it has and none of the fragments only the female NPCs' definition has
  (wh_female_controllerdefs.xml: Bartender_TakeAndTapStein, PickingStuff).
  The in/loop/out sequence, its phase and a two-person partner are read from the fragment names.
  A hand the table leaves empty is filled from the animation's own events: an
  animation that picks an item up from its station ("pickup" with "<action>,<slot>,<hand>" in
  Animations/humans/*/*.animevents, the fragment's animations from Animations/Mannequin/ADB) holds
  that slot's item, named by the slot (GetMash: "scoopSlot" in the right hand).
- props: the NPCTool items of Libs/Tables/item/item*.xml with a model and a manipulation type
  above "default", by name, with the model and the manipulation's tag.

    python Dev/tools/animations/build_catalog.py "<game folder>"

writes Backend/scripting/runtimes/js/animation/catalog.json: one row a line,
animations [fragment, tags, duration, flags, rightHand, leftHand, sequence, phase, partner, clip]
with flags 1 oneShot, 2 fullBody, 4 male, 8 female, 16 slaveBody, 32 player, and props
[name, model, hand]. clip is the first clip the fragment's options play (the game's "blank" left
out), by its name in the body's animation set: what playAnimation's upperBody plays on a layer of
its own.
"""

import argparse
import json
import re
import xml.etree.ElementTree as ElementTree
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/animation/catalog.json"
ACTIONS = "Libs/Tables/action/actor_anim_action.xml"
MANIPULATIONS = "Libs/Tables/item/ItemManipulationType.xml"
ANIMATIONS_PAK = "Animations.pak"
DATABASES = "Animations/Mannequin/ADB/"
EVENTS = ["Animations/humans/male/male.animevents", "Animations/humans/female/female.animevents"]
MALE = "1578932418"
FEMALE = "-116224680"
IDENTITY = "0;0;0;0;0;0;1"
ONE_SHOT = 1
FULL_BODY = 2
FOR_MALE = 4
FOR_FEMALE = 8
SLAVE_BODY = 16
FOR_PLAYER = 32
# The player's fragment ids (its controller definition's Fragments file) and what they import.
PLAYER_FRAGMENTS = "Animations/Mannequin/ADB/kcd_male_fragmentids.xml"
# A fragment's place in its sequence, from the end of its name.
PHASES = [
    ("_VAR", "variation"),
    ("VAR", "variation"),
    ("_Var", "variation"),
    ("Var", "variation"),
    ("Loop", "loop"),
    ("In", "in"),
    ("Out", "out"),
]
PARTNERS = [("_Master", "_Slave"), ("_Slave", "_Master")]


def manipulation_tags(pak):
    tags = {}
    for row in ElementTree.fromstring(pak.read(MANIPULATIONS)).iter("ItemManipulationData"):
        tags[row.get("Id")] = row.get("Tag", "")
    return tags


def hand_of(tags, value):
    if value in (None, "", "-1"):
        return ""
    return tags.get(value, "")


def fragment_animations(pak):
    """(fragment, tags) -> the names of the animations its options play, from every database."""
    found = {}
    for name in pak.namelist():
        if not (name.startswith(DATABASES) and name.endswith(".adb")):
            continue
        root = ElementTree.fromstring(pak.read(name))
        for fragments in root.iter("FragmentList"):
            for fragment in fragments:
                for option in fragment.iter("Fragment"):
                    key = (fragment.tag, option.get("FragTags", ""))
                    for animation in option.iter("Animation"):
                        found.setdefault(key, []).append(animation.get("name", "").lower())
    return found


def pickups(pak):
    """An animation's name -> {"Right"|"Left": the station slot it picks an item up from}."""
    found = {}
    for path in EVENTS:
        for animation in ElementTree.fromstring(pak.read(path)).iter("animation"):
            name = animation.get("name", "").lower().rsplit("/", 1)[-1].removesuffix(".caf")
            for event in animation.iter("event"):
                if event.get("name") != "pickup":
                    continue
                parts = event.get("parameter", "").split(",")
                if len(parts) >= 3 and parts[1] and parts[2] in ("Right", "Left"):
                    found.setdefault(name, {}).setdefault(parts[2], parts[1])
    return found


def picked_hands(fragment, fragment_tags, played, picked):
    """The slots a fragment's animations pick items up from, right and left."""
    right = left = ""
    for animation in played.get((fragment, fragment_tags), []):
        hands = picked.get(animation, {})
        right = right or hands.get("Right", "")
        left = left or hands.get("Left", "")
    return right, left


def sequence_of(fragment, fragments):
    """The sequence and phase of a fragment whose name ends like one of a sequence's parts, when
    another fragment shares the sequence."""
    for suffix, phase in PHASES:
        if not fragment.endswith(suffix) or len(fragment) == len(suffix):
            continue
        base = fragment[: -len(suffix)]
        siblings = [
            base + other for other, _ in PHASES if other != suffix and base + other in fragments
        ]
        if siblings or (phase == "loop" and base in fragments):
            return base, phase
        if phase != "variation" and base in fragments:
            return base, phase
    return "", ""


def clips_of(played, fragment, fragment_tags):
    """The animations of the fragment's option with these tags, else of its option without tags,
    else of any of its options (the databases spell some tags otherwise than the action table)."""
    exact = played.get((fragment, fragment_tags))
    if exact:
        return exact
    untagged = played.get((fragment, ""))
    if untagged:
        return untagged
    for (name, _), animations in sorted(played.items()):
        if name == fragment and animations:
            return animations
    return []


def first_clip(animations):
    """The first clip of a fragment's options by its name in the animation set, '' for none."""
    for name in animations:
        clip = name.rsplit("/", 1)[-1].removesuffix(".caf")
        if clip and clip != "blank":
            return clip
    return ""


def player_fragments(pak, name=PLAYER_FRAGMENTS, seen=None):
    """The fragment ids a player's body has: the tags of its fragment id file and its imports."""
    seen = set() if seen is None else seen
    if name in seen or name not in pak.namelist():
        return set()
    seen.add(name)
    root = ElementTree.fromstring(pak.read(name))
    found = {tag.get("name") for tag in root.iter("Tag") if tag.get("name")}
    for imported in root.iter("Import"):
        found |= player_fragments(pak, imported.get("filename", ""), seen)
    return found


def partner_of(fragment, fragments):
    for own, other in PARTNERS:
        if fragment.endswith(own):
            candidate = fragment[: -len(own)] + other
            if candidate in fragments:
                return candidate
    return ""


def animations(pak, tags, played, picked, player):
    rows = {}
    for row in ElementTree.fromstring(pak.read(ACTIONS)).iter("actor_anim_action"):
        body = row.get("actor_class_hash")
        if body not in (MALE, FEMALE):
            continue
        fragment = row.get("mn_fragment_id", "")
        if not fragment:
            continue
        key = (fragment, row.get("mn_tags", ""))
        known = rows.get(key)
        if known is None:
            flags = 0
            if row.get("isOneShot") == "true":
                flags |= ONE_SHOT
            if row.get("isfullbody") == "true":
                flags |= FULL_BODY
            if row.get("init_sec_align0", IDENTITY) != IDENTITY:
                flags |= SLAVE_BODY
            known = {
                "duration": round(float(row.get("animation_duration", "0")), 3),
                "flags": flags,
                "right": hand_of(tags, row.get("r_man_id")),
                "left": hand_of(tags, row.get("l_man_id")),
            }
            rows[key] = known
        known["flags"] |= FOR_MALE if body == MALE else FOR_FEMALE
        if fragment in player:
            known["flags"] |= FOR_PLAYER
    fragments = {fragment for fragment, _ in rows}
    catalog = []
    for (fragment, fragment_tags), row in sorted(rows.items(), key=lambda item: item[0]):
        sequence, phase = sequence_of(fragment, fragments)
        right, left = picked_hands(fragment, fragment_tags, played, picked)
        catalog.append(
            [
                fragment,
                fragment_tags,
                row["duration"],
                row["flags"],
                row["right"] or right,
                row["left"] or left,
                sequence,
                phase,
                partner_of(fragment, fragments),
                first_clip(clips_of(played, fragment, fragment_tags)),
            ]
        )
    return catalog


def props(pak, tags):
    found = {}
    for name in pak.namelist():
        if not re.fullmatch(r"Libs/Tables/item/item(__\w+)?\.xml", name):
            continue
        for tool in ElementTree.fromstring(pak.read(name)).iter("NPCTool"):
            kind = tool.get("ManipulationType", "0")
            model = tool.get("Model", "")
            item = tool.get("Name", "")
            if not item or not model or kind in ("0", "1") or kind not in tags:
                continue
            found[item] = [item, model, tags[kind]]
    return [found[item] for item in sorted(found)]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("game", help="the game's installation folder")
    args = parser.parse_args()
    with zipfile.ZipFile(Path(args.game) / "Data" / ANIMATIONS_PAK) as pak:
        played = fragment_animations(pak)
        picked = pickups(pak)
        player = player_fragments(pak)
    with zipfile.ZipFile(Path(args.game) / "Data" / "Tables.pak") as pak:
        tags = manipulation_tags(pak)
        catalog = {
            "animations": animations(pak, tags, played, picked, player),
            "props": props(pak, tags),
        }
    lines = ["{"]
    for index, (kind, rows) in enumerate(catalog.items()):
        lines.append(f'  "{kind}": [')
        lines.append(",\n".join("    " + json.dumps(row) for row in rows))
        lines.append("  ]" + ("," if index + 1 < len(catalog) else ""))
    lines.append("}")
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"{OUTPUT}: {len(catalog['animations'])} animations, {len(catalog['props'])} props")


if __name__ == "__main__":
    main()
