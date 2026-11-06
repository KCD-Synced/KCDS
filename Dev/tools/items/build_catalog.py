"""Builds the item catalog from a game installation.

The catalog is what Inventory.classes answers and what the server checks every inventory
operation against: the game's item classes (Libs/Tables/item/item*.xml) that an inventory can
hold, each with

- id: the class GUID, lower case;
- name: the table name (Inventory.add takes it as well as the GUID);
- nameKey: the native localization key of its display name;
- category: the table element, camel case (meleeWeapon, armor, food, ...);
- maximumQuality: the best quality the class is made in (1 for classes without qualities);
- quest: only a quest hands it out;
- price, weight: per unit;
- stackable: units of it share one native item (food, herbs, materials, ointments, poisons,
  ammunition, money); every unit of the others is an item of its own;
- qualityBands: its health carries its quality (gear: weapons, armour, helmets, hoods, ammunition,
  belts and pouches). A class whose table gives no MaxQuality has none.
- model: the mesh it lies on the ground as: its Model, else (a worn class) its
  clothing's nearest DropModel in CharacterComponent.xml; under objects/, '' when neither is given;
- weaponClass: a weapon's class from Libs/Tables/item/weapon_class.xml, camel case
  (dagger, sword, sabre, axe, longsword, mace, flail, halberd, shield, huntingSword, bow,
  crossbowLight, ...), null for anything else; attackModSlash, attackModStab, attackModSmash: its
  AttackModSlash, AttackModStab and AttackModSmash (0 for anything else).

The game's own NPC tools (props NPCs hold) are not inventory items and are left out, as are the
test and deprecated tables.

It also carries the game's own outfits (Libs/Tables/item/clothing_preset.xml): what
Player.outfitSets lists and player.setOutfit dresses a body in, each with its GUID, name, gender
(male, female, or any) and the item classes it is made of, as positions in the class list. The
test presets are left out, and so are classes the catalog does not carry.

And what eating or drinking a food class does by itself (Consumption.baseEffects): the
game's food type (1 is a potion), its buff and its health, energy, nourishment and alcohol benefits
(HealthBenefit, RefreshBenefit, NutritionBenefit, AlcoholContent), before condition, spoilage, perks
and the soul's modifiers.

And where a worn class goes on the body: the equipment slot of an armour, helmet or
hood class (slot, null for any other class) and whether its legs cover the feet (coversFeet), with
the game's slot table (Libs/Tables/item/equipment_slot.xml) next to them. A class names its
clothing (Clothing="HoseJoined01_m03"); the clothing's armour type, from the nearest ArmorType
of itself or of the clothing it derives from in Libs/Tables/Character/CharacterComponent.xml
(the Male definitions first, then the Female ones), decides the slot. Armour types no slot takes
(spectacles, bells, scabbards) leave a class without one; the tool lists them. A horse's tack
is the Horse component's clothing, in the horse_saddle, horse_torso, horse_head and horse_shoe
slots.

    python Dev/tools/items/build_catalog.py "<game folder>"

writes Backend/scripting/runtimes/js/inventory/catalog.json as columns and rows, to keep it small.
"""

import argparse
import json
import xml.etree.ElementTree as ElementTree
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/inventory/catalog.json"
TABLES = "Libs/Tables/item/"
SKIPPED_TABLES = {"item__test.xml", "item__autotests.xml", "item__deprecated.xml"}
SKIPPED_ELEMENTS = {"NPCTool"}
STACKABLE_ELEMENTS = {"Ammo", "Money"}
COLUMNS = [
    "id",
    "name",
    "nameKey",
    "category",
    "maximumQuality",
    "quest",
    "price",
    "weight",
    "stackable",
    "qualityBands",
    "slot",
    "coversFeet",
    "model",
    "weaponClass",
    "attackModSlash",
    "attackModStab",
    "attackModSmash",
]
WEAPON_ELEMENTS = {"MeleeWeapon", "MissileWeapon"}
SLOT_COLUMNS = ["name", "equipmentPart", "bodyLayer", "requiresFilled", "requiresEmpty"]

