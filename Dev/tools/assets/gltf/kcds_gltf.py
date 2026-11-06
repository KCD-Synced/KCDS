"""glTF 2.0 to the Asset/Animation Studio: a .gltf or .glb file's meshes as one of
the game's own static models (kcds_cgf.py), without Blender.

    python kcds_gltf.py <file.gltf|file.glb> <name> [<out folder> [<game folder>]]

<game folder> is the files' path in the game: objects/kcds_studio (the studio's) by default, or a
resource's objects/kcds/<resource>, whose stream/ folder then holds them at that path.

Every mesh the scene's nodes hold is taken, in world space (each node's matrix or its
translation, rotation and scale, down the node tree), its triangles grouped by material. glTF's
axes (y up, metres) become the game's (z up): a point (x, y, z) goes to (x, -z, y), a turn of
90 degrees about x, so faces keep their winding and handedness. Texture coordinates are kept (glTF
and the game both count v from the top).

A material's baseColorFactor is its colour; its baseColorTexture (a PNG or a baseline JPEG,
kcds_jpeg.py, in the file or beside it)
becomes <name>_<material>.dds drawn in place of the colour, its normalTexture
<name>_<material>_normal.dds (at most 1024 pixels a side, kcds_cgf.write_dds). A vertex's tangent is
the file's TANGENT when it has one, else made from the triangle's texture coordinates. At most
65535 vertices. Skins, skeletons and animations are not read yet (static meshes only).

Writes <name>.cgf, <name>.mtl (and the textures) into the folder (the studio's meshes folder,
%LOCALAPPDATA%\\KCDS\\studio\\meshes, at first) and prints one JSON line: { name, vertices,
triangles, materials, textures, files }, or { error } (exit code 1).
"""

import base64
import json
import math
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "blender"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import kcds_cgf  # noqa: E402
import kcds_jpeg  # noqa: E402

GLB_MAGIC = 0x46546C67
GLB_JSON = 0x4E4F534A
GLB_BIN = 0x004E4942
TRIANGLES = 4
COMPONENT = {5120: "b", 5121: "B", 5122: "h", 5123: "H", 5125: "I", 5126: "f"}
WIDTH = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}
LARGEST_TEXTURE = 1024
DEFAULT_MATERIAL = "kcds_default"
# Where the game finds the studio's meshes; a resource's are under objects/kcds/<resource>.
STUDIO_GAME_FOLDER = "objects/kcds_studio"


class GltfError(ValueError):
    """What the file has that the importer cannot take."""


# ---- The file: JSON, buffers, accessors ----


