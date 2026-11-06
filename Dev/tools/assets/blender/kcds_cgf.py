"""The game's own static mesh files for the Asset/Animation Studio.

Writes a self-contained .cgf (CryEngine chunk file "CrCh" 0x746) with the chunk versions KCD2's
own models use, as read from the game's horse whip (Objects/.../horse/whip.cgfm):

  MtlName      0x1014 v0x802  name[128], sub-material count, their physicalize types, their names
  MeshSubsets  0x1017 v0x800  flags 4 (a texel density per subset), count, 2 reserved; per subset
                              first index, index count, first vertex, vertex count, material id,
                              radius, centre; then the densities
  DataStream   0x1016 v0x800  flags, stream type, count, element size, 2 reserved, the data:
                              15 P3S_C4B_T2S (half x y z w=1, colour RGBA, half u v),
                              5 indices (16 bits), 6 tangents (int16 tangent and bitangent, w the
                              handedness, all times 32767)
  Mesh         0x1000 v0x801  flags 10 (texel density and face area), vertex and index counts,
                              subsets and their chunk, 16 stream chunk ids, 4 physics chunk ids,
                              the box, texel density, mean face area, 31 reserved
  Node         0x100B v0x824  name[64], object, parent, children, material, 4 bytes, a 4x4 matrix
                              (identity, last row 0), obsolete position/turn/scale, 3 controller
                              ids (-1), property string length

The studio's game client puts the file in the game's file system from memory and the game loads
it as any of its models (Client/runtime/studio/studio_mesh.hpp). Plain Python (struct only), so
Blender's own Python runs it.
"""

import math
import struct

SIGNATURE = b"CrCh"
FILE_VERSION = 0x746
STREAM_P3S_C4B_T2S = 15
STREAM_INDICES = 5
STREAM_TANGENTS = 6
PHYSICALIZE_NONE = -1
PHYSICALIZE_DEFAULT = 0x1000
MAX_VERTICES = 65535


class Subset:
    """The triangles of one material: index range, vertex range, material id."""

    def __init__(self, first_index, index_count, first_vertex, vertex_count, material):
        self.first_index = first_index
        self.index_count = index_count
        self.first_vertex = first_vertex
        self.vertex_count = vertex_count
        self.material = material


class Mesh:
    """A static mesh: per vertex a position, a uv, a tangent and bitangent with the handedness;
    16-bit triangle indices; subsets by material; the materials' names."""

    def __init__(self):
        self.positions = []  # (x, y, z) metres
        self.uvs = []  # (u, v), v down as the game reads it
        self.tangents = []  # ((tx, ty, tz), (bx, by, bz), sign 1 or -1)
        self.indices = []
        self.subsets = []
        self.materials = []  # names
        self.physicalize = []  # per material: PHYSICALIZE_NONE or PHYSICALIZE_DEFAULT


def _pad4(data):
    return data + b"\0" * (-len(data) % 4)


def _clamp_unit(value):
    return max(-32767, min(32767, int(round(value * 32767.0))))


def _mtl_name_chunk(mesh, material_file):
    name = material_file.encode("ascii")[:127]
    body = name + b"\0" * (128 - len(name))
    body += struct.pack("<i", len(mesh.materials))
    for kind in mesh.physicalize:
        body += struct.pack("<i", kind)
    for material in mesh.materials:
        body += material.encode("ascii") + b"\0"
    return body


def _bounds(points):
    low = [min(p[axis] for p in points) for axis in range(3)]
    high = [max(p[axis] for p in points) for axis in range(3)]
    return low, high


