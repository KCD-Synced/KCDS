"""The SDK's members as Lua and C# scripts write them, for the website's API reference.

    python Dev/tools/web/language_signatures.py <api.json>     prints every member's three forms

add_signatures(api): each member of the reference gets
"signatures": { "lua": ..., "cs": ... }, read with the rules the Lua annotations
(build_lua_annotations.py) and the C# SDK (build_csharp_sdk.py) are generated with, so the page
shows the names and types a Lua or C# script really has. A member a language does not have
(Node's process in Lua, the web platform's classes in C#) gets "" there, and the site says so.
"""

import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import build_csharp_sdk as cs  # noqa: E402
import build_lua_annotations as lua  # noqa: E402
import build_typescript_declarations as ts  # noqa: E402

# The few globals C# has as hand-written classes (Kcds.Timers, Kcds.Network), by JavaScript name.
CSHARP_GLOBALS = {
    (
        "Timers",
        "setTimeout",
    ): "static JsValue Timers.SetTimeout(Delegate callback, double milliseconds)",
    (
        "Timers",
        "setInterval",
    ): "static JsValue Timers.SetInterval(Delegate callback, double milliseconds)",
    ("Timers", "clearTimeout"): "static void Timers.ClearTimeout(JsValue handle)",
    ("Timers", "clearInterval"): "static void Timers.ClearInterval(JsValue handle)",
}
CSHARP_FETCH = "static Task<JsValue> Network.Fetch(string url, object? options = null)"


def short_csharp(text):
    """Kcds.Players.Player as Player: the guide's examples import the SDK's namespaces."""
    return re.sub(r"\bKcds\.\w+\.", "", text)


def short_lua(text):
    """kcds.players.Player as Player (as a script names it after `local Player = ...`), and
    kcds.Promise as Promise."""
    return re.sub(r"\bkcds\.(\w+\.)?", "", text)


def receiver(type_name):
    """The instance a page names in its headings: Player's player."""
    return type_name[:1].lower() + type_name[1:]


class CSharp:
    """C# signatures, as build_csharp_sdk.py declares the members."""

    def __init__(self, api):
        self.api_types = [t for t in api["types"] if t.get("domain") in cs.NAMESPACES]
        self.classes = set()
        for api_type in self.api_types:
            if any(not static for _member, static in cs.members_of(api_type)):
                self.classes.add(api_type["name"])
        for api_type in self.api_types:
            if api_type.get("base"):
                self.classes.add(api_type["base"])
        self.types = cs.Types(self.api_types, self.classes)

    def type_signatures(self, api_type):
        """{ (member name, static): signature } of one type, in the SDK's own order."""
        name = api_type["name"]
        found = {}
        if api_type.get("domain") not in cs.NAMESPACES or name in cs.VALUE_TYPES:
            return found
        taken = set(cs.WRAPPER_MEMBERS) if name in self.classes else set()
        for member, static in cs.members_of(api_type):
            if name not in self.classes and not static:
                continue
            member = cs.as_method(member) or member
            own = cs.member_name(member["name"], name, taken)
            # A static is shown with its class, as a script calls it: Player.GetById(...).
            lead = "static " if static else ""
            owner = f"{name}." if static else ""
            if member["kind"] == "method":
                declared, _arguments, _docs = cs.parameters(member, self.types)
                result = cs.return_type(member, self.types)
                if member["name"] == "toString" and not static and not declared:
                    text = "string ToString()"
                else:
                    text = f"{lead}{result} {owner}{own}({', '.join(declared)})"
            else:
                kind = self.types.csharp(member.get("type"))
                if kind == "void":
                    kind = "JsValue"
                access = "{ get; }" if member.get("readonly") else "{ get; set; }"
                text = f"{lead}{kind} {owner}{own} {access}"
            found[(member["name"], static)] = short_csharp(text)
        return found

    def event(self, member):
        params = []
        for param in member.get("params", []):
            param_name = param.get("name") or "value"
            guess = ts.EVENT_PARAMETER_TYPES.get(param_name)
            kind = self.types.csharp(param.get("type") or guess or "", fallback="JsValue")
            if kind == "void":
                kind = "JsValue"
            params.append(f"{short_csharp(kind)} {cs.identifier(param_name)}")
        return f'Events.On("{member["name"]}", ({", ".join(params)}) => {{ }})'


class Lua:
    """Lua signatures, as build_lua_annotations.py describes the members."""

    def __init__(self, api):
        self.types = lua.Types(api["types"])

    def type_name(self, text, fallback="any"):
        return short_lua(self.types.lua(text, fallback=fallback))

    def method(self, type_name, member, static):
        params = []
        for param in member.get("params", []):
            param_name = param.get("name") or "value"
            if param_name.startswith("..."):
                params.append("...")
                continue
            clean = lua.lua_name(param_name.split("=")[0].strip())
            mark = "?" if lua.optional_param(param) else ""
            if param.get("type"):
                params.append(f"{clean}{mark}: {self.type_name(param.get('type'))}")
            else:
                # The API gives no type: the name alone, as the member's parameter table describes it.
                params.append(f"{clean}{mark}")
        if type_name in ts.GLOBAL_FUNCTION_TYPES:
            owner = ""
        elif static:
            owner = f"{type_name}."
        else:
            owner = f"{receiver(type_name)}:"
        text = f"{owner}{lua.lua_name(member['name'])}({', '.join(params)})"
        returns = (member.get("returns") or "").strip()
        returned = self.type_name(returns)
        if returns not in ("", "void") and returned != "nil":
            text += f": {returned}"
        return text

    def property(self, type_name, member, static):
        owner = type_name if static else receiver(type_name)
        return f"{owner}.{member['name']}: {self.type_name(member.get('type'))}"

    def event(self, member):
        names = [lua.lua_name(param.get("name") or "value") for param in member.get("params", [])]
        return f'Events.on("{member["name"]}", function({", ".join(names)}) end)'


def add_signatures(api):
    """Gives every member of the reference its Lua and C# signature (or "" where it has none)."""
    csharp = CSharp(api)
    luas = Lua(api)
    for api_type in api["types"]:
        type_name = api_type["name"]
        declared = csharp.type_signatures(api_type)
        for member in api_type.get("members", []):
            static = ts.is_static(type_name, member)
            kind = member.get("kind")
            if kind == "event":
                member["signatures"] = {
                    "lua": luas.event(member),
                    "cs": csharp.event(member),
                }
                continue
            if type_name in lua.NOT_FOR_LUA:
                lua_text = ""
            elif "." in member["name"]:
                lua_text = f"{type_name}.{member['name']}"
            elif kind == "method":
                lua_text = luas.method(type_name, member, static)
            else:
                lua_text = luas.property(type_name, member, static)
            cs_text = declared.get((member["name"], static), "")
            if not cs_text:
                cs_text = CSHARP_GLOBALS.get((type_name, member["name"]), "")
            if not cs_text and type_name == "fetch":
                cs_text = CSHARP_FETCH
            member["signatures"] = {"lua": lua_text, "cs": cs_text}
    return api


def main(argv):
    if len(argv) != 1:
        print("usage: python language_signatures.py <api.json>")
        return 2
    api = add_signatures(json.loads(Path(argv[0]).read_text(encoding="utf-8")))
    for api_type in api["types"]:
        for member in api_type.get("members", []):
            print(f"{api_type['name']}.{member['name']} [{member['kind']}]")
            print(f"  lua {member['signatures']['lua']}")
            print(f"  cs  {member['signatures']['cs']}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
