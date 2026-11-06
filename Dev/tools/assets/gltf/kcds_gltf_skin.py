"""A skinned glTF 2.0 mesh as the game's .skin on the character's skeleton, without
Blender.

    python kcds_gltf_skin.py <file.gltf|.glb> <name> [<out folder>] [--map game=glTF,...]
                             [--template <game .skin path>] [--game <installation folder>]

Writes <name>.skin, <name>.mtl and its textures (as kcds_gltf.py does for a static mesh) into the
studio's meshes folder, for the studio's `studio_wear_skin`. Answers a JSON line: { name,
vertices, triangles, materials, textures, bones, mapped, moved, files } or { error }.

How it binds: the skeleton is one of the game's own skins (kcds_skin.Skeleton), by default the
male work coat in the installation's Data/Characters.pak, which carries all 133 bones of the male
skeleton. The file's joints match the game's bones by name, any case, without a rig's prefix
(kcds_gltf_anim.match; --map adds or changes pairs). A vertex weighted to a joint that matches
no bone goes to that joint's nearest matching ancestor; a file joint with weights and no
matching ancestor is refused by name. The mesh is taken as it stands in the file's rest pose
(each vertex through its joints' world matrices and inverse bind matrices), turned to the body's
frame (glTF characters face +z with y up, the game's face +y with z up: right -x, forward z, up
y), and each vertex is moved by its joints' offsets from the file's joint to the game's bone
(`moved` answers the largest, metres). A rig made on the game's skeleton has offsets near 0 and
binds exactly; another rig's proportions are taken over as well as weights blend them, its rest
pose's turns are not.
"""

import json
import math
import os
import struct
import sys
import zipfile
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "blender"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import kcds_cgf  # noqa: E402
import kcds_gltf  # noqa: E402
import kcds_gltf_anim  # noqa: E402
import kcds_skin  # noqa: E402

GltfError = kcds_gltf.GltfError
DEFAULT_GAME = r"C:\Program Files (x86)\Steam\steamapps\common\KingdomComeDeliverance2"
DEFAULT_PAK = os.path.join("Data", "Characters.pak")
DEFAULT_TEMPLATE = (
    "Objects/characters/humans/male/clothing/torso/decor/workcoat/"
    "boundary_torso_workcoat_01/boundary_torso_workcoat_01.skin"
)
IDENTITY = [1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0]
WEIGHT_PLACES = 4


def body(v):
    """glTF's axes to the game character's: right -x, forward z, up y."""
    return (-v[0], v[2], v[1])


def read_template(game_folder, path):
    """A game skin's bytes: a file on disk, or a path in the installation's Characters.pak."""
    if os.path.isfile(path):
        with open(path, "rb") as file:
            return file.read()
    pak_path = os.path.join(game_folder, DEFAULT_PAK)
    if not os.path.isfile(pak_path):
        raise GltfError(f"no game installation at {game_folder} (Data/Characters.pak not found)")
    with zipfile.ZipFile(pak_path) as pak:
        wanted = path.replace("\\", "/").lower()
        for info in pak.infolist():
            if info.filename.lower() == wanted:
                return pak.read(info)
    raise GltfError(f"the game has no skin {path}")


def node_worlds(document):
    """Every node's world matrix in the scene's rest pose, and every node's parent."""
    nodes = document.get("nodes", [])
    parents = {}
    for index, node in enumerate(nodes):
        for child in node.get("children", []):
            parents[child] = index
    worlds = {}
    stack = [(index, IDENTITY) for index in range(len(nodes)) if index not in parents]
    while stack:
        index, parent = stack.pop()
        world = kcds_gltf._multiply(parent, kcds_gltf._matrix_of(nodes[index]))
        worlds[index] = world
        stack.extend((child, world) for child in nodes[index].get("children", []))
    return worlds, parents


def _joint_name(document, index):
    return document["nodes"][index].get("name") or f"joint_{index}"


def bone_of_joints(document, joints, parents, skeleton, given):
    """Per joint of the skin the game bone it binds to (its own or its nearest matching
    ancestor's), or None; and { game bone: glTF joint } of the matches."""
    names = [_joint_name(document, joint) for joint in joints]
    game = [{"name": bone.name} for bone in skeleton.bones]
    try:
        mapped = kcds_gltf_anim.match(game, names, given)
    except kcds_gltf_anim.AnimationError as error:
        raise GltfError(str(error)) from error
    by_joint_name = {gltf: bone for bone, gltf in mapped.items()}
    out = []
    for joint in joints:
        found = None
        walk = joint
        while walk is not None:
            name = _joint_name(document, walk)
            if name in by_joint_name:
                found = (by_joint_name[name], walk)
                break
            walk = parents.get(walk)
        out.append(found)
    return out, mapped


