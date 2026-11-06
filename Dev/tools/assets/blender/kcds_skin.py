"""The game's own skinned mesh files (.skin) for the Asset/Animation Studio.

A .skin is a CryEngine chunk file ("CrCh" 0x746) like a .cgf (kcds_cgf.py), with the skeleton the
mesh is bound to and the skinning compiled in. The chunks and versions are those of the game's own
skins (read from Objects/characters/.../boundary_torso_workcoat_01.skin and checked against the
loaders in WHGame.dll 1.5.5):

  MtlName                0x1014 v0x802  as kcds_cgf
  CompiledBones          0x2000 v0x800  32 reserved bytes, then 584 bytes a bone (loader
                                        ): controller id (CRC-32 of the name), physics
                                        of two LODs (2 x 104), mass, world-to-bone and
                                        bone-to-world (3x4, rows), name[256], limb id, parent
                                        offset, child count, first child offset
  CompiledPhysicalBones  0x2001 v0x800  32 reserved bytes, then 152 bytes a bone
  CompiledIntSkinVertices 0x2005 v0x800 32 reserved bytes, then 64 bytes a vertex:
                                        3 reserved floats, position, 3 reserved floats, 4 bone
                                        ids (16 bits), 4 weights (floats), colour RGBA
  CompiledIntFaces       0x2004 v0x800  3 x 16-bit vertex indices a face
  CompiledExt2IntMap     0x2006 v0x800  16 bits a render vertex: its skin vertex
  BonesBoxes             0x3004 v0x801  a bone: id, its vertices' box, count, 16-bit vertex
                                        indices
  MeshSubsets            0x1017 v0x800  as kcds_cgf, without densities (flags 0)
  DataStream             0x1016 v0x800  0 positions (3 floats), 1 normals (3 floats),
                                        2 uvs (2 floats), 3 colours (RGBA), 5 indices (16 bits),
                                        6 tangents (as kcds_cgf), 9 bone mapping (4 x 16-bit bone
                                        ids, 4 x 8-bit weights summing to 255)
  Mesh                   0x1000 v0x801  as kcds_cgf
  Node                   0x100B v0x824  as kcds_cgf

The skeleton comes from one of the game's own skins bound to the character's skeleton (a
Skeleton read from its bytes): its bone chunks are written unchanged, so the new mesh binds to the
same bones in the same rest pose. The game's files are read from the player's own installation,
never copied into KCDS. Plain Python (struct and zlib), so Blender's own Python runs it.
"""

import struct
import zlib

import kcds_cgf

CHUNK_MTL_NAME = 0x1014
CHUNK_COMPILED_BONES = 0x2000
CHUNK_COMPILED_PHYSICAL_BONES = 0x2001
CHUNK_INT_SKIN_VERTICES = 0x2005
CHUNK_INT_FACES = 0x2004
CHUNK_EXT_TO_INT = 0x2006
CHUNK_BONE_BOXES = 0x3004
CHUNK_SUBSETS = 0x1017
CHUNK_DATA_STREAM = 0x1016
CHUNK_MESH = 0x1000
CHUNK_NODE = 0x100B

STREAM_POSITIONS = 0
STREAM_NORMALS = 1
STREAM_UVS = 2
STREAM_COLOURS = 3
STREAM_INDICES = 5
STREAM_TANGENTS = 6
STREAM_BONE_MAPPING = 9

BONE_BYTES = 584
RESERVED_HEADER = 32
MAX_INFLUENCES = 4


class SkinError(ValueError):
    """A mesh, a skeleton or a file the game's skin cannot be made from."""


class Chunk:
    def __init__(self, kind, version, chunk_id, body):
        self.kind = kind
        self.version = version
        self.chunk_id = chunk_id
        self.body = body


def read_chunks(data):
    """The chunks of a chunk file's bytes, in table order."""
    if len(data) < 16 or data[:4] != kcds_cgf.SIGNATURE:
        raise SkinError("not a CryEngine chunk file")
    _, version, count, table = struct.unpack_from("<4sIII", data, 0)
    if version != kcds_cgf.FILE_VERSION or table + 16 * count > len(data):
        raise SkinError("unknown chunk file version or a cut table")
    chunks = []
    for index in range(count):
        kind, kind_version, chunk_id, size, offset = struct.unpack_from(
            "<HHIII", data, table + 16 * index
        )
        if offset + size > len(data):
            raise SkinError(f"chunk {chunk_id} runs past the end of the file")
        chunks.append(Chunk(kind, kind_version, chunk_id, data[offset : offset + size]))
    return chunks