def load(path):
    """The glTF's JSON and its buffers' bytes."""
    with open(path, "rb") as file:
        data = file.read()
    folder = os.path.dirname(os.path.abspath(path))
    binary = None
    if len(data) >= 12 and struct.unpack_from("<I", data, 0)[0] == GLB_MAGIC:
        at = 12
        document = None
        while at + 8 <= len(data):
            length, kind = struct.unpack_from("<II", data, at)
            chunk = data[at + 8 : at + 8 + length]
            if kind == GLB_JSON:
                document = json.loads(chunk.decode("utf-8"))
            elif kind == GLB_BIN and binary is None:
                binary = chunk
            at += 8 + length
        if document is None:
            raise GltfError("the .glb has no JSON chunk")
    else:
        try:
            document = json.loads(data.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise GltfError(f"not a glTF file ({error})") from error
    version = str(document.get("asset", {}).get("version", ""))
    if not version.startswith("2"):
        raise GltfError(f"glTF {version or '?'}: only glTF 2.0 is read")
    buffers = []
    for index, buffer in enumerate(document.get("buffers", [])):
        uri = buffer.get("uri")
        if uri is None:
            if binary is None:
                raise GltfError(f"buffer {index} has no data")
            buffers.append(binary)
        else:
            buffers.append(read_uri(uri, folder))
    return document, buffers, folder


def read_uri(uri, folder):
    if uri.startswith("data:"):
        header, _, payload = uri.partition(",")
        if not header.endswith(";base64"):
            raise GltfError("a data URI that is not base64")
        return base64.b64decode(payload)
    path = os.path.join(folder, uri.replace("%20", " "))
    if not os.path.isfile(path):
        raise GltfError(f"the file {uri} beside the glTF is missing")
    with open(path, "rb") as file:
        return file.read()


def view_bytes(document, buffers, index):
    view = document["bufferViews"][index]
    start = view.get("byteOffset", 0)
    return buffers[view["buffer"]], start, view.get("byteStride"), view["byteLength"]


def accessor(document, buffers, index):
    """An accessor's elements: tuples of numbers (normalized integers as 0..1 or -1..1)."""
    info = document["accessors"][index]
    if info.get("sparse"):
        raise GltfError("sparse accessors are not read")
    kind = COMPONENT[info["componentType"]]
    width = WIDTH[info["type"]]
    count = info["count"]
    size = struct.calcsize("<" + kind)
    if "bufferView" not in info:
        return [tuple([0] * width) for _ in range(count)]
    data, start, stride, _ = view_bytes(document, buffers, info["bufferView"])
    start += info.get("byteOffset", 0)
    stride = stride or size * width
    out = []
    form = "<" + kind * width
    for element in range(count):
        values = struct.unpack_from(form, data, start + element * stride)
        if info.get("normalized") and kind != "f":
            top = {"b": 127, "B": 255, "h": 32767, "H": 65535}.get(kind, 1)
            values = tuple(max(-1.0, value / top) for value in values)
        out.append(values)
    return out


# ---- PNG pictures (pure Python: zlib) ----


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def decode_picture(data):
    """A PNG's or a baseline JPEG's width, height and blue green red alpha rows (kcds_jpeg)."""
    if data[:2] == b"\xff\xd8":
        try:
            return kcds_jpeg.decode(data)
        except (kcds_jpeg.JpegError, IndexError, KeyError, StopIteration, struct.error) as error:
            raise GltfError(f"a JPEG texture: {error or 'it ends too soon'}") from error
    return decode_png(data)


def decode_png(data):
    """A PNG's width, height and pixels as rows from the top, blue green red alpha bytes (8-bit
    grey, grey with alpha, RGB, RGBA and palette pictures, not interlaced)."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise GltfError("a texture that is neither a PNG nor a JPEG")
    at = 8
    width = height = depth = colour = interlace = None
    palette = b""
    alpha = b""
    compressed = b""
    while at + 8 <= len(data):
        length, kind = struct.unpack_from(">I4s", data, at)
        chunk = data[at + 8 : at + 8 + length]
        if kind == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", chunk)
        elif kind == b"PLTE":
            palette = chunk
        elif kind == b"tRNS":
            alpha = chunk
        elif kind == b"IDAT":
            compressed += chunk
        elif kind == b"IEND":
            break
        at += 12 + length
    if depth != 8 or interlace != 0:
        raise GltfError("a PNG that is not 8 bits a channel or is interlaced")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(colour)
    if channels is None:
        raise GltfError(f"a PNG of colour type {colour}")
    raw = zlib.decompress(compressed)
    row_size = width * channels
    rows = []
    previous = bytearray(row_size)
    at = 0
    for _ in range(height):
        kind = raw[at]
        row = bytearray(raw[at + 1 : at + 1 + row_size])
        at += 1 + row_size
        for x in range(row_size):
            left = row[x - channels] if x >= channels else 0
            up = previous[x]
            corner = previous[x - channels] if x >= channels else 0
            if kind == 1:
                row[x] = (row[x] + left) & 0xFF
            elif kind == 2:
                row[x] = (row[x] + up) & 0xFF
            elif kind == 3:
                row[x] = (row[x] + ((left + up) >> 1)) & 0xFF
            elif kind == 4:
                row[x] = (row[x] + _paeth(left, up, corner)) & 0xFF
        rows.append(row)
        previous = row
    out = bytearray(width * height * 4)
    for y, row in enumerate(rows):
        for x in range(width):
            if colour == 0:
                r = g = b = row[x]
                a = 255
            elif colour == 4:
                r = g = b = row[x * 2]
                a = row[x * 2 + 1]
            elif colour == 2:
                r, g, b = row[x * 3 : x * 3 + 3]
                a = 255
            elif colour == 6:
                r, g, b, a = row[x * 4 : x * 4 + 4]
            else:
                index = row[x]
                r, g, b = palette[index * 3 : index * 3 + 3]
                a = alpha[index] if index < len(alpha) else 255
            to = (y * width + x) * 4
            out[to : to + 4] = bytes((b, g, r, a))
    return width, height, bytes(out)


def _smaller(width, height, bgra):
    """Halved (kcds_cgf's mip step) until at most LARGEST_TEXTURE a side."""
    while max(width, height) > LARGEST_TEXTURE:
        width, height, bgra = kcds_cgf._half(width, height, bgra)
    return width, height, bgra


def image_bytes(document, buffers, folder, texture_index):
    texture = document["textures"][texture_index]
    source = texture.get("source")
    if source is None:
        raise GltfError(f"texture {texture_index} has no picture")
    image = document["images"][source]
    if "bufferView" in image:
        data, start, _, length = view_bytes(document, buffers, image["bufferView"])
        return data[start : start + length]
    return read_uri(image["uri"], folder)


# ---- The scene: nodes and their matrices ----


def _matrix_of(node):
    """A node's local matrix (column-major 4x4, as glTF keeps it)."""
    if "matrix" in node:
        return list(node["matrix"])
    tx, ty, tz = node.get("translation", (0, 0, 0))
    qx, qy, qz, qw = node.get("rotation", (0, 0, 0, 1))
    sx, sy, sz = node.get("scale", (1, 1, 1))
    xx, yy, zz = qx * qx, qy * qy, qz * qz
    xy, xz, yz = qx * qy, qx * qz, qy * qz
    wx, wy, wz = qw * qx, qw * qy, qw * qz
    return [
        (1 - 2 * (yy + zz)) * sx, 2 * (xy + wz) * sx, 2 * (xz - wy) * sx, 0,
        2 * (xy - wz) * sy, (1 - 2 * (xx + zz)) * sy, 2 * (yz + wx) * sy, 0,
        2 * (xz + wy) * sz, 2 * (yz - wx) * sz, (1 - 2 * (xx + yy)) * sz, 0,
        tx, ty, tz, 1,
    ]  # fmt: skip


def _multiply(a, b):
    out = [0.0] * 16
    for column in range(4):
        for row in range(4):
            out[column * 4 + row] = sum(a[k * 4 + row] * b[column * 4 + k] for k in range(4))
    return out


def _point(m, p):
    return tuple(
        m[row] * p[0] + m[4 + row] * p[1] + m[8 + row] * p[2] + m[12 + row] for row in range(3)
    )


def _direction(m, d):
    return tuple(m[row] * d[0] + m[4 + row] * d[1] + m[8 + row] * d[2] for row in range(3))


def _normal_matrix(m):
    """The inverse transpose of the matrix's 3x3 part (column-major 3x3)."""
    a = [[m[column * 4 + row] for column in range(3)] for row in range(3)]
    det = (
        a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
        - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
        + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0])
    )
    if abs(det) < 1e-12:
        raise GltfError("a node is scaled to nothing")
    cofactor = [
        [
            (a[(r + 1) % 3][(c + 1) % 3] * a[(r + 2) % 3][(c + 2) % 3])
            - (a[(r + 1) % 3][(c + 2) % 3] * a[(r + 2) % 3][(c + 1) % 3])
            for c in range(3)
        ]
        for r in range(3)
    ]
    # (A^-1)^T = cofactor / det.
    return [[cofactor[r][c] / det for c in range(3)] for r in range(3)], det