def build_skinned(document, buffers, skeleton, given):
    """A kcds_skin.SkinnedMesh of the scene's skinned meshes, the glTF material per material,
    the matches and the largest move."""
    worlds, parents = node_worlds(document)
    corners_by_material = {}
    order = []
    mapped_all = {}
    moved = 0.0
    skinned = [
        (index, node)
        for index, node in enumerate(document.get("nodes", []))
        if "mesh" in node and "skin" in node and index in worlds
    ]
    if not skinned:
        raise GltfError("the file has no skinned mesh (a mesh node with a skin)")
    for _, node in skinned:
        skin = document["skins"][node["skin"]]
        joints = skin["joints"]
        if "inverseBindMatrices" in skin:
            inverse = [
                list(m) for m in kcds_gltf.accessor(document, buffers, skin["inverseBindMatrices"])
            ]
        else:
            inverse = [IDENTITY] * len(joints)
        skinning = [
            kcds_gltf._multiply(worlds[joint], inverse[k]) for k, joint in enumerate(joints)
        ]
        bones, mapped = bone_of_joints(document, joints, parents, skeleton, given)
        mapped_all.update(mapped)
        offsets = []
        for found in bones:
            if found is None:
                offsets.append(None)
                continue
            bone_name, joint = found
            at = body(kcds_gltf._point(worlds[joint], (0.0, 0.0, 0.0)))
            target = skeleton.bone(bone_name).position
            offsets.append(tuple(target[k] - at[k] for k in range(3)))
        for primitive in document["meshes"][node["mesh"]].get("primitives", []):
            if primitive.get("mode", kcds_gltf.TRIANGLES) != kcds_gltf.TRIANGLES:
                raise GltfError("only triangle meshes are read (not lines or points)")
            attributes = primitive["attributes"]
            if "POSITION" not in attributes or "JOINTS_0" not in attributes:
                continue
            positions = kcds_gltf.accessor(document, buffers, attributes["POSITION"])
            count = len(positions)
            joint_sets = kcds_gltf.accessor(document, buffers, attributes["JOINTS_0"])
            weight_sets = kcds_gltf.accessor(document, buffers, attributes["WEIGHTS_0"])
            if "indices" in primitive:
                flat = [e[0] for e in kcds_gltf.accessor(document, buffers, primitive["indices"])]
            else:
                flat = list(range(count))
            triangles = [tuple(flat[at : at + 3]) for at in range(0, len(flat) - 2, 3)]
            normals_in = (
                kcds_gltf.accessor(document, buffers, attributes["NORMAL"])
                if "NORMAL" in attributes
                else None
            )
            uvs = (
                kcds_gltf.accessor(document, buffers, attributes["TEXCOORD_0"])
                if "TEXCOORD_0" in attributes
                else [(0.0, 0.0)] * count
            )
            placed = []
            normals = []
            weights = []
            for vertex in range(count):
                pairs = [
                    (int(joint), float(weight))
                    for joint, weight in zip(joint_sets[vertex], weight_sets[vertex])
                    if weight > 0
                ]
                if not pairs:
                    raise GltfError(
                        f"a vertex of {node.get('name', 'the mesh')} has no joint weight"
                    )
                total = sum(weight for _, weight in pairs)
                position = [0.0, 0.0, 0.0]
                normal = [0.0, 0.0, 0.0]
                shift = [0.0, 0.0, 0.0]
                bone_weights = {}
                for joint, weight in pairs:
                    share = weight / total
                    p = kcds_gltf._point(skinning[joint], positions[vertex])
                    for k in range(3):
                        position[k] += share * p[k]
                    if normals_in is not None:
                        n = kcds_gltf._direction(skinning[joint], normals_in[vertex])
                        for k in range(3):
                            normal[k] += share * n[k]
                    found = bones[joint]
                    if found is None:
                        raise GltfError(
                            f"the joint {_joint_name(document, joints[joint])} matches no bone of "
                            "the game's skeleton, nor does any joint above it (--map names it)"
                        )
                    bone_weights[found[0]] = bone_weights.get(found[0], 0.0) + share
                    for k in range(3):
                        shift[k] += share * offsets[joint][k]
                moved = max(moved, math.sqrt(sum(part * part for part in shift)))
                at = body(position)
                placed.append(tuple(at[k] + shift[k] for k in range(3)))
                normals.append(kcds_gltf._unit(body(normal)) if normals_in is not None else None)
                weights.append(
                    tuple(
                        sorted((bone, round(w, WEIGHT_PLACES)) for bone, w in bone_weights.items())
                    )
                )
            if normals_in is None:
                normals = _face_normals(placed, triangles)
            # The body's frame is a turn (no mirror), so the faces keep their winding.
            tangents = kcds_gltf._made_tangents(placed, normals, uvs, triangles)
            material = primitive.get("material")
            if material not in corners_by_material:
                corners_by_material[material] = []
                order.append(material)
            corners = corners_by_material[material]
            for triangle in triangles:
                for vertex in triangle:
                    n = normals[vertex]
                    tx, ty, tz, w = tangents[vertex]
                    t = (tx, ty, tz)
                    b = tuple(-part * w for part in kcds_gltf._cross(n, t))
                    corners.append(
                        (
                            tuple(round(part, 5) for part in placed[vertex]),
                            tuple(round(part, 4) for part in n),
                            (round(uvs[vertex][0], 5), round(uvs[vertex][1], 5)),
                            tuple(round(part, 4) for part in t),
                            tuple(round(part, 4) for part in b),
                            -1 if w >= 0 else 1,
                            weights[vertex],
                        )
                    )
    return _assemble(document, corners_by_material, order), mapped_all, moved


