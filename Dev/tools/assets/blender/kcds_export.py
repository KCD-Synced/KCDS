"""Blender to the Asset/Animation Studio: a scene's meshes as one of the game's own
static models (kcds_cgf.py), which the studio shows from KCDS\\studio\\meshes.

  blender -b <file.blend> --python kcds_export.py -- <name> [<out folder>]

The selected mesh objects are exported (every visible mesh object when none is selected), with
their modifiers applied, in world space (Blender and the game are both right-handed with z up,
in metres). Each material becomes a sub-material of <name>.mtl, coloured as its Base Color (or
viewport colour); faces without a material use a grey one. A material whose Base Color comes
from an Image Texture node gets that image as <name>_<material>.dds beside the model (at most 1024
pixels a side), drawn in place of the colour. A vertex is shared where its corners
agree in position, normal, uv and tangent. At most 65535 vertices.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import kcds_cgf  # noqa: E402

DEFAULT_MATERIAL = "kcds_default"
GREY = (0.6, 0.6, 0.6)
# A material's image is written at most this many pixels a side.
LARGEST_TEXTURE = 1024


def _name_ok(name):
    return 0 < len(name) <= 48 and all(c.isalnum() or c in "_-" for c in name) and name.isascii()


def _material_colour(material):
    if material is None:
        return GREY
    if material.use_nodes and material.node_tree:
        for node in material.node_tree.nodes:
            if node.type == "BSDF_PRINCIPLED":
                value = node.inputs["Base Color"].default_value
                return (value[0], value[1], value[2])
    colour = material.diffuse_color
    return (colour[0], colour[1], colour[2])


def _material_image(material):
    """The image of an Image Texture node feeding the material's Base Color, or None."""
    if material is None or not material.use_nodes or not material.node_tree:
        return None
    for node in material.node_tree.nodes:
        if node.type != "BSDF_PRINCIPLED":
            continue
        for link in node.inputs["Base Color"].links:
            source = link.from_node
            if source.type == "TEX_IMAGE" and source.image is not None:
                return source.image
    return None


def _material_normal_image(material):
    """The image of an Image Texture node feeding a Normal Map node on the material's Normal input,
    or None."""
    if material is None or not material.use_nodes or not material.node_tree:
        return None
    for node in material.node_tree.nodes:
        if node.type != "BSDF_PRINCIPLED":
            continue
        for link in node.inputs["Normal"].links:
            normal_map = link.from_node
            if normal_map.type != "NORMAL_MAP":
                continue
            for colour_link in normal_map.inputs["Color"].links:
                source = colour_link.from_node
                if source.type == "TEX_IMAGE" and source.image is not None:
                    return source.image
    return None


