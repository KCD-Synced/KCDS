"""Tests of kcds_skin.py: the .skin file the game reads, bound to a skeleton of a game skin."""

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import kcds_cgf  # noqa: E402
import kcds_skin  # noqa: E402


def bone_record(name, position, parent_offset, children, first_child):
    """One 584-byte compiled bone, as the game's skins hold it."""
    record = struct.pack("<I", kcds_skin.controller_id(name))
    record += b"\0" * (2 * 104)
    record += struct.pack("<f", 1.0)
    x, y, z = position
    record += struct.pack("<12f", 1, 0, 0, -x, 0, 1, 0, -y, 0, 0, 1, -z)
    record += struct.pack("<12f", 1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z)
    encoded = name.encode("ascii")
    record += encoded + b"\0" * (256 - len(encoded))
    record += struct.pack("<4i", -1, parent_offset, children, first_child)
    assert len(record) == kcds_skin.BONE_BYTES
    return record


def game_skin_with_two_bones():
    """A chunk file with only the bone chunks of a skin: Root and Arm (1 m up)."""
    bones = (
        b"\0" * 32
        + bone_record("Root", (0, 0, 0), 0, 1, 1)
        + bone_record("Arm", (0, 0, 1), -1, 0, 0)
    )
    physical = b"\0" * 32 + b"\1" * (2 * 152)
    chunks = [(0x2000, 0x800, 1, bones), (0x2001, 0x800, 2, physical)]
    table = b""
    data = b""
    offset = 16 + 16 * len(chunks)
    for kind, version, chunk_id, body in chunks:
        table += struct.pack("<HHIII", kind, version, chunk_id, len(body), offset + len(data))
        data += body
    return struct.pack("<4sIII", b"CrCh", 0x746, len(chunks), 16) + table + data


def quad():
    """A 1 m square standing on the ground: bottom vertices on Root, top ones on Arm, the middle
    of the top edge shared."""
    mesh = kcds_skin.SkinnedMesh()
    mesh.positions = [(0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1)]
    mesh.normals = [(0, -1, 0)] * 4
    mesh.uvs = [(0, 1), (1, 1), (1, 0), (0, 0)]
    mesh.tangents = [((1, 0, 0), (0, 0, 1), 1)] * 4
    mesh.indices = [0, 1, 2, 0, 2, 3]
    mesh.subsets = [kcds_cgf.Subset(0, 6, 0, 4, 0)]
    mesh.materials = ["kcds_quad"]
    mesh.physicalize = [kcds_cgf.PHYSICALIZE_DEFAULT]
    mesh.weights = [
        [("Root", 1.0)],
        [("Root", 1.0)],
        [("Arm", 2.0), ("Root", 1.0)],
        [("arm", 1.0)],
    ]
    return mesh


def stream(chunks, stream_type):
    for chunk in chunks:
        if chunk.kind == kcds_skin.CHUNK_DATA_STREAM:
            kind, count, size = struct.unpack_from("<3i", chunk.body, 4)
            if kind == stream_type:
                return [chunk.body[24 + i * size : 24 + (i + 1) * size] for i in range(count)]
    raise AssertionError(f"no stream {stream_type}")