def _triangle_areas(mesh):
    """Each triangle's area in space and in its uv."""
    areas = []
    for at in range(0, len(mesh.indices), 3):
        a, b, c = (mesh.indices[at + k] for k in range(3))
        pa, pb, pc = mesh.positions[a], mesh.positions[b], mesh.positions[c]
        u = [pb[k] - pa[k] for k in range(3)]
        v = [pc[k] - pa[k] for k in range(3)]
        cross = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
        space = 0.5 * math.sqrt(sum(part * part for part in cross))
        ta, tb, tc = mesh.uvs[a], mesh.uvs[b], mesh.uvs[c]
        flat = 0.5 * abs((tb[0] - ta[0]) * (tc[1] - ta[1]) - (tc[0] - ta[0]) * (tb[1] - ta[1]))
        areas.append((space, flat))
    return areas


def _densities(mesh):
    """The texel density (space per uv area, as the game's compiler gives) and the mean face
    area (geometric)."""
    areas = _triangle_areas(mesh)
    space = sum(one[0] for one in areas)
    flat = sum(one[1] for one in areas)
    density = math.sqrt(space / flat) if flat > 0 else 1.0
    logs = [math.log(one[0]) for one in areas if one[0] > 0]
    mean = math.exp(sum(logs) / len(logs)) if logs else 0.0
    return density, mean


def _subsets_chunk(mesh, density):
    body = struct.pack("<4i", 4, len(mesh.subsets), 0, 0)
    for subset in mesh.subsets:
        points = mesh.positions[subset.first_vertex : subset.first_vertex + subset.vertex_count]
        low, high = _bounds(points) if points else ([0, 0, 0], [0, 0, 0])
        centre = [(low[k] + high[k]) / 2 for k in range(3)]
        radius = math.sqrt(sum((high[k] - centre[k]) ** 2 for k in range(3)))
        body += struct.pack(
            "<5i4f",
            subset.first_index,
            subset.index_count,
            subset.first_vertex,
            subset.vertex_count,
            subset.material,
            radius,
            *centre,
        )
    for _ in mesh.subsets:
        body += struct.pack("<f", density)
    return body


def _stream_chunk(stream_type, element_size, elements):
    data = b"".join(elements)
    return struct.pack("<6i", 0, stream_type, len(elements), element_size, 0, 0) + data


def _vertex_elements(mesh):
    out = []
    for (x, y, z), (u, v) in zip(mesh.positions, mesh.uvs):
        out.append(struct.pack("<4e4B2e", x, y, z, 1.0, 255, 255, 255, 255, u, v))
    return out


def _tangent_elements(mesh):
    out = []
    for tangent, bitangent, sign in mesh.tangents:
        w = 32767 if sign >= 0 else -32767
        out.append(
            struct.pack(
                "<8h",
                *(_clamp_unit(part) for part in tangent),
                w,
                *(_clamp_unit(part) for part in bitangent),
                w,
            )
        )
    return out


def _mesh_chunk(mesh, subsets_id, stream_ids, density, mean_area):
    low, high = _bounds(mesh.positions)
    streams = [0] * 16
    for stream_type, chunk_id in stream_ids.items():
        streams[stream_type] = chunk_id
    body = struct.pack(
        "<7i", 10, 0, len(mesh.positions), len(mesh.indices), len(mesh.subsets), subsets_id, 0
    )
    body += struct.pack("<16i", *streams)
    body += struct.pack("<4i", 0, 0, 0, 0)
    body += struct.pack("<6f", *low, *high)
    body += struct.pack("<2f", density, mean_area)
    body += b"\0" * (31 * 4)
    return body


def _node_chunk(name, object_id, material_id):
    encoded = name.encode("ascii")[:63]
    body = encoded + b"\0" * (64 - len(encoded))
    body += struct.pack("<4i", object_id, -1, 0, material_id)
    body += b"\0" * 4
    body += struct.pack("<16f", 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0)
    body += b"\0" * (12 + 16 + 12)
    body += struct.pack("<4i", -1, -1, -1, 0)
    return body


