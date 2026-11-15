"""Tests of make_workpiece.py, without the game: the workpiece's placement and morph, the tangent
frame the game rebuilds its normals from, and the glow's fade."""

import math
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import make_workpiece  # noqa: E402


def dome():
    """A small dome: a ring of eight vertices around a raised middle."""
    positions = [(0.0, 0.0, 0.5)]
    uvs = [(0.5, 0.5)]
    for k in range(8):
        angle = k * math.pi / 4
        positions.append((math.cos(angle), math.sin(angle), 0.0))
        uvs.append((0.5 + 0.5 * math.cos(angle), 0.5 - 0.5 * math.sin(angle)))
    indices = []
    for k in range(8):
        indices += [0, 1 + k, 1 + (k + 1) % 8]
    return positions, uvs, indices


AXE_BOX = ([-0.1, -0.04], [0.07, 0.17])


class WorkpieceTest(unittest.TestCase):
    def setUp(self):
        positions, uvs, indices = dome()
        self.mesh, self.deltas = make_workpiece.workpiece_mesh(
            positions, uvs, indices, AXE_BOX, 1.2, 0.25, 0
        )

    def test_the_piece_lies_on_the_axe_piece_and_covers_its_strike_map(self):
        xs = [p[0] for p in self.mesh.positions]
        ys = [p[1] for p in self.mesh.positions]
        self.assertAlmostEqual((min(xs) + max(xs)) / 2, -0.015, places=6)
        self.assertAlmostEqual((min(ys) + max(ys)) / 2, 0.065, places=6)
        self.assertAlmostEqual(max(ys) - min(ys), 1.2 * 0.21, places=6)
        self.assertGreaterEqual(min(p[2] for p in self.mesh.positions), 0.0)

    def test_the_morph_raises_the_flattened_sheet_to_the_model(self):
        middle = self.mesh.positions[0]
        shaped = [middle[k] + self.deltas[0][k] for k in range(3)]
        self.assertAlmostEqual(shaped[2], middle[2] / 0.25, places=6)
        for delta in self.deltas:
            self.assertEqual((delta[0], delta[1]), (0.0, 0.0))
            self.assertGreaterEqual(delta[2], 0.0)

    def test_the_game_rebuilds_each_vertex_normal_from_its_tangent_frame(self):
        for normal, (t, b, sign) in zip(self.mesh.normals, self.mesh.tangents):
            cross = (
                t[1] * b[2] - t[2] * b[1],
                t[2] * b[0] - t[0] * b[2],
                t[0] * b[1] - t[1] * b[0],
            )
            rebuilt = [part * sign for part in cross]
            self.assertGreater(sum(rebuilt[k] * normal[k] for k in range(3)), 0.99)

    def test_the_glow_is_full_in_the_middle_and_fades_to_the_rim(self):
        self.assertEqual(make_workpiece.glow_alpha((0, 0, 0), (0, 0), 1.0), 255)
        self.assertEqual(make_workpiece.glow_alpha((0.3, 0, 0), (0, 0), 1.0), 255)
        self.assertEqual(make_workpiece.glow_alpha((1.0, 0, 0), (0, 0), 1.0), 85)
        middle = make_workpiece.glow_alpha((0.7, 0, 0), (0, 0), 1.0)
        self.assertTrue(85 < middle < 255)

    def test_only_the_hot_skin_has_vertex_colours(self):
        self.assertFalse(getattr(self.mesh, "colours", None))
        positions, uvs, indices = dome()
        hot, _ = make_workpiece.workpiece_mesh(positions, uvs, indices, AXE_BOX, 1.2, 0.25, 1)
        self.assertEqual(len(hot.colours), len(hot.positions))
        self.assertEqual(hot.colours[0], (0, 0, 0, 255))
        self.assertEqual(hot.materials, ["metal", "hot_01"])


if __name__ == "__main__":
    unittest.main()
