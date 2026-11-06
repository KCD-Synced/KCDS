"""A skinned glTF mesh as the game's .skin (kcds_gltf_skin.py): a small rig made here
(Root > Arm > Tip) and a quad skinned to it, bound to a two-bone skeleton of a skin (Root, Arm 1 m
up) as the game's skins carry it."""

import base64
import json
import os
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "blender"))

import kcds_gltf_skin  # noqa: E402
import kcds_skin  # noqa: E402
import test_kcds_skin  # noqa: E402


def translate(x, y, z):
    return [1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0, x, y, z, 1.0]


def skinned_quad(tip_name="Tip", root_name="mixamorig:Root", arm_name="mixamorig:Arm"):
    """A .gltf: Root at the ground, Arm 1.1 m up (glTF y), Tip 0.2 m above it; a quad standing in
    the xy plane, its bottom on Root, its top half on Arm, half on Tip."""
    positions = [(0, 0, 0), (1, 0, 0), (1, 1.1, 0), (0, 1.1, 0)]
    joints = [(0, 0, 0, 0), (0, 0, 0, 0), (1, 2, 0, 0), (1, 0, 0, 0)]
    weights = [(1, 0, 0, 0), (1, 0, 0, 0), (0.5, 0.5, 0, 0), (1, 0, 0, 0)]
    indices = [0, 1, 2, 0, 2, 3]
    data = b""
    views = []
    accessors = []

    def add(raw, component, count, kind):
        nonlocal data
        views.append({"buffer": 0, "byteOffset": len(data), "byteLength": len(raw)})
        accessors.append(
            {"bufferView": len(views) - 1, "componentType": component, "count": count, "type": kind}
        )
        data += raw + b"\0" * (-len(raw) % 4)
        return len(accessors) - 1

    position = add(b"".join(struct.pack("<3f", *p) for p in positions), 5126, 4, "VEC3")
    joint = add(b"".join(struct.pack("<4B", *j) for j in joints), 5121, 4, "VEC4")
    weight = add(b"".join(struct.pack("<4f", *w) for w in weights), 5126, 4, "VEC4")
    index = add(struct.pack("<6H", *indices), 5123, 6, "SCALAR")
    inverse = add(
        b"".join(
            struct.pack("<16f", *m)
            for m in (translate(0, 0, 0), translate(0, -1.1, 0), translate(0, -1.3, 0))
        ),
        5126,
        3,
        "MAT4",
    )
    return {
        "asset": {"version": "2.0"},
        "scenes": [{"nodes": [0, 3]}],
        "nodes": [
            {"name": root_name, "children": [1]},
            {"name": arm_name, "translation": [0, 1.1, 0], "children": [2]},
            {"name": tip_name, "translation": [0, 0.2, 0]},
            {"name": "Quad", "mesh": 0, "skin": 0},
        ],
        "skins": [{"joints": [0, 1, 2], "inverseBindMatrices": inverse}],
        "meshes": [
            {
                "primitives": [
                    {
                        "attributes": {
                            "POSITION": position,
                            "JOINTS_0": joint,
                            "WEIGHTS_0": weight,
                        },
                        "indices": index,
                    }
                ]
            }
        ],
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [
            {
                "uri": "data:application/octet-stream;base64," + base64.b64encode(data).decode(),
                "byteLength": len(data),
            }
        ],
    }


class GltfSkinTest(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.mkdtemp()
        self.template = os.path.join(self.folder, "template.skin")
        with open(self.template, "wb") as file:
            file.write(test_kcds_skin.game_skin_with_two_bones())

    def convert(self, document, given=None):
        path = os.path.join(self.folder, "rig.gltf")
        with open(path, "w", encoding="utf-8") as file:
            json.dump(document, file)
        return kcds_gltf_skin.convert(path, "quad", self.folder, given, template=self.template)

    def written(self):
        with open(os.path.join(self.folder, "quad.skin"), "rb") as file:
            return kcds_skin.read_chunks(file.read())

    def test_joints_match_the_game_bones_by_name(self):
        made = self.convert(skinned_quad())
        self.assertEqual(made["mapped"], {"Root": "mixamorig:Root", "Arm": "mixamorig:Arm"})
        self.assertEqual(made["bones"], 2)
        self.assertEqual(made["vertices"], 4)
        self.assertEqual(made["triangles"], 2)

    def test_vertices_stand_in_the_body_frame_moved_onto_the_game_bones(self):
        made = self.convert(skinned_quad())
        positions = [struct.unpack("<3f", e) for e in test_kcds_skin.stream(self.written(), 0)]
        # glTF (1, 1.1, 0) is right -1, up 1.1 in the body; the file's Arm is 0.1 m above the
        # game's, so a vertex on Arm comes down 0.1 m.
        expected = [(0, 0, 0), (-1, 0, 0), (-1, 0, 1.0), (0, 0, 1.0)]
        for got, want in zip(positions, expected):
            for k in range(3):
                self.assertAlmostEqual(got[k], want[k], places=4)
        self.assertAlmostEqual(made["moved"], 0.1, places=4)

    def test_a_joint_without_a_bone_weighs_on_its_matching_ancestor(self):
        self.convert(skinned_quad())
        mapping = test_kcds_skin.stream(self.written(), kcds_skin.STREAM_BONE_MAPPING)
        top = mapping[2]
        ids = struct.unpack_from("<4H", top, 0)
        weights = struct.unpack_from("<4B", top, 8)
        self.assertEqual((ids[0], weights[0]), (1, 255))
        bottom = mapping[0]
        self.assertEqual((struct.unpack_from("<H", bottom, 0)[0], bottom[8]), (0, 255))

    def test_map_names_a_joint_the_matching_misses(self):
        made = self.convert(skinned_quad(arm_name="Forearm_L"), given={"Arm": "Forearm_L"})
        self.assertEqual(made["mapped"]["Arm"], "Forearm_L")

    def test_a_joint_with_no_bone_above_it_is_refused_by_name(self):
        with self.assertRaisesRegex(kcds_gltf_skin.GltfError, "the joint Pelvis matches no bone"):
            self.convert(skinned_quad(root_name="Pelvis", arm_name="Spine9"))

    def test_a_file_without_a_skin_is_refused(self):
        document = skinned_quad()
        del document["nodes"][3]["skin"]
        with self.assertRaisesRegex(kcds_gltf_skin.GltfError, "no skinned mesh"):
            self.convert(document)

    def test_the_material_file_is_written_beside_it(self):
        made = self.convert(skinned_quad())
        self.assertTrue(made["files"][0].endswith("quad.skin"))
        with open(made["files"][1], encoding="ascii") as file:
            self.assertIn("<Material", file.read())


if __name__ == "__main__":
    unittest.main()