def _face_normals(positions, triangles):
    normals = [(0.0, 0.0, 0.0)] * len(positions)
    for a, b, c in triangles:
        face = kcds_gltf._cross(
            [positions[b][k] - positions[a][k] for k in range(3)],
            [positions[c][k] - positions[a][k] for k in range(3)],
        )
        for vertex in (a, b, c):
            normals[vertex] = tuple(normals[vertex][k] + face[k] for k in range(3))
    return [kcds_gltf._unit(n) for n in normals]


def _assemble(document, corners_by_material, order):
    out = kcds_skin.SkinnedMesh()
    materials = []
    for material in order:
        corners = corners_by_material[material]
        if not corners:
            continue
        first_index = len(out.indices)
        first_vertex = len(out.positions)
        shared = {}
        for corner in corners:
            vertex = shared.get(corner)
            if vertex is None:
                vertex = len(out.positions)
                shared[corner] = vertex
                position, normal, uv, tangent, bitangent, sign, weights = corner
                out.positions.append(position)
                out.normals.append(normal)
                out.uvs.append(uv)
                out.tangents.append((tangent, bitangent, sign))
                out.weights.append(list(weights))
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
        out.materials.append(kcds_gltf._material_name(document, material))
        # A worn mesh has no physics of its own: the character's bones carry it.
        out.physicalize.append(kcds_cgf.PHYSICALIZE_NONE)
        materials.append(material)
    if not out.positions:
        raise GltfError("the skinned meshes have no triangles")
    return out, materials


def convert(path, name, folder, given=None, template=DEFAULT_TEMPLATE, game=DEFAULT_GAME):
    """Writes <name>.skin, <name>.mtl and its textures into `folder`; answers what is printed."""
    if not (0 < len(name) <= 48 and name.isascii() and all(c.isalnum() or c in "_-" for c in name)):
        raise GltfError("the name is letters, digits, '_' and '-', at most 48")
    skeleton = kcds_skin.Skeleton.from_skin(read_template(game, template))
    document, buffers, source_folder = kcds_gltf.load(path)
    (mesh, material_indices), mapped, moved = build_skinned(
        document, buffers, skeleton, given or {}
    )
    os.makedirs(folder, exist_ok=True)
    materials, files = kcds_gltf.write_materials(
        document, buffers, source_folder, name, folder, mesh.materials, material_indices
    )
    model = os.path.join(folder, f"{name}.skin")
    material = os.path.join(folder, f"{name}.mtl")
    with open(model, "wb") as file:
        file.write(
            kcds_skin.write_skin(mesh, skeleton, f"objects/kcds_studio/{name}", node_name=name)
        )
    with open(material, "w", encoding="ascii", newline="\n") as file:
        file.write(kcds_cgf.write_mtl(materials))
    return {
        "name": name,
        "vertices": len(mesh.positions),
        "triangles": len(mesh.indices) // 3,
        "materials": len(mesh.materials),
        "textures": len(files),
        "bones": len(skeleton.bones),
        "mapped": mapped,
        "moved": round(moved, 4),
        "files": [model, material, *files],
    }


def _given_map(text):
    pairs = {}
    for part in text.split(","):
        if "=" in part:
            game, gltf = part.split("=", 1)
            pairs[game.strip()] = gltf.strip()
    return pairs


def main(argv):
    options = {"--map": "", "--template": DEFAULT_TEMPLATE, "--game": DEFAULT_GAME}
    positional = []
    at = 0
    while at < len(argv):
        if argv[at] in options and at + 1 < len(argv):
            options[argv[at]] = argv[at + 1]
            at += 2
        else:
            positional.append(argv[at])
            at += 1
    if len(positional) < 2:
        print(
            "usage: python kcds_gltf_skin.py <file.gltf|file.glb> <name> [<out folder>] "
            "[--map game=glTF,...] [--template <game .skin>] [--game <installation folder>]"
        )
        return 2
    folder = positional[2] if len(positional) > 2 else kcds_gltf.studio_folder()
    try:
        made = convert(
            positional[0],
            positional[1],
            folder,
            _given_map(options["--map"]),
            options["--template"],
            options["--game"],
        )
    except (
        GltfError,
        kcds_skin.SkinError,
        OSError,
        KeyError,
        IndexError,
        struct.error,
        zlib.error,
        zipfile.BadZipFile,
    ) as error:
        print(json.dumps({"error": str(error) or type(error).__name__}))
        return 1
    print(json.dumps(made))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