EQUIPMENT_SLOTS = TABLES + "equipment_slot.xml"
CHARACTER_COMPONENTS = "Libs/Tables/Character/CharacterComponent.xml"
# Clothing is looked up in these character components, in this order: a horse's tack is the
# Horse component's.
CLOTHING_COMPONENTS = ["Male", "Female", "Horse"]
# The table elements of the classes that are dressed on the body: clothing and armour, and the
# quick slot containers (belts and pouches, slots `belt` and `pouch`; one of each, live). Weapons
# are held, so they have no slot.
WORN_ELEMENTS = {"Armor", "Helmet", "Hood", "QuickSlotContainer"}
# The armour types of the slots whose table row lists none.
UNLISTED_ARMOR_TYPES = {
    "Hood": "head_hood",
    "CollarPadded": "collar",
    "CollarMail": "collar",
    "Coat": "body_coat",
    "Waffenrock": "body_coat",
    "Habit": "body_coat",
    "Gloves": "gloves",
    "Gauntlets": "gloves",
    "Shoes": "boot",
    "BootsAnkle": "boot",
    "BootsKnee": "boot",
    "F_Shoes": "boot",
    "Belt": "belt",
    "Pouch": "pouch",
    "Ring": "ring",
    "Necklace": "necklace",
    # A horse's tack: the saddle, the caparison or harness on it, the bridle or
    # chanfron, the shoes.
    "Saddle": "horse_saddle",
    "Caparison": "horse_torso",
    "HorsePadded": "horse_torso",
    "Bridle": "horse_head",
    "HorsePlate": "horse_head",
    "HorseShoe": "horse_shoe",
}
# The armour archetypes of plate legs that cover the feet: boots cannot be worn with them.
FEET_ARCHETYPES = {"57", "58"}


def camel(tag):
    return tag[0].lower() + tag[1:]


def number(text, fallback=0):
    try:
        value = float(text)
    except (TypeError, ValueError):
        return fallback
    return int(value) if value.is_integer() else value


def tables(pak):
    for name in sorted(pak.namelist()):
        if not name.startswith(TABLES):
            continue
        file = name[len(TABLES) :]
        if (
            file.startswith("item")
            and file.endswith(".xml")
            and "_" not in file.replace("item__", "")
        ):
            if file not in SKIPPED_TABLES:
                yield name


def single_slot(name):
    """A slot a table row requires, or None; the catalog holds one name per requirement."""
    if name and len(name.split()) > 1:
        raise ValueError(f"equipment_slot.xml requires more than one slot: {name}")
    return name or None


def equipment_slots(pak):
    """The game's slot rows, as SLOT_COLUMNS, and the slot of each armour type."""
    table = ElementTree.fromstring(pak.read(EQUIPMENT_SLOTS))
    rows = []
    slot_of_type = {}
    for slot in table.iter("EquipmentSlot"):
        name = slot.get("Name")
        rows.append(
            [
                name,
                int(slot.get("EquipmentPartId")),
                int(slot.get("BodyLayerTypeId")),
                single_slot(slot.get("RequiresFilledSlot")),
                single_slot(slot.get("RequiresEmptySlot")),
            ]
        )
        for armor_type in slot.get("ArmorTypes", "").split():
            slot_of_type[armor_type] = name
    names = {row[0] for row in rows}
    for armor_type, name in UNLISTED_ARMOR_TYPES.items():
        if name not in names or armor_type in slot_of_type:
            raise ValueError(f"equipment_slot.xml no longer matches the slot of {armor_type}")
        slot_of_type[armor_type] = name
    return rows, slot_of_type


