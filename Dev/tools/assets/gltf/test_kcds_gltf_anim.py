"""A glTF animation onto the game's character (kcds_gltf_anim.py): a small rig made
here (hips, spine, an arm and forearm), turned over one second; the studio's turns it gives move
the game's bones where the file's bones go, also when the rests differ (the file's arm out to the
side, the game's hanging down) and under a turned parent."""

import contextlib
import io
import json
import math
import os
import struct
import tempfile
import unittest

import kcds_gltf_anim as anim

IDENTITY_AXES = {"x": [1, 0, 0], "y": [0, 1, 0], "z": [0, 0, 1]}


def quaternion_about(axis, degrees):
    half = math.radians(degrees) / 2
    return tuple(part * math.sin(half) for part in axis) + (math.cos(half),)


def rig(arm_turns, hips_turns=None, hips_rise=None, prefix="mixamorig:", arm_out=(-0.3, 0.0, 0.0)):
    """A .gltf (JSON with a data URI buffer): Hips > Spine > RightArm > RightForeArm (glTF axes:
    the arm out to the character's right, -x), its animation over 1 s (0, 0.5, 1 s): the arm's
    turns, the hips' turns and rise (glTF y)."""
    times = [0.0, 0.5, 1.0]
    data = struct.pack("<3f", *times)
    accessors = [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "SCALAR"}]
    views = [{"buffer": 0, "byteOffset": 0, "byteLength": 12}]
    channels = []
    samplers = []

    def track(node, path, values, width):
        nonlocal data
        start = len(data)
        for value in values:
            data += struct.pack(f"<{width}f", *value)
        views.append({"buffer": 0, "byteOffset": start, "byteLength": len(data) - start})
        accessors.append(
            {
                "bufferView": len(views) - 1,
                "componentType": 5126,
                "count": 3,
                "type": "VEC4" if width == 4 else "VEC3",
            }
        )
        samplers.append({"input": 0, "output": len(accessors) - 1, "interpolation": "LINEAR"})
        channels.append({"sampler": len(samplers) - 1, "target": {"node": node, "path": path}})

    track(2, "rotation", arm_turns, 4)
    if hips_turns:
        track(0, "rotation", hips_turns, 4)
    if hips_rise:
        track(0, "translation", [(0.0, 1.0 + rise, 0.0) for rise in hips_rise], 3)
    import base64

    document = {
        "asset": {"version": "2.0"},
        "scenes": [{"nodes": [0]}],
        "nodes": [
            {"name": f"{prefix}Hips", "translation": [0, 1.0, 0], "children": [1]},
            {"name": f"{prefix}Spine", "translation": [0, 0.3, 0], "children": [2]},
            {"name": f"{prefix}RightArm", "translation": [-0.2, 0.3, 0], "children": [3]},
            {"name": f"{prefix}RightForeArm", "translation": list(arm_out)},
        ],
        "skins": [{"joints": [0, 1, 2, 3]}],
        "animations": [{"name": "Wave", "channels": channels, "samplers": samplers}],
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [
            {
                "uri": "data:application/octet-stream;base64," + base64.b64encode(data).decode(),
                "byteLength": len(data),
            }
        ],
    }
    return document


GAME_JOINTS = [
    {"name": "Hips", "parent": ""},
    {"name": "Spine", "parent": "Hips"},
    {"name": "RightArm", "parent": "Spine"},
    {"name": "RightForeArm", "parent": "RightArm"},
]


def game_info(arm_bone=(0.3, 0.0, 0.0)):
    """studio_joint_info as the game would answer it, every parent's axes the body's own."""
    return {
        "Hips": {"axes": IDENTITY_AXES, "bone": [0, 0, 0.3], "child": "Spine"},
        "Spine": {"axes": IDENTITY_AXES, "bone": [0.2, 0, 0.3], "child": "RightArm"},
        "RightArm": {"axes": IDENTITY_AXES, "bone": list(arm_bone), "child": "RightForeArm"},
        "RightForeArm": {"axes": IDENTITY_AXES},
    }


def studio_turn(degrees):
    """The studio's turn of degrees about x, then y, then z (animation.cjs quaternion) as rows."""
    x, y, z = (
        quaternion_about(axis, value)
        for axis, value in zip(((1, 0, 0), (0, 1, 0), (0, 0, 1)), degrees)
    )
    return anim.q_matrix(anim.q_multiply(z, anim.q_multiply(y, x)))


def apply(rows, v):
    return [sum(rows[r][c] * v[c] for c in range(3)) for r in range(3)]