class SkinTest(unittest.TestCase):
    def setUp(self):
        self.skeleton = kcds_skin.Skeleton.from_skin(game_skin_with_two_bones())
        self.data = kcds_skin.write_skin(quad(), self.skeleton, "objects/kcds_studio/kcds_quad")
        self.chunks = kcds_skin.read_chunks(self.data)

    def test_skeleton_is_read_from_the_game_skin(self):
        self.assertEqual([bone.name for bone in self.skeleton.bones], ["Root", "Arm"])
        self.assertEqual(self.skeleton.bones[1].position, (0, 0, 1))
        self.assertEqual(self.skeleton.bones[1].controller, kcds_skin.controller_id("Arm"))

    def test_chunks_are_those_of_the_game_skins(self):
        kinds = [(chunk.kind, chunk.version) for chunk in self.chunks]
        self.assertEqual(
            kinds,
            [
                (0x1014, 0x802),
                (0x2000, 0x800),
                (0x2001, 0x800),
                (0x2005, 0x800),
                (0x2004, 0x800),
                (0x2006, 0x800),
                (0x3004, 0x801),
                (0x3004, 0x801),
                (0x1017, 0x800),
            ]
            + [(0x1016, 0x800)] * 7
            + [(0x1000, 0x801), (0x100B, 0x824)],
        )

    def test_bone_chunks_are_the_skeleton_unchanged(self):
        self.assertEqual(self.chunks[1].body, self.skeleton.bones_body)
        self.assertEqual(self.chunks[2].body, self.skeleton.physical_body)

    def test_skin_vertices_hold_positions_and_normalised_weights(self):
        body = self.chunks[3].body
        self.assertEqual(len(body), 32 + 4 * 64)
        third = struct.unpack_from("<9f4H4f4B", body, 32 + 2 * 64)
        self.assertEqual(third[3:6], (1.0, 0.0, 1.0))
        self.assertEqual(third[9:11], (1, 0))
        self.assertAlmostEqual(third[13], 2 / 3, places=6)
        self.assertAlmostEqual(third[14], 1 / 3, places=6)

    def test_faces_and_the_map_are_one_to_one(self):
        self.assertEqual(struct.unpack("<6H", self.chunks[4].body), (0, 1, 2, 0, 2, 3))
        self.assertEqual(struct.unpack("<4H", self.chunks[5].body), (0, 1, 2, 3))

    def test_bone_mapping_weights_sum_to_255(self):
        for element in stream(self.chunks, kcds_skin.STREAM_BONE_MAPPING):
            ids = struct.unpack_from("<4H", element, 0)
            weights = struct.unpack_from("<4B", element, 8)
            self.assertEqual(sum(weights), 255)
            self.assertTrue(all(bone in (0, 1) for bone in ids))
        third = stream(self.chunks, kcds_skin.STREAM_BONE_MAPPING)[2]
        self.assertEqual(struct.unpack_from("<4B", third, 8), (170, 85, 0, 0))

    def test_bone_boxes_list_the_vertices_each_bone_moves(self):
        root = self.chunks[6].body
        bone, *box = struct.unpack_from("<I6f", root, 0)
        count = struct.unpack_from("<i", root, 28)[0]
        self.assertEqual(bone, 0)
        self.assertEqual(struct.unpack_from(f"<{count}H", root, 32), (0, 1, 2))
        self.assertEqual(box, [0, 0, 0, 1, 0, 1])
        arm = self.chunks[7].body
        count = struct.unpack_from("<i", arm, 28)[0]
        self.assertEqual(struct.unpack_from(f"<{count}H", arm, 32), (2, 3))

    def test_render_streams_are_full_floats(self):
        positions = stream(self.chunks, kcds_skin.STREAM_POSITIONS)
        self.assertEqual(struct.unpack("<3f", positions[2]), (1.0, 0.0, 1.0))
        normals = stream(self.chunks, kcds_skin.STREAM_NORMALS)
        self.assertEqual(struct.unpack("<3f", normals[0]), (0.0, -1.0, 0.0))
        self.assertEqual(len(stream(self.chunks, kcds_skin.STREAM_UVS)[0]), 8)

    def test_an_unknown_bone_is_refused_by_name(self):
        mesh = quad()
        mesh.weights[0] = [("Tail", 1.0)]
        with self.assertRaisesRegex(kcds_skin.SkinError, "no bone 'Tail'"):
            kcds_skin.write_skin(mesh, self.skeleton, "objects/kcds_studio/kcds_quad")

    def test_a_vertex_without_weight_is_refused(self):
        mesh = quad()
        mesh.weights[1] = []
        with self.assertRaisesRegex(kcds_skin.SkinError, "vertex 1 has no bone weight"):
            kcds_skin.write_skin(mesh, self.skeleton, "objects/kcds_studio/kcds_quad")

    def test_a_file_without_bones_is_no_skeleton(self):
        plain = struct.pack("<4sIII", b"CrCh", 0x746, 0, 16)
        with self.assertRaisesRegex(kcds_skin.SkinError, "no compiled bones"):
            kcds_skin.Skeleton.from_skin(plain)


if __name__ == "__main__":
    unittest.main()