class Bone:
    def __init__(self, index, name, controller, bone_to_world):
        self.index = index
        self.name = name
        self.controller = controller
        self.bone_to_world = bone_to_world  # 12 floats, 3 rows of (x axis, y axis, z axis, t)

    @property
    def position(self):
        return (self.bone_to_world[3], self.bone_to_world[7], self.bone_to_world[11])


class Skeleton:
    """The bones a game skin is bound to, and their compiled chunks as the game wrote them."""

    def __init__(self, bones_body, physical_body):
        if (len(bones_body) - RESERVED_HEADER) % BONE_BYTES != 0 or len(
            bones_body
        ) <= RESERVED_HEADER:
            raise SkinError("the compiled bones chunk has no whole bones")
        self.bones_body = bones_body
        self.physical_body = physical_body
        self.bones = []
        count = (len(bones_body) - RESERVED_HEADER) // BONE_BYTES
        for index in range(count):
            at = RESERVED_HEADER + index * BONE_BYTES
            controller = struct.unpack_from("<I", bones_body, at)[0]
            bone_to_world = struct.unpack_from("<12f", bones_body, at + 264)
            raw_name = bones_body[at + 312 : at + 568].split(b"\0")[0]
            name = raw_name.decode("ascii", "replace")
            self.bones.append(Bone(index, name, controller, bone_to_world))
        self.by_name = {bone.name: bone for bone in self.bones}
        self.by_lower = {bone.name.lower(): bone for bone in self.bones}

    @classmethod
    def from_skin(cls, data):
        bones = physical = None
        for chunk in read_chunks(data):
            if chunk.kind == CHUNK_COMPILED_BONES and chunk.version == 0x800:
                bones = chunk.body
            elif chunk.kind == CHUNK_COMPILED_PHYSICAL_BONES and chunk.version == 0x800:
                physical = chunk.body
        if bones is None:
            raise SkinError("the file has no compiled bones (not a compiled .skin)")
        return cls(bones, physical)

    def bone(self, name):
        found = self.by_name.get(name) or self.by_lower.get(name.lower())
        if found is None:
            raise SkinError(f"the skeleton has no bone '{name}'")
        return found


class SkinnedMesh(kcds_cgf.Mesh):
    """A kcds_cgf.Mesh with per vertex a normal and up to four (bone name, weight) pairs."""

    def __init__(self):
        super().__init__()
        self.normals = []  # (x, y, z), unit
        self.weights = []  # [(bone name, weight), ...]


def _influences(mesh, skeleton):
    """Per vertex four (bone index, float weight) pairs summing to 1, heaviest first."""
    out = []
    for vertex, pairs in enumerate(mesh.weights):
        merged = {}
        for name, weight in pairs:
            if weight > 0:
                index = skeleton.bone(name).index
                merged[index] = merged.get(index, 0.0) + weight
        strongest = sorted(merged.items(), key=lambda item: (-item[1], item[0]))[:MAX_INFLUENCES]
        total = sum(weight for _, weight in strongest)
        if total <= 0:
            raise SkinError(f"vertex {vertex} has no bone weight")
        strongest = [(index, weight / total) for index, weight in strongest]
        while len(strongest) < MAX_INFLUENCES:
            strongest.append((0, 0.0))
        out.append(strongest)
    return out


def _bytes_weights(influence):
    """The four float weights as 8-bit weights summing to exactly 255."""
    raw = [weight * 255.0 for _, weight in influence]
    whole = [int(value) for value in raw]
    rest = 255 - sum(whole)
    order = sorted(range(MAX_INFLUENCES), key=lambda k: -(raw[k] - whole[k]))
    for k in order[:rest]:
        whole[k] += 1
    return whole


def check(mesh):
    kcds_cgf.check(mesh)
    if len(mesh.normals) != len(mesh.positions) or len(mesh.weights) != len(mesh.positions):
        raise SkinError("every vertex needs a normal and its bone weights")


def _stream(stream_type, element_size, elements):
    return kcds_cgf._stream_chunk(stream_type, element_size, elements)


def _int_vertices(mesh, influences):
    body = b"\0" * RESERVED_HEADER
    for position, influence in zip(mesh.positions, influences):
        body += struct.pack("<3f", 0.0, 0.0, 0.0)
        body += struct.pack("<3f", *position)
        body += struct.pack("<3f", 0.0, 0.0, 0.0)
        body += struct.pack("<4H", *(index for index, _ in influence))
        body += struct.pack("<4f", *(weight for _, weight in influence))
        body += struct.pack("<4B", 255, 255, 255, 255)
    return body


