"""The reference's Lua and C# signatures (language_signatures.py)."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from language_signatures import add_signatures  # noqa: E402


def method(name, params=(), returns="", static=None, **extra):
    return {
        "kind": "method",
        "name": name,
        "params": [{"name": p, "type": "", "optional": False, "default": None} for p in params],
        "returns": returns,
        "static": static,
        **extra,
    }


def api(*types):
    return {"schema": 1, "apiVersion": "0.4.1", "types": list(types)}


PLAYER = {
    "name": "Player",
    "domain": "players",
    "members": [
        method("getById", ["id"], "Player | null", static=True),
        method("kick", ["reason"], "boolean", static=False),
        {
            "kind": "property",
            "name": "nickname",
            "type": "string",
            "readonly": True,
            "static": False,
        },
    ],
}
EVENTS = {
    "name": "Events",
    "domain": "core",
    "members": [
        {"kind": "event", "name": "playerReady", "params": [{"name": "player", "type": ""}]},
    ],
}


class SignaturesTest(unittest.TestCase):
    def signatures(self, *types):
        reference = add_signatures(api(*types))
        return {
            (t["name"], m["name"]): m["signatures"]
            for t in reference["types"]
            for m in t["members"]
        }

    def test_statics_instances_and_properties(self):
        found = self.signatures(PLAYER)
        self.assertEqual(found[("Player", "getById")]["lua"], "Player.getById(id): Player?")
        self.assertEqual(
            found[("Player", "getById")]["cs"], "static Player? Player.GetById(object? id)"
        )
        self.assertEqual(found[("Player", "kick")]["lua"], "player:kick(reason): boolean")
        self.assertEqual(found[("Player", "kick")]["cs"], "bool Kick(object? reason)")
        self.assertEqual(found[("Player", "nickname")]["lua"], "player.nickname: string")
        self.assertEqual(found[("Player", "nickname")]["cs"], "string Nickname { get; }")

    def test_events_take_typed_handlers(self):
        found = self.signatures(EVENTS, PLAYER)
        self.assertEqual(
            found[("Events", "playerReady")]["lua"],
            'Events.on("playerReady", function(player) end)',
        )
        self.assertEqual(
            found[("Events", "playerReady")]["cs"],
            'Events.On("playerReady", (Player player) => { })',
        )

    def test_a_language_without_the_member_gets_nothing(self):
        process = {
            "name": "process",
            "domain": None,
            "members": [{"kind": "property", "name": "env", "type": "", "static": True}],
        }
        found = self.signatures(process)
        self.assertEqual(found[("process", "env")], {"lua": "", "cs": ""})

    def test_timers_are_the_hand_written_csharp_class(self):
        timers = {
            "name": "Timers",
            "domain": None,
            "members": [method("setTimeout", ["handler", "delay"], "Timeout")],
        }
        found = self.signatures(timers)
        self.assertEqual(found[("Timers", "setTimeout")]["lua"], "setTimeout(handler, delay): any")
        self.assertTrue(
            found[("Timers", "setTimeout")]["cs"].startswith("static JsValue Timers.SetTimeout(")
        )


if __name__ == "__main__":
    unittest.main()
