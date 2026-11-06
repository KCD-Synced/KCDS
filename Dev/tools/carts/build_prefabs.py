"""Builds the cart and wagon blueprints from a game installation.

Every prefab of Data/IPL_GameData.pak Prefabs/manmade/vehicles that is a cart or a wagon becomes a
blueprint: the meshes its objects show (GeomEntity Geometry, Brush Prefab; .cgf only), with their
parents resolved into the prefab's own frame (+y the front, z = 0 the ground), each with its
rotation and scale, the wheels marked so the game client rolls them.

- The root is the first part: the front axle (wagon_b_axel.cgf and its kin, which carry the reins'
  helpers whattach_cartAtt_*) or the cart's front part; the client hangs every other part on it.
- Every wagon is moved and turned so that its axle stands where wagon_b_covered's does: the
  horses, the seats and the axles the client measured on that wagon then fit them all.
- The chassis says how the client harnesses it: a wagon (on the b axle: two horses), or a cart
  (cart_b: one horse between its shafts). The body says where people sit, by its mesh: the
  covered b body (its long side benches), the open b body (its two shelves at the back), an a
  body, a c body (whose bed covers the axle's driver plank), or the cart. The body part's index
  names its entity for the server's interactions (kcds_cart_<id>_<index>). A loaded prefab (a
  mesh from outside the vehicles folder: sacks, hay, stones, crates) seats no passengers.
- A cart's loads (what it can carry for the server's Cart#load): the game's own loaded carts of
  Prefabs/exteriorDecoration (LOADS), their goods put on the cart as they lie on its one-mesh
  model there (cart_b.cgf, cart_a.cgf; cart_b.cgf shares its origin with cart_b_front_part.cgf).
- Left out: the wagon character prefab (wagon_b_messy_cdf), prefabs made only of other prefabs,
  the wheelbarrow, the one-mesh wagons (wagon_c_static_*), the collision helper
  (wagon_collision_triangle.cgf), nested prefabs, entities and particles.

    python Dev/tools/carts/build_prefabs.py "<game folder>" [--clang-format <clang-format>]

writes Client/runtime/carts/cart_prefabs.hpp (the parts and loads, for the client),
Backend/scripting/runtimes/js/carts/prefabs.json (the names and chassis, for the server) and
Backend/scripting/runtimes/js/carts/loads.json (each cart's loads, for the server).
"""

import argparse
import json
import math
import subprocess
import xml.etree.ElementTree as ElementTree
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HEADER = ROOT / "Client/runtime/carts/cart_prefabs.hpp"
LISTING = ROOT / "Backend/scripting/runtimes/js/carts/prefabs.json"
LOAD_LISTING = ROOT / "Backend/scripting/runtimes/js/carts/loads.json"
# The game's loaded carts: (cart, load name, prefab, the cart's mesh in it).
# (camp_supplies/cart_with_things is a camp scene around a cart_a, with a c wagon and logs: no load.)
LOADS = (
    ("cart_b", "goods", "carts_with_goods/cart_b2_with_goods_V1", "cart_b.cgf"),
    ("cart_b", "goods2", "carts_with_goods/cart_b2_with_goods_V2", "cart_b.cgf"),
    ("cart_b", "sacks", "carts_with_goods/cart_b2_with_sacks", "cart_b.cgf"),
    (
        "cart_a",
        "shadyGoods",
        "carts_with_goods/cart_a14_with_shady_goods",
        "cart_a.cgf",
    ),
    (
        "cart_a",
        "shadyGoods2",
        "carts_with_goods/cart_a14_with_shady_goods_V2",
        "cart_a.cgf",
    ),
    ("cart_a", "branches", "carts_with_goods/cart_with_branches", "cart_a.cgf"),
)
# A load's goods lie on the bed (higher than this above the cart's origin) within this distance of
# its middle, sideways (metres).
LOAD_LOWEST = 0.5
LOAD_HALF_WIDTH = 1.0
LOAD_FOLDER = "Prefabs/exteriorDecoration/"
FOLDER = "Prefabs/manmade/vehicles/"
LEFT_OUT = {
    "barrow_a_stones",
    "wagon_b_messy_cdf",
    "wagon_c_static_sacks_a",
    "wagon_c_static_sacks_b",
}
SKIPPED_MESHES = {"wagon_collision_triangle.cgf"}
ROOT_MESHES = (
    "wagon_b_axel.cgf",
    "wagon_b_axel_lowered.cgf",
    "wagon_b_covered_axel.cgf",
    "cart_b_front_part.cgf",
)