def _bone_boxes(mesh, influences):
    """Per bone that moves a vertex: (bone index, the box chunk's body)."""
    members = {}
    for vertex, influence in enumerate(influences):
        for index, weight in influence:
            if weight > 0:
                members.setdefault(index, []).append(vertex)
    out = []
    for index in sorted(members):
        vertices = members[index]
        low, high = kcds_cgf._bounds([mesh.positions[v] for v in vertices])
        body = struct.pack("<I6fi", index, *low, *high, len(vertices))
        body += struct.pack(f"<{len(vertices)}H", *vertices)
        out.append((index, body))
    return out


def _bone_mapping(influences):
    out = []
    for influence in influences:
        weights = _bytes_weights(influence)
        out.append(struct.pack("<4H4B", *(index for index, _ in influence), *weights))
    return out


def _subsets(mesh):
    body = struct.pack("<4i", 0, len(mesh.subsets), 0, 0)
    for subset in mesh.subsets:
        points = mesh.positions[subset.first_vertex : subset.first_vertex + subset.vertex_count]
        low, high = kcds_cgf._bounds(points)
        centre = [(low[k] + high[k]) / 2 for k in range(3)]
        radius = sum((high[k] - centre[k]) ** 2 for k in range(3)) ** 0.5
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
    return body


def write_skin(mesh, skeleton, material_file, node_name="kcds_skin"):
    """The .skin file's bytes, bound to `skeleton`; `material_file` is the .mtl's game path
    without its extension."""
    check(mesh)
    influences = _influences(mesh, skeleton)
    density, mean_area = kcds_cgf._densities(mesh)
    chunks = []

    def add(kind, version, body):
        chunks.append((kind, version, len(chunks) + 1, kcds_cgf._pad4(body), len(body)))
        return len(chunks)

    material_id = add(CHUNK_MTL_NAME, 0x802, kcds_cgf._mtl_name_chunk(mesh, material_file))
    add(CHUNK_COMPILED_BONES, 0x800, skeleton.bones_body)
    if skeleton.physical_body is not None:
        add(CHUNK_COMPILED_PHYSICAL_BONES, 0x800, skeleton.physical_body)
    add(CHUNK_INT_SKIN_VERTICES, 0x800, _int_vertices(mesh, influences))
    add(CHUNK_INT_FACES, 0x800, struct.pack(f"<{len(mesh.indices)}H", *mesh.indices))
    vertices = len(mesh.positions)
    add(CHUNK_EXT_TO_INT, 0x800, struct.pack(f"<{vertices}H", *range(vertices)))
    for _, body in _bone_boxes(mesh, influences):
        add(CHUNK_BONE_BOXES, 0x801, body)
    subsets_id = add(CHUNK_SUBSETS, 0x800, _subsets(mesh))
    streams = {
        STREAM_POSITIONS: (12, [struct.pack("<3f", *p) for p in mesh.positions]),
        STREAM_NORMALS: (12, [struct.pack("<3f", *n) for n in mesh.normals]),
        STREAM_UVS: (8, [struct.pack("<2f", *uv) for uv in mesh.uvs]),
        STREAM_COLOURS: (4, [struct.pack("<4B", 255, 255, 255, 255)] * vertices),
        STREAM_INDICES: (2, [struct.pack("<H", i) for i in mesh.indices]),
        STREAM_TANGENTS: (16, kcds_cgf._tangent_elements(mesh)),
        STREAM_BONE_MAPPING: (12, _bone_mapping(influences)),
    }
    stream_ids = {}
    for stream_type, (size, elements) in streams.items():
        stream_ids[stream_type] = add(
            CHUNK_DATA_STREAM, 0x800, _stream(stream_type, size, elements)
        )
    mesh_body = kcds_cgf._mesh_chunk(mesh, subsets_id, stream_ids, density, mean_area)
    mesh_id = add(CHUNK_MESH, 0x801, mesh_body)
    add(CHUNK_NODE, 0x824, kcds_cgf._node_chunk(node_name, mesh_id, material_id))

    table_offset = 16
    data_offset = table_offset + 16 * len(chunks)
    table = b""
    data = b""
    for kind, version, chunk_id, body, size in chunks:
        table += struct.pack("<HHIII", kind, version, chunk_id, size, data_offset + len(data))
        data += body
    header = struct.pack(
        "<4sIII", kcds_cgf.SIGNATURE, kcds_cgf.FILE_VERSION, len(chunks), table_offset
    )
    return header + table + data


def controller_id(name):
    """A bone's controller id as the game's compiled bones hold it: the CRC-32 of its name."""
    return zlib.crc32(name.encode("ascii")) & 0xFFFFFFFF
