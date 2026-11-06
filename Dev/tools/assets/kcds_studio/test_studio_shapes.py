"""The studio's simple props (studio_shapes.py): their size, that every triangle faces
out the way its vertices' tangent frames say, and the files and refusals of the command line."""

import contextlib
import io
import json
import math
import os
import struct
import tempfile
import unittest

import studio_shapes


def _sub(a, b):
    return tuple(a[k] - b[k] for k in range(3))


def _dot(a, b):
    return sum(a[k] * b[k] for k in range(3))


def _bounds(mesh):
    low = [min(point[axis] for point in mesh.positions) for axis in range(3)]
    high = [max(point[axis] for point in mesh.positions) for axis in range(3)]
    return low, high


class ShapesTest(unittest.TestCase):
    def assert_faces_out(self, mesh, middle):
        """Each triangle turns counter-clockwise to its front: its normal agrees with its
        vertices' tangent x bitangent, and points away from the shape's middle."""
        for at in range(0, len(mesh.indices), 3):
            a, b, c = mesh.indices[at : at + 3]
            pa, pb, pc = mesh.positions[a], mesh.positions[b], mesh.positions[c]
            face = studio_shapes._cross(_sub(pb, pa), _sub(pc, pa))
            self.assertGreater(math.sqrt(_dot(face, face)), 0, f"triangle {at // 3} is flat")
            for vertex in (a, b, c):
                tangent, bitangent, sign = mesh.tangents[vertex]
                self.assertEqual(sign, 1)
                frame = studio_shapes._cross(tangent, bitangent)
                self.assertGreater(_dot(face, frame), 0, f"triangle {at // 3} faces in")
            if middle is not None:
                centre = tuple((pa[k] + pb[k] + pc[k]) / 3 for k in range(3))
                self.assertGreater(_dot(face, _sub(centre, middle)), 0)

    def assert_uv_frame(self, mesh):
        """Along a triangle's edges u grows where the tangent runs and v falls where the
        bitangent runs (the game's v runs down)."""
        for at in range(0, len(mesh.indices), 3):
            a, b, _ = mesh.indices[at : at + 3]
            edge = _sub(mesh.positions[b], mesh.positions[a])
            du = mesh.uvs[b][0] - mesh.uvs[a][0]
            dv = mesh.uvs[b][1] - mesh.uvs[a][1]
            tangent, bitangent, _ = mesh.tangents[a]
            if abs(du) > 1e-6 and abs(_dot(edge, tangent)) > 1e-6:
                self.assertEqual(du > 0, _dot(edge, tangent) > 0)
            if abs(dv) > 1e-6 and abs(_dot(edge, bitangent)) > 1e-6:
                self.assertEqual(dv < 0, _dot(edge, bitangent) > 0)

    def test_box(self):
        mesh = studio_shapes.box(0.4, 1.2, 0.3)
        self.assertEqual(len(mesh.positions), 24)
        self.assertEqual(len(mesh.indices), 36)
        low, high = _bounds(mesh)
        for axis, (want_low, want_high) in enumerate(((-0.2, 0.2), (-0.6, 0.6), (0.0, 0.3))):
            self.assertAlmostEqual(low[axis], want_low)
            self.assertAlmostEqual(high[axis], want_high)
        self.assert_faces_out(mesh, (0, 0, 0.15))
        self.assert_uv_frame(mesh)

    def test_cylinder(self):
        mesh = studio_shapes.cylinder(0.3, 0.8, sides=16)
        self.assertEqual(len(mesh.positions), 2 * 17 + 2 * 17)
        self.assertEqual(len(mesh.indices) // 3, 16 * 2 + 16 * 2)
        low, high = _bounds(mesh)
        self.assertAlmostEqual(low[2], 0)
        self.assertAlmostEqual(high[2], 0.8)
        self.assertAlmostEqual(high[0], 0.3)
        self.assert_faces_out(mesh, (0, 0, 0.4))
        self.assert_uv_frame(mesh)

    def test_sphere(self):
        mesh = studio_shapes.sphere(0.5, rings=8, sides=12)
        self.assertEqual(len(mesh.positions), 9 * 13)
        # Two triangles a quad, one in the rings at the top and the bottom.
        self.assertEqual(len(mesh.indices) // 3, 12 * (2 * 8 - 2))
        low, high = _bounds(mesh)
        self.assertAlmostEqual(low[2], 0)
        self.assertAlmostEqual(high[2], 1.0)
        for point in mesh.positions:
            self.assertAlmostEqual(math.dist(point, (0, 0, 0.5)), 0.5)
        self.assert_faces_out(mesh, (0, 0, 0.5))
        self.assert_uv_frame(mesh)

    def test_plane(self):
        mesh = studio_shapes.plane(2.0, 3.0)
        self.assertEqual(len(mesh.indices), 6)
        low, high = _bounds(mesh)
        self.assertEqual((low[2], high[2]), (0, 0))
        self.assertAlmostEqual(high[1] - low[1], 3.0)
        self.assert_faces_out(mesh, (0, 0, -1))
        self.assert_uv_frame(mesh)

    def test_make_checks_the_options(self):
        refusals = [
            ({"shape": "cone"}, "shape is one of"),
            ({"shape": "box", "size": [1, 1]}, "size is 3"),
            ({"shape": "box", "size": [1, 1, 0]}, "from 0.01 to 50"),
            ({"shape": "cylinder", "sides": 2}, "sides is a whole number from 3 to 128"),
            ({"shape": "sphere", "rings": 1.5}, "rings is a whole number"),
            ({"shape": "plane", "colour": [1, 2, 0]}, "colour is"),
            ({"shape": "plane", "surface": "mat_lava"}, "surface is one of"),
            ({"shape": "plane", "texture": "x"}, "unknown texture"),
        ]
        for options, message in refusals:
            with self.assertRaises(ValueError) as caught:
                studio_shapes.make("one", options)
            self.assertIn(message, str(caught.exception), options)

    def test_material_has_the_colour_and_surface(self):
        _, material = studio_shapes.make(
            "crate", {"shape": "box", "colour": [0.5, 0.25, 0], "surface": "mat_metal"}
        )
        self.assertIn('Name="crate"', material)
        self.assertIn('Diffuse="0.5000,0.2500,0.0000"', material)
        self.assertIn('SurfaceType="mat_metal"', material)

    def test_command_line_writes_the_model_and_material(self):
        with tempfile.TemporaryDirectory() as folder:
            options = json.dumps({"shape": "cylinder", "radius": 0.1, "height": 1.5})
            printed = io.StringIO()
            with contextlib.redirect_stdout(printed):
                self.assertEqual(studio_shapes.main(["post", options, folder]), 0)
                self.assertEqual(studio_shapes.main(["bad name", options, folder]), 1)
            answers = [json.loads(line) for line in printed.getvalue().splitlines()]
            self.assertEqual((answers[0]["vertices"], answers[0]["triangles"]), (100, 96))
            self.assertIn("the name is", answers[1]["error"])
            with open(os.path.join(folder, "post.cgf"), "rb") as file:
                signature, version, chunks, _ = struct.unpack("<4sIII", file.read(16))
            self.assertEqual((signature, version, chunks), (b"CrCh", 0x746, 7))
            with open(os.path.join(folder, "post.mtl"), encoding="ascii") as file:
                self.assertIn('SurfaceType="mat_wood"', file.read())
            self.assertFalse(os.path.exists(os.path.join(folder, "bad name.cgf")))


if __name__ == "__main__":
    unittest.main()