def check(mesh):
    """Raises ValueError for a mesh the game's file cannot hold."""
    if not mesh.positions or not mesh.indices:
        raise ValueError("the mesh has no triangles")
    if len(mesh.positions) > MAX_VERTICES:
        raise ValueError(f"the mesh has {len(mesh.positions)} vertices, at most {MAX_VERTICES}")
    if len(mesh.indices) % 3 != 0 or max(mesh.indices) >= len(mesh.positions):
        raise ValueError("the triangle indices do not fit the vertices")
    if not (len(mesh.uvs) == len(mesh.tangents) == len(mesh.positions)):
        raise ValueError("every vertex needs a uv and a tangent")
    if not mesh.subsets or not mesh.materials or len(mesh.physicalize) != len(mesh.materials):
        raise ValueError("the mesh needs a subset and a material for it")
    if max(abs(part) for point in mesh.positions for part in point) > 60000:
        raise ValueError("the mesh is too large for half-float positions")


def write_cgf(mesh, material_file, node_name="kcds_mesh"):
    """The .cgf file's bytes; `material_file` is the .mtl's game path without its extension
    (objects/kcds_studio/<name>)."""
    check(mesh)
    density, mean_area = _densities(mesh)
    # Chunk ids in the order the game's own split file has them.
    chunks = []

    def add(chunk_type, version, body):
        chunks.append((chunk_type, version, len(chunks) + 1, _pad4(body), len(body)))
        return len(chunks)

    material_id = add(0x1014, 0x802, _mtl_name_chunk(mesh, material_file))
    subsets_id = add(0x1017, 0x800, _subsets_chunk(mesh, density))
    vertices_id = add(0x1016, 0x800, _stream_chunk(STREAM_P3S_C4B_T2S, 16, _vertex_elements(mesh)))
    indices_id = add(
        0x1016,
        0x800,
        _stream_chunk(STREAM_INDICES, 2, [struct.pack("<H", i) for i in mesh.indices]),
    )
    tangents_id = add(0x1016, 0x800, _stream_chunk(STREAM_TANGENTS, 16, _tangent_elements(mesh)))
    stream_ids = {
        STREAM_INDICES: indices_id,
        STREAM_TANGENTS: tangents_id,
        STREAM_P3S_C4B_T2S: vertices_id,
    }
    mesh_id = add(0x1000, 0x801, _mesh_chunk(mesh, subsets_id, stream_ids, density, mean_area))
    add(0x100B, 0x824, _node_chunk(node_name, mesh_id, material_id))

    table_offset = 16
    data_offset = table_offset + 16 * len(chunks)
    table = b""
    data = b""
    for chunk_type, version, chunk_id, body, size in chunks:
        table += struct.pack("<HHIII", chunk_type, version, chunk_id, size, data_offset + len(data))
        data += body
    header = struct.pack("<4sIII", SIGNATURE, FILE_VERSION, len(chunks), table_offset)
    return header + table + data


def write_mtl(materials):
    """The .mtl's text: per material { name, colour (r, g, b 0..1), surface, texture, normal }; a
    texture (its game path, a .dds) is drawn in place of the colour, a normal map shapes its
    light."""
    lines = ['<Material MtlFlags="524544" vertModifType="0">', "  <SubMaterials>"]
    for material in materials:
        r, g, b = material.get("colour", (0.8, 0.8, 0.8))
        if material.get("texture"):
            r, g, b = 1.0, 1.0, 1.0
        surface = material.get("surface", "mat_wood")
        flags = "%NORMAL_MAP" if material.get("normal") else ""
        lines.append(
            f'    <Material Name="{material["name"]}" MtlFlags="524416" Shader="Illum" '
            f'GenMask="20000000000000" StringGenMask="{flags}" SurfaceType="{surface}" '
            f'Diffuse="{r:.4f},{g:.4f},{b:.4f}" Specular="0.04,0.04,0.04" Opacity="1" '
            f'Shininess="10" vertModifType="0" LayerAct="1">'
        )
        if material.get("texture") or material.get("normal"):
            lines.append("      <Textures>")
            if material.get("texture"):
                lines.append(f'        <Texture Map="Diffuse" File="{material["texture"]}" />')
            if material.get("normal"):
                lines.append(f'        <Texture Map="Bumpmap" File="{material["normal"]}" />')
            lines.append("      </Textures>")
        else:
            lines.append("      <Textures />")
        lines.append("    </Material>")
    lines.append("  </SubMaterials>")
    lines.append("</Material>")
    return "\n".join(lines) + "\n"