def numbers(text, count, default):
    if not text:
        return list(default)
    values = [float(value) for value in text.split(",")]
    return values if len(values) == count else list(default)


def multiply(a, b):
    """The quaternion a * b, both (w, x, y, z)."""
    aw, ax, ay, az = a
    bw, bx, by, bz = b
    return (
        aw * bw - ax * bx - ay * by - az * bz,
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
    )


def conjugate(q):
    return (q[0], -q[1], -q[2], -q[3])


def rotate(q, v):
    w, x, y, z = multiply(multiply(q, (0.0, v[0], v[1], v[2])), conjugate(q))
    return (x, y, z)


def normalized(q):
    length = math.sqrt(sum(value * value for value in q)) or 1.0
    return tuple(value / length for value in q)


def mesh_of(element):
    kind = element.get("Type")
    model = ""
    if kind == "GeomEntity":
        model = element.get("Geometry", "")
    elif kind == "Brush":
        model = element.get("Prefab", "")
    model = model.replace("\\", "/").lower()
    if not model.endswith(".cgf"):
        return ""
    if model.rsplit("/", 1)[-1] in SKIPPED_MESHES:
        return ""
    return model


def world_transforms(objects):
    """Each object's position and rotation in the prefab's frame, by id."""
    resolved = {}

    def resolve(object_id, depth=0):
        if object_id in resolved:
            return resolved[object_id]
        element = objects[object_id]
        position = numbers(element.get("Pos"), 3, (0.0, 0.0, 0.0))
        rotation = normalized(numbers(element.get("Rotate"), 4, (1.0, 0.0, 0.0, 0.0)))
        parent = element.get("Parent")
        if parent in objects and depth < 16:
            parent_position, parent_rotation = resolve(parent, depth + 1)
            moved = rotate(parent_rotation, position)
            position = [parent_position[i] + moved[i] for i in range(3)]
            rotation = normalized(multiply(parent_rotation, rotation))
        resolved[object_id] = (position, rotation)
        return resolved[object_id]

    for object_id in objects:
        resolve(object_id)
    return resolved


def prefab_parts(text):
    tree = ElementTree.fromstring(text)
    objects = {}
    for element in tree.iter("Object"):
        if element.get("Id"):
            objects[element.get("Id")] = element
    transforms = world_transforms(objects)
    parts = []
    for object_id, element in objects.items():
        model = mesh_of(element)
        if not model:
            continue
        position, rotation = transforms[object_id]
        scale = numbers(element.get("Scale"), 3, (1.0, 1.0, 1.0))
        parts.append(
            {
                "model": model,
                "position": position,
                "rotation": rotation,
                "scale": scale,
                "wheel": "wheel" in model.rsplit("/", 1)[-1],
            }
        )
    roots = [i for i, part in enumerate(parts) if part["model"].endswith(ROOT_MESHES)]
    if not roots:
        return None, None
    parts.insert(0, parts.pop(roots[0]))
    chassis = "cart" if parts[0]["model"].endswith("cart_b_front_part.cgf") else "wagon"
    return chassis, parts


def load_parts(text, base_mesh, root):
    """A loaded cart prefab's goods in the cart's own frame: placed as they lie on base_mesh
    there, with base_mesh where `root` (position, rotation) puts the cart's root. Only what lies
    on the cart or hangs on its sides is its load; the scene's goods on the ground are left out.
    """
    tree = ElementTree.fromstring(text)
    objects = {e.get("Id"): e for e in tree.iter("Object") if e.get("Id")}
    transforms = world_transforms(objects)
    base = None
    goods = []
    for object_id, element in objects.items():
        model = mesh_of(element)
        if not model:
            continue
        position, rotation = transforms[object_id]
        if model.endswith("/" + base_mesh) and base is None:
            base = (position, rotation)
            continue
        scale = numbers(element.get("Scale"), 3, (1.0, 1.0, 1.0))
        goods.append({"model": model, "position": position, "rotation": rotation, "scale": scale})
    if base is None:
        return []
    back = conjugate(base[1])
    placed = []
    for part in goods:
        offset = [part["position"][i] - base[0][i] for i in range(3)]
        local = rotate(back, offset)
        if local[2] < LOAD_LOWEST or abs(local[0]) > LOAD_HALF_WIDTH:
            continue
        local_rotation = multiply(back, part["rotation"])
        turned = rotate(root[1], local)
        placed.append(
            dict(
                part,
                position=[root[0][i] + turned[i] for i in range(3)],
                rotation=normalized(multiply(root[1], local_rotation)),
                wheel=False,
            )
        )
    return placed