def _apply3(rows, v):
    return tuple(sum(rows[r][c] * v[c] for c in range(3)) for r in range(3))


def _unit(v):
    length = math.sqrt(sum(part * part for part in v))
    return tuple(part / length for part in v) if length > 1e-12 else (0.0, 0.0, 1.0)


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _game(v):
    """glTF's axes to the game's: y up becomes z up."""
    return (v[0], -v[2], v[1])


def scene_meshes(document):
    """(mesh index, world matrix) of every mesh in the scene, down its node tree."""
    scenes = document.get("scenes", [])
    roots = scenes[document.get("scene", 0)].get("nodes", []) if scenes else []
    if not scenes:
        children = {
            child for node in document.get("nodes", []) for child in node.get("children", [])
        }
        roots = [index for index in range(len(document.get("nodes", []))) if index not in children]
    found = []
    stack = [(index, [1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0]) for index in roots]
    while stack:
        index, parent = stack.pop()
        node = document["nodes"][index]
        world = _multiply(parent, _matrix_of(node))
        if "mesh" in node:
            found.append((node["mesh"], world))
        stack.extend((child, world) for child in node.get("children", []))
    return found


# ---- The mesh ----


def _material_name(document, index):
    if index is None:
        return DEFAULT_MATERIAL
    given = document.get("materials", [])[index].get("name") or f"material_{index}"
    cleaned = "".join(c if (c.isalnum() and c.isascii()) or c == "_" else "_" for c in given)
    return cleaned[:60] or f"material_{index}"