class AnimationTest(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.mkdtemp()

    def tearDown(self):
        for name in os.listdir(self.folder):
            os.remove(os.path.join(self.folder, name))
        os.rmdir(self.folder)

    def run_tool(self, mode, document, request):
        path = os.path.join(self.folder, "rig.gltf")
        with open(path, "w", encoding="utf-8") as file:
            json.dump(document, file)
        request_path = os.path.join(self.folder, "request.json")
        with open(request_path, "w", encoding="utf-8") as file:
            json.dump(request, file)
        printed = io.StringIO()
        with contextlib.redirect_stdout(printed):
            code = anim.main([mode, path, request_path])
        return code, json.loads(printed.getvalue().splitlines()[-1])

    def arm_direction(self, answer, key, game_bone):
        """Where the game's arm bone points after the key's turns (spine and hips not turned)."""
        pose = answer["animation"]["keys"][key]["pose"]
        rows = studio_turn(pose.get("RightArm", [0, 0, 0]))
        return apply(rows, game_bone)

    def test_names_match_without_the_rig_prefix(self):
        code, answer = self.run_tool(
            "names", rig([quaternion_about((0, 0, 1), 0)] * 3), {"joints": GAME_JOINTS}
        )
        self.assertEqual(code, 0, answer)
        self.assertEqual(answer["mapped"]["RightArm"], "mixamorig:RightArm")
        self.assertEqual(answer["unmappedGame"], [])
        self.assertEqual(answer["animations"], [{"index": 0, "name": "Wave", "length": 1.0}])

    def test_an_arm_swung_up_goes_where_the_files_arm_goes(self):
        # The arm out to the right (glTF -x) turned -90 degrees about glTF z (the character's
        # forward): -x goes to +y, up.
        turns = [quaternion_about((0, 0, 1), degrees) for degrees in (0, -45, -90)]
        code, answer = self.run_tool(
            "convert", rig(turns), {"joints": GAME_JOINTS, "info": game_info()}
        )
        self.assertEqual(code, 0, answer)
        keys = answer["animation"]["keys"]
        self.assertEqual(len(keys), 11)
        self.assertEqual(keys[0]["time"], 0)
        self.assertEqual(keys[-1]["time"], 1.0)
        # The game's arm (out to the right, [0.3, 0, 0]) now points up as the file's does.
        end = self.arm_direction(answer, -1, [0.3, 0, 0])
        for got, want in zip(end, [0, 0, 0.3]):
            self.assertAlmostEqual(got, want, places=2)
        # The hips, the spine and the forearm (it only goes with the arm) do not turn: left out.
        self.assertEqual(set(keys[-1]["pose"]), {"RightArm"})

    def test_rests_that_differ_the_game_arm_lies_along_the_files(self):
        # The game's arm hangs down; the file's rests out to the right, not turned.
        still = [quaternion_about((0, 0, 1), 0)] * 3
        code, answer = self.run_tool(
            "convert", rig(still), {"joints": GAME_JOINTS, "info": game_info(arm_bone=(0, 0, -0.3))}
        )
        self.assertEqual(code, 0, answer)
        end = self.arm_direction(answer, 0, [0, 0, -0.3])
        for got, want in zip(end, [0.3, 0, 0]):
            self.assertAlmostEqual(got, want, places=2)

    def test_under_a_turned_hips_the_arm_turn_is_its_own(self):
        # The hips turn 90 degrees about glTF y (up) and the arm along with them: the arm has no
        # turn of its own relative to its parent.
        hips = [quaternion_about((0, 1, 0), degrees) for degrees in (0, 45, 90)]
        still = [quaternion_about((0, 0, 1), 0)] * 3
        code, answer = self.run_tool(
            "convert", rig(still, hips_turns=hips), {"joints": GAME_JOINTS, "info": game_info()}
        )
        self.assertEqual(code, 0, answer)
        pose = answer["animation"]["keys"][-1]["pose"]
        # The hips turn about the body's up (z) by 90 degrees; nothing else turns.
        self.assertEqual(set(pose), {"Hips"})
        self.assertAlmostEqual(abs(pose["Hips"][2]), 90, places=0)

    def test_the_hips_rise_is_the_keys_lift(self):
        still = [quaternion_about((0, 0, 1), 0)] * 3
        code, answer = self.run_tool(
            "convert",
            rig(still, hips_rise=[0.0, -0.2, 0.0]),
            {"joints": GAME_JOINTS, "info": game_info()},
        )
        self.assertEqual(code, 0, answer)
        lifts = [key.get("lift", 0) for key in answer["animation"]["keys"]]
        self.assertAlmostEqual(min(lifts), -0.2, places=2)

    def test_limits_and_refusals(self):
        turns = [quaternion_about((0, 0, 1), degrees) for degrees in (0, 45, 90)]
        code, answer = self.run_tool(
            "convert", rig(turns), {"joints": GAME_JOINTS, "info": game_info(), "rate": 1000}
        )
        self.assertEqual(len(answer["animation"]["keys"]), 32)
        code, answer = self.run_tool("convert", rig(turns), {"joints": GAME_JOINTS})
        self.assertEqual(code, 1)
        self.assertIn("no joint info", answer["error"])
        code, answer = self.run_tool(
            "convert", rig(turns, prefix="Other_"), {"joints": GAME_JOINTS, "info": game_info()}
        )
        self.assertIn("no joint of the file matches", answer["error"])
        code, answer = self.run_tool(
            "convert",
            rig(turns, prefix="Other_"),
            {
                "joints": GAME_JOINTS,
                "info": game_info(),
                "map": {"RightArm": "Other_RightArm", "Spine": "Other_Spine"},
            },
        )
        self.assertEqual(code, 0, answer)
        code, answer = self.run_tool(
            "convert", rig(turns), {"joints": GAME_JOINTS, "info": game_info(), "animation": 3}
        )
        self.assertIn("animation is 0 to 0", answer["error"])


if __name__ == "__main__":
    unittest.main()
