"""Simple props for the Asset/Animation Studio: a box, a cylinder, a sphere or a
plane as the game's own static model (kcds_cgf.py), written where the studio shows your meshes
from (%LOCALAPPDATA%\\KCDS\\studio\\meshes), so a prop needs no Blender.

    python studio_shapes.py <name> <json> [<out folder>]

<json> is { shape, size, radius, height, sides, rings, colour, surface }:

  box       size [width (x), depth (y), height (z)] metres, 0.5 each at first
  cylinder  radius (0.25) and height (0.5), sides around (24)
  sphere    radius (0.25), rings from top to bottom (12), sides around (24)
  plane     size [width (x), depth (y)] metres (1 each), facing up

Every shape stands on the ground (z 0) about its middle (x 0, y 0). colour is [r, g, b] from 0 to
1 (0.8 grey at first), surface one of SURFACES (how it sounds and what it does when hit, the
game's Libs/MaterialEffects/SurfaceTypes.xml; mat_wood at first). The uv of each side runs 0 to 1
(a texture is put on it in the studio, studio_texture, with its tiling). Writes <name>.cgf and
<name>.mtl and prints one JSON line: { name, shape, vertices, triangles, files }.
"""

import json
import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "blender"))

import kcds_cgf  # noqa: E402

SHAPES = ("box", "cylinder", "sphere", "plane")
# The game's surface types that suit a prop (of the 73 in its SurfaceTypes.xml).
SURFACES = (
    "mat_wood",
    "mat_wood_soft",
    "mat_metal",
    "mat_rock",
    "mat_plaster",
    "mat_ceramics",
    "mat_glass",
    "mat_fabric",
    "mat_leather",
    "mat_sack",
    "mat_thatch",
    "mat_wicker",
    "mat_soil",
    "mat_default",
)
SMALLEST = 0.01
LARGEST = 50
SIDES = (3, 128)
RINGS = (2, 64)


def _cross(a, b):
    return (
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    )


class _Builder:
    """One mesh of one material. A vertex's tangent runs where u grows, its bitangent where v
    falls (the game's v runs down), and tangent x bitangent is its normal (kcds_cgf.cube).
    Triangles go counter-clockwise seen from their front."""

    def __init__(self, material):
        self.mesh = kcds_cgf.Mesh()
        self.material = material

    def vertex(self, position, uv, tangent, bitangent):
        self.mesh.positions.append(tuple(position))
        self.mesh.uvs.append(tuple(uv))
        self.mesh.tangents.append((tuple(tangent), tuple(bitangent), 1))
        return len(self.mesh.positions) - 1

    def triangle(self, a, b, c):
        self.mesh.indices += [a, b, c]

    def quad(self, low_left, low_right, high_right, high_left):
        self.triangle(low_left, low_right, high_right)
        self.triangle(low_left, high_right, high_left)

    def finish(self):
        mesh = self.mesh
        mesh.subsets.append(kcds_cgf.Subset(0, len(mesh.indices), 0, len(mesh.positions), 0))
        mesh.materials.append(self.material)
        mesh.physicalize.append(kcds_cgf.PHYSICALIZE_DEFAULT)
        kcds_cgf.check(mesh)
        return mesh


def box(width=0.5, depth=0.5, height=0.5, material="kcds_box"):
    """A box standing on the ground: six flat sides, each its own four vertices."""
    builder = _Builder(material)
    half = (width / 2, depth / 2, height / 2)
    # Each side: its normal and the way its u grows.
    sides = [
        ((1, 0, 0), (0, 1, 0)),
        ((-1, 0, 0), (0, -1, 0)),
        ((0, 1, 0), (-1, 0, 0)),
        ((0, -1, 0), (1, 0, 0)),
        ((0, 0, 1), (1, 0, 0)),
        ((0, 0, -1), (-1, 0, 0)),
    ]
    for normal, across in sides:
        up = _cross(normal, across)
        corners = []
        for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            point = []
            for axis in range(3):
                offset = normal[axis] + across[axis] * su + up[axis] * sv
                point.append(offset * half[axis] + half[2] * (axis == 2))
            uv = ((su + 1) / 2, 1 - (sv + 1) / 2)
            corners.append(builder.vertex(point, uv, across, up))
        builder.quad(*corners)
    return builder.finish()