def _made_tangents(positions, normals, uvs, triangles):
    """Per vertex a tangent and handedness from its triangles' texture coordinates."""
    tangent = [[0.0, 0.0, 0.0] for _ in positions]
    bitangent = [[0.0, 0.0, 0.0] for _ in positions]
    for a, b, c in triangles:
        p1, p2, p3 = positions[a], positions[b], positions[c]
        w1, w2, w3 = uvs[a], uvs[b], uvs[c]
        e1 = [p2[k] - p1[k] for k in range(3)]
        e2 = [p3[k] - p1[k] for k in range(3)]
        du1, dv1 = w2[0] - w1[0], w2[1] - w1[1]
        du2, dv2 = w3[0] - w1[0], w3[1] - w1[1]
        det = du1 * dv2 - du2 * dv1
        if abs(det) < 1e-12:
            continue
        r = 1.0 / det
        t = [(e1[k] * dv2 - e2[k] * dv1) * r for k in range(3)]
        s = [(e2[k] * du1 - e1[k] * du2) * r for k in range(3)]
        for vertex in (a, b, c):
            for k in range(3):
                tangent[vertex][k] += t[k]
                bitangent[vertex][k] += s[k]
    out = []
    for index, normal in enumerate(normals):
        t = tangent[index]
        # Gram-Schmidt against the normal; any direction across it when the uv gives none.
        dot = sum(t[k] * normal[k] for k in range(3))
        t = _unit([t[k] - normal[k] * dot for k in range(3)])
        if abs(sum(t[k] * normal[k] for k in range(3))) > 0.9:
            t = _unit(_cross(normal, (1.0, 0.0, 0.0) if abs(normal[0]) < 0.9 else (0.0, 1.0, 0.0)))
        # glTF's handedness: the bitangent (where v grows) on the side of n x t.
        w = 1.0 if sum(_cross(normal, t)[k] * bitangent[index][k] for k in range(3)) >= 0 else -1.0
        out.append((*t, w))
    return out


def build(document, buffers):
    """One kcds_cgf.Mesh of the scene's meshes and, per material in the mesh's order, its glTF
    material index (or None)."""
    corners_by_material = {}
    order = []
    for mesh_index, world in scene_meshes(document):
        normal_rows, det = _normal_matrix(world)
        for primitive in document["meshes"][mesh_index].get("primitives", []):
            if primitive.get("mode", TRIANGLES) != TRIANGLES:
                raise GltfError("only triangle meshes are read (not lines or points)")
            attributes = primitive["attributes"]
            if "POSITION" not in attributes:
                continue
            positions = accessor(document, buffers, attributes["POSITION"])
            count = len(positions)
            if "indices" in primitive:
                flat = [element[0] for element in accessor(document, buffers, primitive["indices"])]
            else:
                flat = list(range(count))
            triangles = [tuple(flat[at : at + 3]) for at in range(0, len(flat) - 2, 3)]
            if det < 0:
                # A mirroring node turns the faces round: their winding goes back.
                triangles = [(a, c, b) for a, b, c in triangles]
            world_positions = [_point(world, p) for p in positions]
            if "NORMAL" in attributes:
                normals = [
                    _unit(_apply3(normal_rows, n))
                    for n in accessor(document, buffers, attributes["NORMAL"])
                ]
            else:
                normals = [(0.0, 0.0, 0.0)] * count
                for a, b, c in triangles:
                    face = _cross(
                        [world_positions[b][k] - world_positions[a][k] for k in range(3)],
                        [world_positions[c][k] - world_positions[a][k] for k in range(3)],
                    )
                    for vertex in (a, b, c):
                        normals[vertex] = tuple(normals[vertex][k] + face[k] for k in range(3))
                normals = [_unit(n) for n in normals]
            uvs = (
                accessor(document, buffers, attributes["TEXCOORD_0"])
                if "TEXCOORD_0" in attributes
                else [(0.0, 0.0)] * count
            )
            if "TANGENT" in attributes:
                tangents = [
                    (
                        *_unit(_direction(world, t[:3])),
                        (1.0 if t[3] >= 0 else -1.0) * (1.0 if det >= 0 else -1.0),
                    )
                    for t in accessor(document, buffers, attributes["TANGENT"])
                ]
            else:
                tangents = _made_tangents(world_positions, normals, uvs, triangles)
            material = primitive.get("material")
            if material not in corners_by_material:
                corners_by_material[material] = []
                order.append(material)
            corners = corners_by_material[material]
            for triangle in triangles:
                for vertex in triangle:
                    n = _game(normals[vertex])
                    tx, ty, tz, w = tangents[vertex]
                    t = _game((tx, ty, tz))
                    # glTF's bitangent (n x t times w) points where v grows; the game's where v
                    # falls (kcds_cgf: tangent x bitangent is the normal times the sign).
                    b = tuple(-part * w for part in _cross(n, t))
                    corners.append(
                        (
                            tuple(round(part, 5) for part in _game(world_positions[vertex])),
                            (round(uvs[vertex][0], 5), round(uvs[vertex][1], 5)),
                            tuple(round(part, 4) for part in t),
                            tuple(round(part, 4) for part in b),
                            -1 if w >= 0 else 1,
                        )
                    )
    out = kcds_cgf.Mesh()
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
                position, uv, tangent, bitangent, sign = corner
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
        out.materials.append(_material_name(document, material))
        out.physicalize.append(kcds_cgf.PHYSICALIZE_DEFAULT)
        materials.append(material)
    if not out.positions:
        raise GltfError("the scene has no triangle mesh")
    return out, materials