def moved_to(parts, position, rotation):
    """The parts moved and turned as one so that the first stands at position and rotation."""
    turn = normalized(multiply(rotation, conjugate(parts[0]["rotation"])))
    origin = parts[0]["position"]
    moved = []
    for part in parts:
        offset = [part["position"][i] - origin[i] for i in range(3)]
        turned = rotate(turn, offset)
        moved.append(
            dict(
                part,
                position=[position[i] + turned[i] for i in range(3)],
                rotation=normalized(multiply(turn, part["rotation"])),
            )
        )
    return moved


BODIES = (
    ("wagon_b_covered_body", "coveredB"),
    ("wagon_b_body", "openB"),
    ("wagon_a", "a"),
    ("wagon_c", "c"),
)


def body_of(chassis, parts):
    """The body class and the body part's index (the cart's front part for a cart)."""
    if chassis == "cart":
        return "cart", 0
    for index, part in enumerate(parts):
        mesh = part["model"].rsplit("/", 1)[-1]
        if "_body" not in mesh:
            continue
        for prefix, body in BODIES:
            if mesh.startswith(prefix):
                return body, index
    return "openB", 0


def is_loaded(parts):
    return any("/vehicles/" not in part["model"] for part in parts)


def json_row(row):
    """A listing row as prettier writes it: on one line when it fits in 100 columns."""
    fields = [f"{json.dumps(key)}: {json.dumps(value)}" for key, value in row.items()]
    line = "  { " + ", ".join(fields) + " }"
    if len(line) + 1 <= 100:
        return line
    return "  {\n" + ",\n".join("    " + field for field in fields) + "\n  }"


def number(value):
    text = f"{value:.5f}".rstrip("0").rstrip(".")
    if text in ("-0", ""):
        text = "0"
    if "." not in text:
        text += ".0"
    return text + "f"


def triple(values):
    return "{" + ", ".join(number(value) for value in values) + "}"


def part_line(part):
    w, x, y, z = part["rotation"]
    rotation = "{" + ", ".join(number(value) for value in (x, y, z, w)) + "}"
    wheel = "true" if part["wheel"] else "false"
    return (
        f'    {{"{part["model"]}",\n'
        f"     {triple(part['position'])},\n"
        f"     {rotation},\n"
        f"     {triple(part['scale'])},\n"
        f"     {wheel}}},"
    )