def collect_clothing(element, armor_type, archetype, drop_model, found):
    """Adds each clothing under element with its nearest armour type, archetype and drop model."""
    for child in element:
        child_type = armor_type
        child_archetype = archetype
        child_drop = drop_model
        if child.tag == "Clothing":
            child_type = child.get("ArmorType", armor_type)
            child_archetype = child.get("ArmorArchetypeId", archetype)
            child_drop = child.get("DropModel", drop_model)
            if child.get("Name"):
                found.setdefault(child.get("Name"), (child_type, child_archetype, child_drop))
        collect_clothing(child, child_type, child_archetype, child_drop, found)


def clothing_definitions(pak):
    """Clothing name -> (armour type, armour archetype, drop model), each None when never given."""
    table = ElementTree.fromstring(pak.read(CHARACTER_COMPONENTS))
    components = {
        component.get("Name"): component for component in table.find("CharacterComponents")
    }
    found = {}
    for name in CLOTHING_COMPONENTS:
        collect_clothing(components[name], None, None, None, found)
    return found


def worn_slot(element, clothing, slot_of_type):
    """(slot, covers feet, why there is no slot) of one item row of the tables."""
    if element.tag not in WORN_ELEMENTS:
        return None, False, ""
    name = element.attrib.get("Clothing")
    if name is None:
        return None, False, "no Clothing attribute"
    if name not in clothing:
        return None, False, f"clothing not defined under {' or '.join(CLOTHING_COMPONENTS)}"
    armor_type, archetype, _ = clothing[name]
    if armor_type is None:
        return None, False, "clothing without an armour type"
    slot = slot_of_type.get(armor_type)
    if slot is None:
        return None, False, f"armour type {armor_type}"
    return slot, archetype in FEET_ARCHETYPES, ""


def mesh_path(path):
    """A table's mesh path as the game opens it: under objects/, with forward slashes, and .cgf
    when the table leaves the extension out (food: "manmade/food/food/fruits/apple")."""
    path = path.replace("\\", "/").lstrip("/")
    if "." not in path.rsplit("/", 1)[-1]:
        path += ".cgf"
    if path.lower().startswith("objects/"):
        return path
    return "objects/" + path


def ground_model(element, clothing):
    """The mesh a class lies on the ground as: its own Model, else (a worn class) its
    clothing's nearest DropModel; '' when the tables give it none (the game draws nothing)."""
    model = element.attrib.get("Model", "")
    if model:
        return mesh_path(model)
    if element.tag in WORN_ELEMENTS:
        name = element.attrib.get("Clothing")
        drop_model = clothing.get(name, (None, None, None))[2] if name else None
        if drop_model:
            return mesh_path(drop_model)
    return ""


def weapon_classes(pak):
    """The game's weapon classes: id -> name in camel case (crossbow_light -> crossbowLight)."""
    root = ElementTree.fromstring(pak.read(TABLES + "weapon_class.xml"))
    names = {}
    for element in root.iter():
        if element.tag in ("MeleeWeaponClass", "MissileWeaponClass") and "id" in element.attrib:
            words = element.attrib["name"].split("_")
            names[element.attrib["id"]] = words[0] + "".join(word.title() for word in words[1:])
    names.pop("-1", None)
    return names


def classes(pak, clothing, slot_of_type):
    """The sorted class rows, and why each worn class without a slot has none: guid -> reason."""
    found = {}
    weapon_class_names = weapon_classes(pak)
    unslotted = {}
    for table in tables(pak):
        for element in ElementTree.fromstring(pak.read(table)).iter():
            attributes = element.attrib
            if (
                element.tag in SKIPPED_ELEMENTS
                or "Id" not in attributes
                or "Name" not in attributes
            ):
                continue
            guid = attributes["Id"].lower()
            maximum_quality = int(number(attributes.get("MaxQuality"), 1)) or 1
            slot, covers_feet, why_not = worn_slot(element, clothing, slot_of_type)
            unslotted.pop(guid, None)
            if why_not:
                unslotted[guid] = why_not
            found[guid] = [
                guid,
                attributes["Name"],
                attributes.get("UIName", ""),
                camel(element.tag),
                maximum_quality,
                attributes.get("IsQuestItem") == "true",
                number(attributes.get("Price")),
                number(attributes.get("Weight")),
                attributes.get("IsDivisible") == "true" or element.tag in STACKABLE_ELEMENTS,
                "MaxQuality" in attributes,
                slot,
                covers_feet,
                ground_model(element, clothing),
                (
                    weapon_class_names.get(attributes.get("Class"))
                    if element.tag in WEAPON_ELEMENTS
                    else None
                ),
                number(attributes.get("AttackModSlash")) if element.tag in WEAPON_ELEMENTS else 0,
                number(attributes.get("AttackModStab")) if element.tag in WEAPON_ELEMENTS else 0,
                number(attributes.get("AttackModSmash")) if element.tag in WEAPON_ELEMENTS else 0,
            ]
    return sorted(found.values(), key=lambda row: row[1].lower()), unslotted