def convert(path, name, folder, game_folder=STUDIO_GAME_FOLDER):
    """Writes <name>.cgf, <name>.mtl and its textures into `folder`; answers what is printed.
    `game_folder` is where the game finds them: the studio's folder, or a resource's
    objects/kcds/<resource> (its stream/ folder holds them at that path)."""
    if not (0 < len(name) <= 48 and name.isascii() and all(c.isalnum() or c in "_-" for c in name)):
        raise GltfError("the name is letters, digits, '_' and '-', at most 48")
    if not valid_game_folder(game_folder):
        raise GltfError("the game folder is objects/<folders>, letters, digits, '_' and '-'")
    document, buffers, source_folder = load(path)
    mesh, material_indices = build(document, buffers)
    kcds_cgf.check(mesh)
    os.makedirs(folder, exist_ok=True)
    materials, files = write_materials(
        document,
        buffers,
        source_folder,
        name,
        folder,
        mesh.materials,
        material_indices,
        game_folder,
    )
    model = os.path.join(folder, f"{name}.cgf")
    material = os.path.join(folder, f"{name}.mtl")
    with open(model, "wb") as file:
        file.write(kcds_cgf.write_cgf(mesh, f"{game_folder}/{name}", node_name=name))
    with open(material, "w", encoding="ascii", newline="\n") as file:
        file.write(kcds_cgf.write_mtl(materials))
    return {
        "name": name,
        "vertices": len(mesh.positions),
        "triangles": len(mesh.indices) // 3,
        "materials": len(mesh.materials),
        "textures": len(files),
        "files": [model, material, *files],
    }


def write_materials(
    document,
    buffers,
    source_folder,
    name,
    folder,
    names,
    material_indices,
    game_folder=None,
):
    """The materials for kcds_cgf.write_mtl, per material of the mesh its colour and its base
    colour and normal textures written as <name>_<material>[_normal].dds into `folder`, named
    by their path in `game_folder`; and the texture files written."""
    game_folder = game_folder or STUDIO_GAME_FOLDER
    files = []
    materials = []
    for material_name, index in zip(names, material_indices):
        info = document.get("materials", [])[index] if index is not None else {}
        pbr = info.get("pbrMetallicRoughness", {})
        r, g, b, _ = pbr.get("baseColorFactor", (0.8, 0.8, 0.8, 1.0))
        entry = {"name": material_name, "colour": (r, g, b)}
        for key, given, suffix, srgb in (
            ("texture", pbr.get("baseColorTexture"), "", True),
            ("normal", info.get("normalTexture"), "_normal", False),
        ):
            if not given:
                continue
            width, height, bgra = _smaller(
                *decode_picture(image_bytes(document, buffers, source_folder, given["index"]))
            )
            file_name = f"{name}_{material_name}{suffix}.dds"
            target = os.path.join(folder, file_name)
            with open(target, "wb") as file:
                file.write(kcds_cgf.write_dds(width, height, bgra, srgb=srgb))
            files.append(target)
            entry[key] = f"{game_folder}/{file_name}"
        materials.append(entry)
    return materials, files


def studio_folder():
    return os.path.join(os.environ.get("LOCALAPPDATA", ""), "KCDS", "studio", "meshes")


def valid_game_folder(game_folder):
    parts = game_folder.split("/")
    return (
        len(parts) >= 2
        and parts[0] == "objects"
        and all(
            part and part.isascii() and all(c.isalnum() or c in "_-" for c in part)
            for part in parts
        )
    )


def main(argv):
    if len(argv) < 2:
        print(
            "usage: python kcds_gltf.py <file.gltf|file.glb> <name> [<out folder> [<game folder>]]"
        )
        return 2
    try:
        made = convert(
            argv[0],
            argv[1],
            argv[2] if len(argv) > 2 else studio_folder(),
            argv[3] if len(argv) > 3 else STUDIO_GAME_FOLDER,
        )
    except (GltfError, OSError, KeyError, IndexError, struct.error, zlib.error) as error:
        print(json.dumps({"error": str(error) or type(error).__name__}))
        return 1
    print(json.dumps(made))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