def write_header(prefabs, loads):
    lines = [
        "// The game's carts and wagons: the prefabs of Data/IPL_GameData.pak",
        "// Prefabs/manmade/vehicles, their meshes in the prefab's frame (+y the front, z = 0 the",
        "// ground; rotations x, y, z, w), the root (the front axle or front part) first.",
        "// Generated by Dev/tools/carts/build_prefabs.py from the game installation; do not edit.",
        "#pragma once",
        "",
        "#include <cstddef>",
        "",
        "namespace kcds::runtime::carts::prefabs {",
        "",
        "enum class Chassis { Wagon, Cart };",
        "",
        "enum class Body { CoveredB, OpenB, A, C, Cart };",
        "",
        "struct Part {",
        "  const char* model;",
        "  float position[3];",
        "  float rotation[4];",
        "  float scale[3];",
        "  bool wheel;",
        "};",
        "",
        "struct Load {",
        "  const char* cart;",
        "  const char* name;",
        "  const Part* parts;",
        "  std::size_t part_count;",
        "};",
        "",
        "struct Prefab {",
        "  const char* name;",
        "  Chassis chassis;",
        "  Body body;",
        "  std::size_t body_part;",
        "  bool loaded;",
        "  const Part* parts;",
        "  std::size_t part_count;",
        "};",
        "",
    ]
    for name, chassis, parts in prefabs:
        lines.append(f"inline constexpr Part k_{name}[] = {{")
        lines.extend(part_line(part) for part in parts)
        lines.append("};")
        lines.append("")
    lines.append("inline constexpr Prefab kPrefabs[] = {")
    for name, chassis, parts in prefabs:
        kind = "Chassis::Cart" if chassis == "cart" else "Chassis::Wagon"
        body, index = body_of(chassis, parts)
        kinds = {
            "coveredB": "CoveredB",
            "openB": "OpenB",
            "a": "A",
            "c": "C",
            "cart": "Cart",
        }
        loaded = "true" if is_loaded(parts) else "false"
        lines.append(
            f'    {{"{name}", {kind}, Body::{kinds[body]}, {index}, {loaded}, k_{name}, '
            f"{len(parts)}}},"
        )
    lines.append("};")
    lines.append("")
    for cart, name, parts in loads:
        lines.append(f"inline constexpr Part k_{cart}_{name}[] = {{")
        lines.extend(part_line(part) for part in parts)
        lines.append("};")
        lines.append("")
    lines.append("inline constexpr Load kLoads[] = {")
    for cart, name, parts in loads:
        lines.append(f'    {{"{cart}", "{name}", k_{cart}_{name}, {len(parts)}}},')
    lines.append("};")
    lines.append("")
    lines.append("}  // namespace kcds::runtime::carts::prefabs")
    HEADER.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("game", help="the game folder (with Data/IPL_GameData.pak)")
    parser.add_argument("--clang-format", help="formats the header with this clang-format")
    arguments = parser.parse_args()
    game = Path(arguments.game)
    prefabs = []
    with zipfile.ZipFile(game / "Data" / "IPL_GameData.pak") as pak:
        for path in sorted(pak.namelist()):
            if not (path.startswith(FOLDER) and path.endswith(".xml")):
                continue
            name = path[len(FOLDER) : -len(".xml")]
            if "/" in name or name in LEFT_OUT:
                continue
            chassis, parts = prefab_parts(pak.read(path))
            if parts:
                prefabs.append((name, chassis, parts))
    reference = next(parts for name, _, parts in prefabs if name == "wagon_b_covered")
    anchor = (reference[0]["position"], reference[0]["rotation"])
    prefabs = [
        (name, chassis, moved_to(parts, *anchor) if chassis == "wagon" else parts)
        for name, chassis, parts in prefabs
    ]
    roots = {name: (parts[0]["position"], parts[0]["rotation"]) for name, _, parts in prefabs}
    roots["cart_a"] = ([0.0, 0.0, 0.0], (1.0, 0.0, 0.0, 0.0))
    loads = []
    with zipfile.ZipFile(game / "Data" / "IPL_GameData.pak") as pak:
        for cart, name, prefab, base_mesh in LOADS:
            parts = load_parts(pak.read(LOAD_FOLDER + prefab + ".xml"), base_mesh, roots[cart])
            if parts:
                loads.append((cart, name, parts))
    write_header(prefabs, loads)
    if arguments.clang_format:
        subprocess.run([arguments.clang_format, "-i", str(HEADER)], check=True)
    listing = []
    for name, chassis, parts in prefabs:
        body, index = body_of(chassis, parts)
        listing.append(
            {
                "name": name,
                "chassis": chassis,
                "body": body,
                "bodyPart": index,
                "loaded": is_loaded(parts),
            }
        )
    rows = [json_row(row) for row in listing]
    LISTING.write_text("[\n" + ",\n".join(rows) + "\n]\n", encoding="utf-8", newline="\n")
    by_cart = {}
    for cart, name, _ in loads:
        by_cart.setdefault(cart, []).append(name)
    rows = [f"  {json.dumps(cart)}: {json.dumps(names)}" for cart, names in by_cart.items()]
    LOAD_LISTING.write_text("{\n" + ",\n".join(rows) + "\n}\n", encoding="utf-8", newline="\n")
    print(f"{len(prefabs)} prefabs: {', '.join(name for name, _, _ in prefabs)}")
    print(f"{len(loads)} loads: {', '.join(cart + '/' + name for cart, name, _ in loads)}")


if __name__ == "__main__":
    main()