def _half(width, height, bgra):
    """The next mip level: each side halved (at least 1), each pixel the mean of the 2 x 2."""
    half_width = max(1, width // 2)
    half_height = max(1, height // 2)
    out = bytearray(half_width * half_height * 4)
    for y in range(half_height):
        for x in range(half_width):
            sums = [0, 0, 0, 0]
            count = 0
            for dy in (0, 1):
                for dx in (0, 1):
                    sx = x * 2 + dx
                    sy = y * 2 + dy
                    if sx >= width or sy >= height:
                        continue
                    at = (sy * width + sx) * 4
                    for part in range(4):
                        sums[part] += bgra[at + part]
                    count += 1
            at = (y * half_width + x) * 4
            for part in range(4):
                out[at + part] = (sums[part] + count // 2) // count
    return half_width, half_height, bytes(out)


def write_dds(width, height, bgra, srgb=False):
    """A picture (rows from the top, blue green red alpha) as a .dds the game loads: 32-bit
    BGRA without compression and every mip level down to 1 x 1, as the game client makes one
    from a .png (Client/runtime/studio/texture_dds.hpp). `srgb`: the colours are sRGB (CryEngine's
    marker 'CRYF' at byte 124, EIF_SRGBRead 0x800 at byte 36, an empty CExt ... CEnd after)."""
    if width <= 0 or height <= 0 or len(bgra) != width * height * 4:
        raise ValueError("the picture's size and pixels do not agree")
    mips = 1
    w, h = width, height
    while w > 1 or h > 1:
        w, h = max(1, w // 2), max(1, h // 2)
        mips += 1
    flags = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | 0x20000
    header = struct.pack("<4sIIIIIII", b"DDS ", 124, flags, height, width, width * 4, 0, mips)
    reserved = [0] * 11
    reserved[1] = 0x800 if srgb else 0
    header += struct.pack("<11I", *reserved)
    header += struct.pack(
        "<IIIIIIII", 32, 0x41, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000
    )
    header += struct.pack("<IIII", 0x8 | 0x1000 | 0x400000, 0, 0, 0) + b"CRYF"
    levels = [bytes(bgra)]
    w, h, level = width, height, bytes(bgra)
    for _ in range(mips - 1):
        w, h, level = _half(w, h, level)
        levels.append(level)
    return header + b"".join(levels) + b"CExtCEnd" + b"\0\0\0\0"


def _rgb565(r, g, b):
    return ((r * 31 + 127) // 255) << 11 | ((g * 63 + 127) // 255) << 5 | ((b * 31 + 127) // 255)


def _from565(value):
    r = (value >> 11) & 31
    g = (value >> 5) & 63
    b = value & 31
    return (r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31


def _bc1_block(pixels):
    """One 4 x 4 block (16 (r, g, b) tuples) as BC1's 8 bytes: the two end colours of the block's
    widest channel range, the four-colour palette between them, each pixel's nearest."""
    channel = max(range(3), key=lambda k: max(p[k] for p in pixels) - min(p[k] for p in pixels))
    high = max(pixels, key=lambda p: p[channel])
    low = min(pixels, key=lambda p: p[channel])
    c0 = _rgb565(*high)
    c1 = _rgb565(*low)
    if c0 < c1:
        c0, c1 = c1, c0
    if c0 == c1:
        return struct.pack("<HHI", c0, c1, 0)
    a = _from565(c0)
    b = _from565(c1)
    palette = [
        a,
        b,
        tuple((2 * a[k] + b[k] + 1) // 3 for k in range(3)),
        tuple((a[k] + 2 * b[k] + 1) // 3 for k in range(3)),
    ]
    bits = 0
    for index, pixel in enumerate(pixels):
        nearest = min(
            range(4), key=lambda i: sum((pixel[k] - palette[i][k]) ** 2 for k in range(3))
        )
        bits |= nearest << (2 * index)
    return struct.pack("<HHI", c0, c1, bits)


def _bc1_level(width, height, bgra):
    """A whole picture's BC1 blocks, rows of 4 x 4 from the top; edges repeat their last pixel."""
    out = bytearray()
    for by in range(0, height, 4):
        for bx in range(0, width, 4):
            pixels = []
            for y in range(by, by + 4):
                for x in range(bx, bx + 4):
                    at = (min(y, height - 1) * width + min(x, width - 1)) * 4
                    pixels.append((bgra[at + 2], bgra[at + 1], bgra[at]))
            out += _bc1_block(pixels)
    return bytes(out)


def write_dds_bc1(width, height, bgra, srgb=False):
    """A picture (rows from the top, blue green red alpha; alpha is dropped) as a BC1 (DXT1) .dds
    with every mip level, an eighth of write_dds's size, the compressed format the game's own
    opaque textures use. The same CryEngine marker and srgb flag as write_dds."""
    if width <= 0 or height <= 0 or len(bgra) != width * height * 4:
        raise ValueError("the picture's size and pixels do not agree")
    mips = 1
    w, h = width, height
    while w > 1 or h > 1:
        w, h = max(1, w // 2), max(1, h // 2)
        mips += 1
    top_size = max(1, (width + 3) // 4) * max(1, (height + 3) // 4) * 8
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000
    header = struct.pack("<4sIIIIIII", b"DDS ", 124, flags, height, width, top_size, 0, mips)
    reserved = [0] * 11
    reserved[1] = 0x800 if srgb else 0
    header += struct.pack("<11I", *reserved)
    header += struct.pack("<II4sIIIII", 32, 0x4, b"DXT1", 0, 0, 0, 0, 0)
    header += struct.pack("<IIII", 0x8 | 0x1000 | 0x400000, 0, 0, 0) + b"CRYF"
    levels = [_bc1_level(width, height, bgra)]
    w, h, level = width, height, bytes(bgra)
    for _ in range(mips - 1):
        w, h, level = _half(w, h, level)
        levels.append(_bc1_level(w, h, level))
    return header + b"".join(levels) + b"CExtCEnd" + b"\0\0\0\0"


def cube(size=0.5, material="kcds_cube"):
    """A test mesh: a cube standing on the ground, a side of `size` metres, flat sides."""
    mesh = Mesh()
    half = size / 2
    faces = [
        ((1, 0, 0), (0, 1, 0)),
        ((-1, 0, 0), (0, -1, 0)),
        ((0, 1, 0), (-1, 0, 0)),
        ((0, -1, 0), (1, 0, 0)),
        ((0, 0, 1), (1, 0, 0)),
        ((0, 0, -1), (-1, 0, 0)),
    ]
    for normal, across in faces:
        up = (
            normal[1] * across[2] - normal[2] * across[1],
            normal[2] * across[0] - normal[0] * across[2],
            normal[0] * across[1] - normal[1] * across[0],
        )
        first = len(mesh.positions)
        for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            point = tuple(
                normal[k] * half
                + across[k] * half * su
                + up[k] * half * sv
                + (half if k == 2 else 0)
                for k in range(3)
            )
            mesh.positions.append(point)
            mesh.uvs.append(((su + 1) / 2, 1 - (sv + 1) / 2))
            mesh.tangents.append((across, up, 1))
        mesh.indices += [first, first + 1, first + 2, first, first + 2, first + 3]
    mesh.subsets.append(Subset(0, len(mesh.indices), 0, len(mesh.positions), 0))
    mesh.materials.append(material)
    mesh.physicalize.append(PHYSICALIZE_DEFAULT)
    return mesh