def _image_bgra(image):
    """An image's pixels as rows from the top, blue green red alpha bytes, at most
    LARGEST_TEXTURE pixels a side (scaled down keeping its shape); (width, height, bytes)."""
    width, height = image.size
    if width <= 0 or height <= 0:
        raise ValueError(f"the image {image.name} has no pixels")
    longest = max(width, height)
    if longest > LARGEST_TEXTURE:
        image = image.copy()
        width = max(1, width * LARGEST_TEXTURE // longest)
        height = max(1, height * LARGEST_TEXTURE // longest)
        image.scale(width, height)
    pixels = image.pixels[:]
    out = bytearray(width * height * 4)
    for y in range(height):
        # Blender's rows run from the bottom.
        source_row = (height - 1 - y) * width * 4
        target_row = y * width * 4
        for x in range(width):
            at = source_row + x * 4
            to = target_row + x * 4
            r, g, b, a = pixels[at : at + 4]
            out[to] = min(255, max(0, int(b * 255 + 0.5)))
            out[to + 1] = min(255, max(0, int(g * 255 + 0.5)))
            out[to + 2] = min(255, max(0, int(r * 255 + 0.5)))
            out[to + 3] = min(255, max(0, int(a * 255 + 0.5)))
    return width, height, bytes(out)


def _material_name(material):
    if material is None:
        return DEFAULT_MATERIAL
    cleaned = "".join(
        c if (c.isalnum() and c.isascii()) or c == "_" else "_" for c in material.name
    )
    return cleaned[:60] or DEFAULT_MATERIAL


def _objects(bpy):
    chosen = [o for o in bpy.context.selected_objects if o.type == "MESH"]
    if chosen:
        return chosen
    return [o for o in bpy.context.scene.objects if o.type == "MESH" and o.visible_get()]


def build_mesh(bpy, objects):
    """One kcds_cgf.Mesh of the objects: triangles grouped by material."""
    depsgraph = bpy.context.evaluated_depsgraph_get()
    materials = []  # (name, colour)
    material_index = {}
    images = {}  # material name -> its Base Color image or None
    normals = {}  # material name -> its normal map image or None
    # Per material: the triangles' corners as (position, normal, uv, tangent, bitangent, sign).
    corners_by_material = {}
    for obj in objects:
        evaluated = obj.evaluated_get(depsgraph)
        mesh = evaluated.to_mesh()
        try:
            matrix = obj.matrix_world
            normal_matrix = matrix.to_3x3().inverted_safe().transposed()
            uv_layer = mesh.uv_layers.active
            if uv_layer is not None:
                mesh.calc_tangents(uvmap=uv_layer.name)
            mesh.calc_loop_triangles()
            for triangle in mesh.loop_triangles:
                slot = obj.material_slots[triangle.material_index] if obj.material_slots else None
                material = slot.material if slot else None
                name = _material_name(material)
                if name not in material_index:
                    material_index[name] = len(materials)
                    materials.append((name, _material_colour(material)))
                    images[name] = _material_image(material)
                    normals[name] = _material_normal_image(material)
                index = material_index[name]
                corners = corners_by_material.setdefault(index, [])
                for loop_index in triangle.loops:
                    loop = mesh.loops[loop_index]
                    position = matrix @ mesh.vertices[loop.vertex_index].co
                    normal = (normal_matrix @ loop.normal).normalized()
                    if uv_layer is not None:
                        u, v = uv_layer.data[loop_index].uv
                        tangent = (normal_matrix @ loop.tangent).normalized()
                        sign = loop.bitangent_sign
                    else:
                        u, v = 0.0, 0.0
                        tangent = normal.orthogonal().normalized()
                        sign = 1.0
                    bitangent = normal.cross(tangent) * sign
                    corners.append(
                        (
                            tuple(round(x, 5) for x in position),
                            tuple(round(x, 4) for x in normal),
                            (round(u, 5), round(1.0 - v, 5)),
                            tuple(round(x, 4) for x in tangent),
                            tuple(round(x, 4) for x in bitangent),
                            1 if sign >= 0 else -1,
                        )
                    )
        finally:
            evaluated.to_mesh_clear()
    out = kcds_cgf.Mesh()
    for index, (name, colour) in enumerate(materials):
        corners = corners_by_material.get(index, [])
        if not corners:
            continue
        first_index = len(out.indices)
        first_vertex = len(out.positions)
        shared = {}
        for corner in corners:
            key = corner
            vertex = shared.get(key)
            if vertex is None:
                vertex = len(out.positions)
                shared[key] = vertex
                position, _normal, uv, tangent, bitangent, sign = corner
                out.positions.append(position)
                out.uvs.append(uv)
                out.tangents.append((tangent, bitangent, sign))
            out.indices.append(vertex)
        out.subsets.append(
            kcds_cgf.Subset(
                first_index,
                len(out.indices) - first_index,
                first_vertex,
                len(out.positions) - first_vertex,
                len(out.materials),
            )
        )
        out.materials.append(name)
        out.physicalize.append(kcds_cgf.PHYSICALIZE_DEFAULT)
    return out, [
        {
            "name": name,
            "colour": colour,
            "image": images.get(name),
            "normal_image": normals.get(name),
        }
        for name, colour in materials
        if corners_by_material.get(material_index[name])
    ]


def export(bpy, name, folder):
    """Writes <name>.cgf and <name>.mtl into `folder`; returns their paths."""
    if not _name_ok(name):
        raise ValueError("the name is letters, digits, '_' and '-', at most 48")
    objects = _objects(bpy)
    if not objects:
        raise ValueError("the scene has no mesh to export")
    mesh, materials = build_mesh(bpy, objects)
    os.makedirs(folder, exist_ok=True)
    model = os.path.join(folder, f"{name}.cgf")
    material = os.path.join(folder, f"{name}.mtl")
    # Each material's image as <name>_<material>.dds beside the model (the studio mounts them
    # with it, studio/studio_mesh.hpp), drawn in place of its colour.
    textures = 0
    for entry in materials:
        image = entry.pop("image", None)
        normal_image = entry.pop("normal_image", None)
        for source, suffix, key in ((image, "", "texture"), (normal_image, "_normal", "normal")):
            if source is None:
                continue
            width, height, bgra = _image_bgra(source)
            file_name = f"{name}_{entry['name']}{suffix}.dds"
            with open(os.path.join(folder, file_name), "wb") as file:
                # Colours are sRGB; a normal map is not.
                file.write(kcds_cgf.write_dds(width, height, bgra, srgb=(key == "texture")))
            entry[key] = f"objects/kcds_studio/{file_name}"
            textures += 1
    with open(model, "wb") as file:
        file.write(kcds_cgf.write_cgf(mesh, f"objects/kcds_studio/{name}", node_name=name))
    with open(material, "w", encoding="ascii") as file:
        file.write(kcds_cgf.write_mtl(materials))
    print(
        f"KCDS export: {name}: {len(mesh.positions)} vertices, {len(mesh.indices) // 3} "
        f"triangles, {len(mesh.materials)} materials, {textures} textures -> {model}"
    )
    return model, material


def studio_folder():
    local = os.environ.get("LOCALAPPDATA", "")
    return os.path.join(local, "KCDS", "studio", "meshes")


def main():
    import bpy  # Blender's own module: this runs inside Blender only.

    args = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
    if not args:
        print("usage: blender -b <file.blend> --python kcds_export.py -- <name> [<out folder>]")
        sys.exit(2)
    folder = args[1] if len(args) > 1 else studio_folder()
    try:
        export(bpy, args[0], folder)
    except ValueError as error:
        print(f"KCDS export failed: {error}")
        sys.exit(1)


if __name__ == "__main__":
    main()
