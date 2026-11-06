"""Builds the recipe catalog from a game installation.

The catalog is what Recipe.all and Recipe.get answer and what Player.learnRecipe, forgetRecipe
and knowsRecipe go by: every smithing and alchemy recipe of the game, the perk (or perks) a
player who knows it owns, what it takes and what it makes.

- smithing: Libs/Tables/minigame/BlacksmithRecipes.xml. A recipe's id is the game's own
  ("r_horseshoe"); the perk that makes it known is its PerkId.
- alchemy: Libs/Tables/minigame/AlchemyRecipe.xml, numbered. A recipe's id here is its name
  without the "recipe_" prefix and the "_name" suffix ("potion_hair_o_dog"); the perks that make
  it known are those Libs/Tables/rpg/perk_recipe.xml gives its number.
- smithingFinishingXp: the craftsmanship XP the game gives for a finished workpiece before the
  player's own multipliers, Libs/Tables/rpg/rpg_param.xml's BlacksmithingFinishingXP (smithing
  recipes carry no XP of their own; the hammering's effectivity XP comes on top while working).

    python Dev/tools/recipes/build_catalog.py "<game folder>"

writes Backend/scripting/runtimes/js/recipes/catalog.json.
"""

import argparse
import json
import xml.etree.ElementTree as ElementTree
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/recipes/catalog.json"
SMITHING = "Libs/Tables/minigame/BlacksmithRecipes.xml"
ALCHEMY = "Libs/Tables/minigame/AlchemyRecipe.xml"
PERK_RECIPES = "Libs/Tables/rpg/perk_recipe.xml"
RPG_PARAMS = "Libs/Tables/rpg/rpg_param.xml"


def read_table(pak, name):
    return ElementTree.fromstring(pak.read(name))


def rpg_param(pak, key):
    for row in read_table(pak, RPG_PARAMS).iter("rpg_param"):
        if row.get("rpg_param_key") == key:
            return float(row.get("rpg_param_value"))
    raise SystemExit(f"{RPG_PARAMS} has no {key}")


def smithing_recipes(pak):
    recipes = []
    for recipe in read_table(pak, SMITHING).iter("BlacksmithRecipe"):
        ui_name = recipe.find("UIName")
        recipes.append(
            {
                "id": recipe.get("Id"),
                "perks": [recipe.get("PerkId").lower()],
                "uiName": ui_name.get("StringName") if ui_name is not None else "",
                "category": recipe.get("Category", ""),
                "workpiece": recipe.get("WorkpieceId", ""),
                "minSkill": int(recipe.get("MinSkillLevel", "0")),
                "dlcId": int(recipe.get("DlcId", "0")),
                "ingredients": [
                    [item.get("ItemClassId").lower(), int(item.get("Amount", "1"))]
                    for item in recipe.iter("BlacksmithRecipeIngredient")
                ],
                "products": [
                    [
                        item.get("ItemClassId").lower(),
                        int(item.get("MinimalQuality", "0")),
                    ]
                    for item in recipe.iter("BlacksmithRecipeProduct")
                ],
            }
        )
    return recipes


def alchemy_id(ui_name):
    name = ui_name
    if name.startswith("recipe_"):
        name = name[len("recipe_") :]
    if name.endswith("_name"):
        name = name[: -len("_name")]
    return name


def alchemy_recipes(pak):
    perks = {}
    for row in read_table(pak, PERK_RECIPES).iter("perk_recipe"):
        perks.setdefault(row.get("recipe_id"), []).append(row.get("perk_id").lower())
    recipes = []
    for recipe in read_table(pak, ALCHEMY).iter("AlchemyRecipe"):
        number = recipe.get("RecipeId")
        ui_name = recipe.get("UIName", "")
        recipes.append(
            {
                "id": alchemy_id(ui_name),
                "number": int(number),
                "perks": sorted(perks.get(number, [])),
                "uiName": ui_name,
                "base": recipe.get("BaseMaterial", ""),
                "xp": int(recipe.get("BaseQualityRewardXP", "0")),
                "ingredients": [
                    [
                        item.get("IngredientItemId").lower(),
                        int(item.get("Quantity", "1")),
                    ]
                    for item in recipe.iter("AlchemyIngredient")
                ],
                "products": [
                    [
                        item.get("ProductItemId").lower(),
                        int(item.get("Quality", "0")),
                        int(item.get("MaxYield", "1")),
                    ]
                    for item in recipe.iter("AlchemyProduct")
                ],
            }
        )
    return recipes


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("game", help="the game's installation folder")
    args = parser.parse_args()
    with zipfile.ZipFile(Path(args.game) / "Data" / "Tables.pak") as pak:
        catalog = {"smithing": smithing_recipes(pak), "alchemy": alchemy_recipes(pak)}
        finishing_xp = rpg_param(pak, "BlacksmithingFinishingXP")
    ids = [recipe["id"] for kind in catalog.values() for recipe in kind]
    duplicates = sorted({name for name in ids if ids.count(name) > 1})
    if duplicates:
        raise SystemExit(f"recipe ids are not unique: {duplicates}")
    # One recipe a line, as the progression catalog is written.
    lines = ["{", f'  "smithingFinishingXp": {int(finishing_xp)},']
    for index, (kind, recipes) in enumerate(catalog.items()):
        lines.append(f'  "{kind}": [')
        lines.append(",\n".join("    " + json.dumps(recipe) for recipe in recipes))
        lines.append("  ]" + ("," if index + 1 < len(catalog) else ""))
    lines.append("}")
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"{OUTPUT}: {len(catalog['smithing'])} smithing, {len(catalog['alchemy'])} alchemy")


if __name__ == "__main__":
    main()