def print_unslotted(rows, unslotted):
    """Lists the worn classes that got no slot, by reason, for a human to check."""
    by_reason = {}
    for row in rows:
        if row[0] in unslotted:
            by_reason.setdefault(unslotted[row[0]], []).append(row[1])
    slotted = sum(1 for row in rows if row[COLUMNS.index("slot")] is not None)
    print(f"{slotted} classes have a slot; worn classes left without one:")
    for reason, names in sorted(by_reason.items()):
        examples = ", ".join(names[:5])
        print(f"  {reason}: {len(names)} classes, e.g. {examples}")


FOOD_COLUMNS = ["item", "type", "buff", "health", "energy", "nourishment", "alcohol"]


def foods(pak, position):
    found = []
    for table in tables(pak):
        for element in ElementTree.fromstring(pak.read(table)).iter("Food"):
            attributes = element.attrib
            guid = attributes.get("Id", "").lower()
            if guid not in position:
                continue
            found.append(
                [
                    position[guid],
                    int(number(attributes.get("Type"), 0)),
                    attributes.get("BuffDefinitionId", "").lower() or None,
                    number(attributes.get("HealthBenefit")),
                    number(attributes.get("RefreshBenefit")),
                    number(attributes.get("NutritionBenefit")),
                    number(attributes.get("AlcoholContent")),
                ]
            )
    return sorted(found)


GENDERS = {"Male": "male", "Female": "female"}


def outfits(pak, position):
    found = []
    table = ElementTree.fromstring(pak.read(TABLES + "clothing_preset.xml"))
    for preset in table.iter("clothing_preset"):
        name = preset.get("clothing_preset_name", "")
        if not name or name.startswith("test_"):
            continue
        items = [guid.text.lower() for guid in preset.iter("Guid") if guid.text]
        items = [position[item] for item in items if item in position]
        found.append(
            [
                preset.get("clothing_preset_id", "").lower(),
                name,
                GENDERS.get(preset.get("gender"), "any"),
                items,
            ]
        )
    return sorted(found, key=lambda row: row[1].lower())


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("game", type=Path, help="the game's installation folder")
    args = parser.parse_args()
    with zipfile.ZipFile(args.game / "Data" / "Tables.pak") as pak:
        slots, slot_of_type = equipment_slots(pak)
        rows, unslotted = classes(pak, clothing_definitions(pak), slot_of_type)
        position = {row[0]: index for index, row in enumerate(rows)}
        presets = outfits(pak, position)
        food = foods(pak, position)
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    catalog = {
        "columns": COLUMNS,
        "classes": rows,
        "outfitColumns": ["id", "name", "gender", "items"],
        "outfits": presets,
        "foodColumns": FOOD_COLUMNS,
        "foods": food,
        "slotColumns": SLOT_COLUMNS,
        "slots": slots,
    }
    OUTPUT.write_text(json.dumps(catalog, separators=(",", ":")) + "\n", encoding="utf-8")
    print(
        f"{len(rows)} item classes, {len(presets)} outfits, {len(food)} foods, "
        f"{len(slots)} equipment slots -> {OUTPUT.relative_to(ROOT)}"
    )
    print_unslotted(rows, unslotted)


if __name__ == "__main__":
    main()
