"""Tests of Dev/tools/web/build_typescript_declarations.py."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "web"))

import build_typescript_declarations as declarations  # noqa: E402


def member(name, signature, side="server", kind="method", **fields):
    return {"name": name, "signature": signature, "side": side, "kind": kind, **fields}


API = {
    "apiVersion": "9.9.9",
    "types": [
        {
            "name": "Vector3",
            "summary": "A point.",
            "members": [
                member("zero", "Vector3.zero()", side="shared", returns="Vector3"),
                member("x", "x", side="shared", kind="property", type="number"),
            ],
        },
        {
            "name": "Gate",
            "summary": "A gate.",
            "members": [
                member(
                    "all",
                    "Gate.all(level)",
                    returns="Gate[]",
                    params=[{"name": "level"}],
                ),
                member("openness", "openness", kind="property", type="number"),
                member(
                    "state",
                    "state",
                    kind="property",
                    type="number",
                    stability="deprecated",
                ),
                member(
                    "toggle",
                    "toggle(options = {})",
                    returns="boolean",
                    params=[{"name": "options", "default": "{}"}],
                ),
                member("states", "states", kind="property", type="Timeout"),
                member(
                    "pieces",
                    "pieces()",
                    returns="({ name, kind: 'a', done? } | null)[]",
                ),
            ],
        },
        {
            "name": "World",
            "summary": "The world.",
            "members": [
                member(
                    "raycast",
                    "World.raycast(player, from, to)",
                    side="shared",
                    returns="Promise<{ hit }>",
                    params=[{"name": "player"}, {"name": "from"}, {"name": "to"}],
                    variants={
                        "client": {
                            "signature": "World.raycast(from, to)",
                            "params": [{"name": "from"}, {"name": "to"}],
                            "returns": "WorldRayHit | null",
                            "summary": "The first hit on this machine.",
                        }
                    },
                )
            ],
        },
        {
            "name": "Web",
            "summary": "Client: pages.",
            "members": [
                member(
                    "on",
                    "Web.on(viewId, eventName, handler)",
                    side="client",
                    returns="boolean",
                    params=[
                        {"name": "viewId"},
                        {"name": "eventName"},
                        {"name": "handler"},
                    ],
                )
            ],
        },
        {
            "name": "Events",
            "summary": "Events.",
            "members": [
                member(
                    "on",
                    "Events.on(name, fn)",
                    side="shared",
                    params=[{"name": "name"}, {"name": "fn"}],
                ),
                member(
                    "playerPickpocketed",
                    'on(event: "playerPickpocketed", cb: (thief, victim, items) => void): void',
                    kind="event",
                    params=[{"name": "thief"}, {"name": "victim"}, {"name": "items"}],
                ),
                member(
                    "uiReady",
                    'on(event: "uiReady", cb: () => void): void',
                    side="client",
                    kind="event",
                ),
            ],
        },
        {
            "name": "Timers",
            "summary": "Timers.",
            "members": [
                member(
                    "setTimeout",
                    "setTimeout(handler, delay)",
                    side="shared",
                    returns="Timeout",
                    params=[{"name": "handler"}, {"name": "delay"}],
                )
            ],
        },
    ],
}


NAMESPACED_API = {
    "types": [
        {
            "name": "Door",
            "domain": "world",
            "side": "server",
            "summary": "A door.",
            "members": [
                {
                    "name": "open",
                    "kind": "method",
                    "side": "server",
                    "summary": "Opens it.",
                    "params": [],
                    "returns": "boolean",
                }
            ],
        }
    ]
}


class NamespaceTest(unittest.TestCase):
    def test_a_domain_type_is_reached_only_through_kcds(self):
        server = declarations.build(NAMESPACED_API, "9.9.9")["server-api.d.ts"]
        self.assertNotIn("declare class Door", server)
        self.assertIn("declare namespace KcdsTypes {", server)
        self.assertIn("  class Door {", server)
        self.assertIn("readonly Door: typeof KcdsTypes.Door;", server)
        self.assertIn("type Door = KcdsTypes.Door;", server)


class DeclarationsTest(unittest.TestCase):
    def setUp(self):
        self.files = declarations.build(API, "9.9.9")

    def test_shared_types_go_to_shared(self):
        shared = self.files["shared.d.ts"]
        self.assertIn("declare class Vector3 {", shared)
        self.assertIn("  static zero(): Vector3;", shared)
        self.assertIn("  readonly x: number;", shared)
        self.assertIn("declare function setTimeout(handler: any, delay: any): Timeout;", shared)
        self.assertNotIn("declare class Gate", shared)

    def test_static_instance_and_optional_members(self):
        server = self.files["server-api.d.ts"]
        self.assertIn('/// <reference path="./shared.d.ts" />', server)
        self.assertIn("  static all(level: any): Gate[];", server)
        self.assertIn("  readonly openness: number;", server)
        self.assertIn("  toggle(options?: any): boolean;", server)
        self.assertIn("  pieces(): ({ name: any; kind: 'a'; done?: any } | null)[];", server)
        self.assertNotIn("declare class Gate", self.files["client-api.d.ts"])

    def test_a_side_variant_is_declared_on_its_side(self):
        server = self.files["server-api.d.ts"]
        client = self.files["client-api.d.ts"]
        self.assertIn(
            "  static raycast(player: any, from: any, to: any): Promise<{ hit: any }>;",
            server,
        )
        self.assertIn("  static raycast(from: any, to: any): WorldRayHit | null;", client)
        self.assertIn("The first hit on this machine.", client)
        self.assertNotIn("declare class World", self.files["shared.d.ts"])
        # A ray hit is a declared data type, never an any stub.
        self.assertIn("interface WorldRayHit {", self.files["shared.d.ts"])
        self.assertNotIn("type WorldRayHit = any;", client)

    def test_names_without_a_declaration_are_any(self):
        self.assertIn("type Timeout = any;", self.files["shared.d.ts"])
        self.assertNotIn("type Gate = any;", self.files["server-api.d.ts"])

    def test_handler_types_are_declared_and_used(self):
        shared = self.files["shared.d.ts"]
        self.assertIn("type KeyHandler = (key: string, state: 'down' | 'up') => void;", shared)
        self.assertIn("type WebEventHandler = (payload: any) => void;", shared)
        self.assertIn(
            "  static on(viewId: any, eventName: any, handler: WebEventHandler): boolean;",
            self.files["client-api.d.ts"],
        )
        self.assertNotIn("type WebEventHandler = any;", self.files["client-api.d.ts"])

    def test_data_types_are_declared(self):
        shared = self.files["shared.d.ts"]
        for name in (
            "Appearance",
            "InventoryRow",
            "ProgressionSnapshot",
            "CookPotSnapshot",
            "JoinRequest",
        ):
            self.assertIn(f"interface {name} {{", shared)
        self.assertNotIn("type InventoryRow = any;", self.files["server-api.d.ts"])

    def test_named_types_of_issue_547_are_declared(self):
        shared = self.files["shared.d.ts"]
        for name in (
            "InventorySnapshot",
            "InventoryUnit",
            "InventoryResult",
            "InventoryChange",
            "LevelEditDefinition",
            "DialoguePage",
            "PlacementPose",
            "ScreenEffectInfo",
        ):
            self.assertIn(f"interface {name} {{", shared)
        self.assertIn("  food: CookPotFood | null;", shared)
        self.assertIn("type CookPotFood = 'goulash' | 'lentil' | 'soup';", shared)
        self.assertIn("type MessageHandler = (payload: any, reply: (value?: any) => void)", shared)
        # A data type declared with `type` is no stub on either side.
        self.assertNotIn("type CookPotFood = any;", self.files["server-api.d.ts"])

    def test_a_deprecated_member_is_not_declared(self):
        server = self.files["server-api.d.ts"]
        self.assertIn("  readonly openness: number;", server)
        self.assertNotIn("state: number;", server)

    def test_events_type_a_resources_own_event_and_the_browser_events(self):
        server = self.files["server-api.d.ts"]
        client = self.files["client-api.d.ts"]
        self.assertIn("  static on(name: string, fn: EventHandler): any;", server)
        # thief and victim are Player where the file declares it (this API has no Player).
        self.assertIn("victim: any, items: PickpocketedItem[]) => any;", server)
        self.assertIn("  webViewFailed: (event: BrowserLoadingFailedEvent) => any;", client)
        self.assertNotIn("webViewFailed", server)

    def test_unreadable_types_fall_back_to_any(self):
        self.assertEqual(declarations.ts_type("array"), "any")
        self.assertEqual(declarations.ts_type("table of things"), "any")
        self.assertEqual(declarations.ts_type("string | null"), "string | null")
        self.assertEqual(declarations.ts_type(""), "any")


if __name__ == "__main__":
    unittest.main()
