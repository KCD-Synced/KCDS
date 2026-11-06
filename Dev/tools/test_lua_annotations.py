"""Tests of Dev/tools/web/build_lua_annotations.py."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "web"))

import build_lua_annotations as annotations  # noqa: E402


def member(name, signature, side="server", kind="method", **fields):
    return {"name": name, "signature": signature, "side": side, "kind": kind, **fields}


API = {
    "apiVersion": "9.9.9",
    "types": [
        {
            "name": "Events",
            "domain": "core",
            "summary": "Events.",
            "members": [
                member(
                    "on",
                    "Events.on(name, fn)",
                    side="shared",
                    static=True,
                    params=[{"name": "name"}, {"name": "fn"}],
                    returns="void",
                ),
                member(
                    "playerConnect",
                    'on(event: "playerConnect", cb: (player) => void): void',
                    kind="event",
                    params=[{"name": "player"}],
                ),
                member(
                    "hudShown",
                    'on(event: "hudShown", cb: () => void): void',
                    side="client",
                    kind="event",
                ),
            ],
        },
        {
            "name": "Vector3",
            "domain": "math",
            "summary": "A point.",
            "members": [
                member(
                    "zero",
                    "Vector3.zero()",
                    side="shared",
                    static=True,
                    returns="Vector3",
                ),
                member("x", "x", side="shared", kind="property", type="number"),
                member(
                    "distance",
                    "distance(other)",
                    side="shared",
                    static=False,
                    returns="number",
                    params=[{"name": "other", "type": "Vector3"}],
                ),
            ],
        },
        {
            "name": "Player",
            "domain": "players",
            "summary": "A player.",
            "members": [
                member(
                    "getById",
                    "Player.getById(id)",
                    static=True,
                    returns="Player | null",
                    params=[{"name": "id", "type": "number"}],
                ),
                member(
                    "load",
                    "Player.load(end)",
                    static=True,
                    returns="Promise<string[]>",
                    params=[{"name": "end", "description": "Optional end."}],
                ),
                member("emit", "emit(name, payload)", static=False, returns="void"),
                member("stats", "stats", kind="property", type="{ hp, stamina }"),
            ],
        },
        {
            "name": "Timers",
            "summary": "Timers.",
            "members": [
                member(
                    "setTimeout",
                    "setTimeout(fn, ms)",
                    side="shared",
                    returns="Timeout",
                    params=[
                        {"name": "fn", "type": "() => void"},
                        {"name": "ms", "type": "number"},
                    ],
                ),
            ],
        },
        {
            "name": "process",
            "summary": "Node's process.",
            "members": [member("cwd", "process.cwd()", static=True, returns="string")],
        },
    ],
}


class LuaAnnotationsTest(unittest.TestCase):
    def setUp(self):
        self.files = annotations.build(API, "9.9.9")
        self.server = self.files["kcds-server.lua"]
        self.client = self.files["kcds-client.lua"]

    def test_one_file_per_side(self):
        self.assertEqual(set(self.files), {"kcds-server.lua", "kcds-client.lua"})
        self.assertTrue(self.server.startswith("---@meta\n"))
        self.assertIn("KCDS SDK 9.9.9, server scripts", self.server)

    def test_classes_by_domain_with_new(self):
        self.assertIn("---@class kcds.math.Vector3", self.server)
        self.assertIn("function kcds.math.Vector3.new(...) end", self.server)
        # A namespace (only statics) has no constructor.
        self.assertNotIn("function kcds.core.Events.new", self.server)

    def test_statics_with_a_dot_instance_methods_with_a_colon(self):
        self.assertIn("function kcds.math.Vector3.zero() end", self.server)
        self.assertIn("function kcds.math.Vector3:distance(other) end", self.server)
        self.assertIn("---@param other kcds.math.Vector3", self.server)

    def test_nil_promises_tables_and_keywords(self):
        self.assertIn("---@return kcds.players.Player?", self.server)
        self.assertIn("---@return kcds.Promise<string[]>", self.server)
        self.assertIn("---@class kcds.Promise<T>", self.server)
        self.assertIn("---@field stats { hp: any, stamina: any }", self.server)
        self.assertIn("---@param end_? any Optional end.", self.server)
        self.assertIn("function kcds.players.Player.load(end_) end", self.server)

    def test_sides(self):
        self.assertNotIn("kcds.players.Player.getById", self.client)
        self.assertIn('---@overload fun(name: "playerConnect"', self.server)
        self.assertNotIn('"hudShown"', self.server)
        self.assertIn('---@overload fun(name: "hudShown", handler: fun(): any)', self.client)
        self.assertIn("handler: fun(player: kcds.players.Player): any", self.server)

    def test_globals(self):
        self.assertIn("function setTimeout(fn, ms) end", self.server)
        self.assertIn("---@param fn fun()", self.server)
        self.assertNotIn("process", self.server)

    def test_type_conversion(self):
        types = annotations.Types(API["types"])
        self.assertEqual(types.lua("Vector3 | null"), "kcds.math.Vector3?")
        self.assertEqual(types.lua("string | number"), "string|number")
        self.assertEqual(types.lua("'a' | 'b'"), '"a"|"b"')
        self.assertEqual(
            types.lua("(player, seconds) => boolean"),
            "fun(player: any, seconds: any): boolean",
        )
        self.assertEqual(types.lua("Record<string, Player>"), "table<string, kcds.players.Player>")
        self.assertEqual(types.lua("Timeout"), "any")
        self.assertEqual(types.lua("a table of things"), "any")
        self.assertEqual(types.lua(""), "any")


if __name__ == "__main__":
    unittest.main()