def cylinder(radius=0.25, height=0.5, sides=24, material="kcds_cylinder"):
    """A cylinder standing on the ground: a round side (its texture once around) and two flat
    round ends."""
    builder = _Builder(material)
    ring_low = []
    ring_high = []
    for step in range(sides + 1):
        angle = 2 * math.pi * step / sides
        cos, sin = math.cos(angle), math.sin(angle)
        tangent = (-sin, cos, 0)
        u = step / sides
        ring_low.append(builder.vertex((radius * cos, radius * sin, 0), (u, 1), tangent, (0, 0, 1)))
        ring_high.append(
            builder.vertex((radius * cos, radius * sin, height), (u, 0), tangent, (0, 0, 1))
        )
    for step in range(sides):
        builder.quad(ring_low[step], ring_low[step + 1], ring_high[step + 1], ring_high[step])
    # The ends: a fan round a middle vertex, the picture seen from above (top) or below (bottom).
    for z, bitangent, facing_up in ((height, (0, 1, 0), True), (0, (0, -1, 0), False)):
        flip = -1 if facing_up else 1
        middle = builder.vertex((0, 0, z), (0.5, 0.5), (1, 0, 0), bitangent)
        rim = []
        for step in range(sides):
            angle = 2 * math.pi * step / sides
            cos, sin = math.cos(angle), math.sin(angle)
            uv = (0.5 + 0.5 * cos, 0.5 + 0.5 * sin * flip)
            rim.append(builder.vertex((radius * cos, radius * sin, z), uv, (1, 0, 0), bitangent))
        for step in range(sides):
            following = rim[(step + 1) % sides]
            if facing_up:
                builder.triangle(middle, rim[step], following)
            else:
                builder.triangle(middle, following, rim[step])
    return builder.finish()


def sphere(radius=0.25, rings=12, sides=24, material="kcds_sphere"):
    """A sphere resting on the ground: rings from the top to the bottom, its texture once
    around (u) and once from top to bottom (v)."""
    builder = _Builder(material)
    grid = []
    for ring in range(rings + 1):
        down = math.pi * ring / rings
        row = []
        for step in range(sides + 1):
            around = 2 * math.pi * step / sides
            normal = (
                math.sin(down) * math.cos(around),
                math.sin(down) * math.sin(around),
                math.cos(down),
            )
            tangent = (-math.sin(around), math.cos(around), 0)
            bitangent = _cross(normal, tangent)
            position = (radius * normal[0], radius * normal[1], radius + radius * normal[2])
            uv = (step / sides, ring / rings)
            row.append(builder.vertex(position, uv, tangent, bitangent))
        grid.append(row)
    for ring in range(rings):
        high = grid[ring]
        low = grid[ring + 1]
        for step in range(sides):
            if ring > 0:
                # Below the top: the upper triangle (at the top its two upper corners are one).
                builder.triangle(low[step], high[step + 1], high[step])
            if ring < rings - 1:
                # Above the bottom: the lower one.
                builder.triangle(low[step], low[step + 1], high[step + 1])
    return builder.finish()


def plane(width=1.0, depth=1.0, material="kcds_plane"):
    """A flat rectangle on the ground facing up (seen from above only)."""
    builder = _Builder(material)
    corners = []
    for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
        point = (su * width / 2, sv * depth / 2, 0)
        uv = ((su + 1) / 2, 1 - (sv + 1) / 2)
        corners.append(builder.vertex(point, uv, (1, 0, 0), (0, 1, 0)))
    builder.quad(*corners)
    return builder.finish()


def _length(options, key, default):
    value = options.get(key, default)
    if not isinstance(value, (int, float)) or not SMALLEST <= value <= LARGEST:
        raise ValueError(f"{key} is from {SMALLEST} to {LARGEST} metres")
    return float(value)


def _count(options, key, default, limits):
    value = options.get(key, default)
    if not isinstance(value, int) or not limits[0] <= value <= limits[1]:
        raise ValueError(f"{key} is a whole number from {limits[0]} to {limits[1]}")
    return value


def _size(options, count, default):
    size = options.get("size", [default] * count)
    if not isinstance(size, list) or len(size) != count:
        raise ValueError(f"size is {count} lengths in metres")
    return [_length({"part": part}, "part", default) for part in size]


def make(name, options):
    """The shape's mesh and its .mtl text for <name> (options as the module says)."""
    shape = options.get("shape")
    if shape not in SHAPES:
        raise ValueError(f"shape is one of {', '.join(SHAPES)}")
    known = {"shape", "size", "radius", "height", "sides", "rings", "colour", "surface"}
    unknown = sorted(set(options) - known)
    if unknown:
        raise ValueError(f"unknown {', '.join(unknown)}")
    if shape == "box":
        width, depth, height = _size(options, 3, 0.5)
        mesh = box(width, depth, height, material=name)
    elif shape == "cylinder":
        radius = _length(options, "radius", 0.25)
        height = _length(options, "height", 0.5)
        mesh = cylinder(radius, height, _count(options, "sides", 24, SIDES), material=name)
    elif shape == "sphere":
        radius = _length(options, "radius", 0.25)
        rings = _count(options, "rings", 12, RINGS)
        mesh = sphere(radius, rings, _count(options, "sides", 24, SIDES), material=name)
    else:
        width, depth = _size(options, 2, 1.0)
        mesh = plane(width, depth, material=name)
    colour = options.get("colour", [0.8, 0.8, 0.8])
    parts_ok = all(isinstance(part, (int, float)) and 0 <= part <= 1 for part in colour)
    if not isinstance(colour, list) or len(colour) != 3 or not parts_ok:
        raise ValueError("colour is [r, g, b], each from 0 to 1")
    surface = options.get("surface", "mat_wood")
    if surface not in SURFACES:
        raise ValueError(f"surface is one of {', '.join(SURFACES)}")
    material = kcds_cgf.write_mtl([{"name": name, "colour": tuple(colour), "surface": surface}])
    return mesh, material


def write(name, options, folder):
    """Writes <name>.cgf and <name>.mtl into `folder`; answers what the module prints."""
    if not (0 < len(name) <= 48 and name.isascii() and all(c.isalnum() or c in "_-" for c in name)):
        raise ValueError("the name is letters, digits, '_' and '-', at most 48")
    mesh, material = make(name, options)
    os.makedirs(folder, exist_ok=True)
    model_path = os.path.join(folder, f"{name}.cgf")
    material_path = os.path.join(folder, f"{name}.mtl")
    with open(model_path, "wb") as file:
        file.write(kcds_cgf.write_cgf(mesh, f"objects/kcds_studio/{name}", node_name=name))
    # Lines end in \n on every system, as the studio's prop menu writes them (studio/shapes.cjs).
    with open(material_path, "w", encoding="ascii", newline="\n") as file:
        file.write(material)
    return {
        "name": name,
        "shape": options["shape"],
        "vertices": len(mesh.positions),
        "triangles": len(mesh.indices) // 3,
        "files": [model_path, material_path],
    }


def studio_folder():
    return os.path.join(os.environ.get("LOCALAPPDATA", ""), "KCDS", "studio", "meshes")


def main(argv):
    if len(argv) < 2:
        print("usage: python studio_shapes.py <name> <json> [<out folder>]")
        return 2
    try:
        options = json.loads(argv[1])
        if not isinstance(options, dict):
            raise ValueError("the options are a JSON object")
        made = write(argv[0], options, argv[2] if len(argv) > 2 else studio_folder())
    except ValueError as error:
        print(json.dumps({"error": str(error)}))
        return 1
    print(json.dumps(made))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
