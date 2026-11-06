"""Lua annotations of the KCDS SDK, generated from the extracted API.

    python Dev/tools/web/build_lua_annotations.py <api.json> <out dir> [--version X]

<api.json> is what Dev/tools/web/extract_sdk_api.cjs writes. Writes two definition files for the
Lua language server (LuaLS, the `---@` annotations), one per side, as a Lua resource's scripts
see the SDK (KCDS-DOCS/lua.md):

    kcds-server.lua   what server scripts have
    kcds-client.lua   what client scripts have

A resource's editor workspace lists one of them as a library (`workspace.library`). They only
describe; nothing runs them. Types come from the same reading of the API as the TypeScript
declarations (build_typescript_declarations.py), written the Lua way: `kcds.<domain>.<Type>`
classes, `Type.new(...)` for a class's constructor, `Type.name(...)` for its statics and
`value:name(...)` for its instance methods, nil for null, `fun(...)` for functions, and promises
with :next/:catch/:finally/:await. Where the API gives no type, `any` stands in.
"""

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import build_typescript_declarations as ts  # noqa: E402

# Globals a JavaScript script has that a Lua script has not (KCDS-DOCS/lua.md): Node's own, and
# what Lua writes its own way.
NOT_FOR_LUA = {"process", "console"}

LUA_BUILTIN = {
    "any": "any",
    "unknown": "any",
    "never": "any",
    "void": "nil",
    "null": "nil",
    "undefined": "nil",
    "string": "string",
    "number": "number",
    "boolean": "boolean",
    "true": "true",
    "false": "false",
    "bigint": "integer",
    "object": "table",
    "Function": "function",
    "Uint8Array": "any",
    "ArrayBuffer": "any",
    "Date": "any",
    "Error": "any",
}

PROMISE_CLASS = [
    "---A promise: what an asynchronous member answers.",
    "---@class kcds.Promise<T>",
    "local Promise = {}",
    "",
    "---Runs `handler` with the value once the promise is fulfilled; answers a promise of what it returns.",
    "---@param handler fun(value: T): any",
    "---@return kcds.Promise<any>",
    "function Promise:next(handler) end",
    "",
    "---Runs `handler` with the error if the promise is rejected.",
    "---@param handler fun(problem: any): any",
    "---@return kcds.Promise<any>",
    "function Promise:catch(handler) end",
    "",
    "---Runs `handler` once the promise settles, either way.",
    "---@param handler fun()",
    "---@return kcds.Promise<T>",
    "function Promise:finally(handler) end",
    "",
    "---Waits for the promise and answers its value (at the top level of a file only).",
    "---@return T",
    "function Promise:await() end",
    "",
]


def split_top(text, separator):
    """Splits at a separator outside brackets."""
    parts, depth, current = [], 0, ""
    for character in text:
        if character in "<([{":
            depth += 1
        elif character in ">)]}":
            depth -= 1
        if character == separator and depth == 0:
            parts.append(current)
            current = ""
        else:
            current += character
    parts.append(current)
    return [part.strip() for part in parts]


class Types:
    """How the API's type names read in Lua on one side."""

    def __init__(self, api_types):
        self.paths = {}
        for api_type in api_types:
            domain = api_type.get("domain")
            name = api_type["name"]
            self.paths[name] = f"kcds.{domain}.{name}" if domain else name

    def lua(self, text, fallback="any"):
        """A type as the API writes it, as a LuaLS type; the fallback where there is none."""
        text = (text or "").strip()
        # ts_type takes no function types; they are read here ("(player) => void").
        typescript = text if "=>" in text else ts.ts_type(text, fallback="")
        if not typescript:
            return fallback
        try:
            return self.convert(typescript)
        except ValueError:
            return fallback

    def convert(self, text):
        text = text.strip()
        if not text:
            raise ValueError("empty type")
        options = split_top(text, "|")
        if len(options) > 1:
            converted = []
            for option in options:
                lua = self.convert(option)
                if lua not in converted:
                    converted.append(lua)
            if converted == ["nil"]:
                return "nil"
            if "nil" in converted:
                converted.remove("nil")
                single = converted[0] if len(converted) == 1 else None
                if single and re.fullmatch(r"[\w.]+(\[\])?", single):
                    return single + "?"
                return "(" + "|".join(converted) + ")?"
            return "|".join(converted)
        if text.startswith("(") and text.endswith(")") and "=>" not in text:
            return self.convert(text[1:-1])
        if "=>" in text:
            return self.function(text)
        if text.endswith("[]"):
            inner = self.convert(text[:-2])
            return f"({inner})[]" if "|" in inner else f"{inner}[]"
        if text.startswith("{") and text.endswith("}"):
            return self.table(text[1:-1])
        if text[0] in "'\"" and text[-1] == text[0]:
            return '"' + text[1:-1] + '"'
        if re.fullmatch(r"-?\d+(\.\d+)?", text):
            return "number"
        generic = re.fullmatch(r"([\w$.]+)<(.*)>", text)
        if generic:
            name, inner = generic.groups()
            if name == "Promise":
                return f"kcds.Promise<{self.convert(inner)}>"
            if name in ("Array", "ReadonlyArray"):
                return self.convert(inner) + "[]"
            if name in ("Record", "Map"):
                key, value = split_top(inner, ",")
                return f"table<{self.convert(key)}, {self.convert(value)}>"
            if name == "Set":
                return f"table<{self.convert(inner)}, true>"
            return "any"
        if text in LUA_BUILTIN:
            return LUA_BUILTIN[text]
        if text in self.paths:
            return self.paths[text]
        if re.fullmatch(r"[A-Za-z_$][\w$.]*", text):
            # A type the API names but does not declare (Timeout, ...).
            return "any"
        raise ValueError(f"not a type: {text}")

    def function(self, text):
        arrow = text.index("=>")
        params, result = text[:arrow].strip(), text[arrow + 2 :].strip()
        if not (params.startswith("(") and params.endswith(")")):
            raise ValueError("not a function type")
        lua_params = []
        for param in split_top(params[1:-1], ","):
            if not param:
                continue
            name, _, kind = param.partition(":")
            name = name.strip().rstrip("?")
            if name.startswith("..."):
                lua_params.append("...: any")
                continue
            lua_params.append(f"{lua_name(name)}: {self.convert(kind) if kind else 'any'}")
        signature = f"fun({', '.join(lua_params)})"
        returned = self.convert(result)
        return signature if returned == "nil" else f"{signature}: {returned}"

    def table(self, body):
        fields = []
        for field in split_top(body.replace(";", ","), ","):
            if not field:
                continue
            name, _, kind = field.partition(":")
            optional = name.strip().endswith("?")
            name = name.strip().rstrip("?")
            if not re.fullmatch(r"[A-Za-z_]\w*", name):
                raise ValueError("not a field name")
            lua = self.convert(kind) if kind.strip() else "any"
            if optional and not lua.endswith("?"):
                lua = f"{lua}?" if re.fullmatch(r"[\w.]+(\[\])?", lua) else f"({lua})?"
            fields.append(f"{name}: {lua}")
        return "{ " + ", ".join(fields) + " }" if fields else "table"


LUA_KEYWORDS = {
    "and",
    "break",
    "do",
    "else",
    "elseif",
    "end",
    "false",
    "for",
    "function",
    "goto",
    "if",
    "in",
    "local",
    "nil",
    "not",
    "or",
    "repeat",
    "return",
    "then",
    "true",
    "until",
    "while",
}


def lua_name(name):
    clean = re.sub(r"[^\w]", "_", name) or "value"
    if clean[0].isdigit():
        clean = "_" + clean
    return clean + "_" if clean in LUA_KEYWORDS else clean


def comment(text):
    lines = []
    for line in (text or "").strip().splitlines():
        lines.append(f"---{line}" if line.strip() else "---")
    return lines


def optional_param(param):
    described = (param.get("description") or "").lower()
    return (
        param.get("name") == "options"
        or param.get("optional")
        or param.get("default") is not None
        or described.startswith("optional")
        or "(optional" in described
        or "default" in described
    )


def function_lines(owner, member, types, static):
    """A method: its comment, parameters and return, and a stub `function` line."""
    lines = comment(member.get("summary", ""))
    names = []
    for param in member.get("params", []):
        name = param.get("name") or "value"
        if name.startswith("..."):
            lines.append(f"---@param ... any {one_line(param.get('description'))}".rstrip())
            names.append("...")
            continue
        clean = lua_name(name.split("=")[0].strip())
        kind = types.lua(param.get("type"))
        mark = "?" if optional_param(param) else ""
        lines.append(
            f"---@param {clean}{mark} {kind} {one_line(param.get('description'))}".rstrip()
        )
        names.append(clean)
    returned = types.lua(member.get("returns"), fallback="any")
    if returned != "nil" and (member.get("returns") or "").strip() not in ("", "void"):
        lines.append(f"---@return {returned}")
    joiner = "." if static else ":"
    lines.append(f"function {owner}{joiner}{lua_name(member['name'])}({', '.join(names)}) end")
    return lines


def one_line(text):
    return " ".join((text or "").split())


def event_overloads(events, types):
    """Events.on's overloads: each event's name with its handler's parameters."""
    lines = []
    for event in events:
        params = []
        for param in event.get("params", []):
            name = lua_name(param.get("name") or "value")
            guess = ts.EVENT_PARAMETER_TYPES.get(param.get("name") or "")
            kind = types.lua(param.get("type") or guess or "")
            params.append(f"{name}: {kind}")
        handler = f"fun({', '.join(params)}): any"
        lines.append(f'---@overload fun(name: "{event["name"]}", handler: {handler})')
    return lines


def side_members(api_type, side):
    return [
        ts.as_side(member, side)
        for member in api_type.get("members", [])
        if member.get("side") in (side, "shared")
    ]


def type_lines(api_type, side, types, events):
    """One type as a side's Lua sees it: a class, its fields, `new` and its functions."""
    name = api_type["name"]
    members = [m for m in side_members(api_type, side) if m.get("kind") != "event"]
    if not members or name in NOT_FOR_LUA:
        return []
    path = types.paths[name]
    lines = comment(api_type.get("summary", ""))
    if name in ts.GLOBAL_FUNCTION_TYPES:
        # Global functions (setTimeout and the rest).
        for member in members:
            if member["kind"] == "method":
                lines += [""] + function_lines("", member, types, True)
        return [line.replace("function .", "function ") for line in lines] + [""]
    base = api_type.get("base") or ""
    lines.append(f"---@class {path}{f': {types.paths.get(base, base)}' if base else ''}")
    seen = set()
    methods = []
    for member in members:
        if "." in member["name"]:
            # A nested namespace's entry (Locales.defaults.items.x): its first part is a field.
            first = member["name"].split(".")[0]
            if ("property", first) not in seen:
                seen.add(("property", first))
                lines.append(f"---@field {lua_name(first)} table")
            continue
        static = ts.is_static(name, member)
        key = (member["kind"], member["name"])
        if key in seen:
            continue
        seen.add(key)
        if member["kind"] == "property":
            summary = one_line(member.get("summary"))
            lines.append(
                f"---@field {lua_name(member['name'])} {types.lua(member.get('type'))} {summary}".rstrip()
            )
        elif member["kind"] == "method":
            methods.append((member, static))
    lines.append(f"{path} = {{}}")
    instance = any(not static for _member, static in methods) or any(
        m["kind"] == "property" and not ts.is_static(name, m) for m in members
    )
    if instance:
        lines += [
            "",
            f"---Makes a new {name} (the class's constructor).",
            "---@param ... any",
            f"---@return {path}",
            f"function {path}.new(...) end",
        ]
    for member, static in methods:
        block = function_lines(path, member, types, static)
        if name == "Events" and member["name"] in ("on", "once") and events:
            block = block[:-1] + event_overloads(events, types) + block[-1:]
        lines += [""] + block
    return lines + [""]


def build(api, version):
    api_types = api["types"]
    types = Types(api_types)
    outputs = {}
    for side in ("server", "client"):
        events = []
        for api_type in api_types:
            for member in api_type.get("members", []):
                if member.get("kind") == "event" and member.get("side") in (
                    side,
                    "shared",
                ):
                    events.append(member)
        lines = [
            "---@meta",
            f"-- KCDS SDK {version}, {side} scripts in Lua: generated from the SDK's own API",
            "-- (Dev/tools/web/build_lua_annotations.py). Do not edit: regenerate it from the release.",
            "",
        ]
        lines += PROMISE_CLASS
        lines += ["---The KCDS SDK, by domain.", "kcds = {}"]
        domains = sorted({t["domain"] for t in api_types if t.get("domain")})
        lines += [f"kcds.{domain} = {{}}" for domain in domains] + [""]
        for api_type in api_types:
            lines += type_lines(api_type, side, types, events)
        outputs[f"kcds-{side}.lua"] = "\n".join(lines)
    return outputs


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("api")
    parser.add_argument("out")
    parser.add_argument("--version", default=None)
    options = parser.parse_args()
    api = json.loads(Path(options.api).read_text(encoding="utf-8"))
    version = options.version or api.get("apiVersion") or "dev"
    out = Path(options.out)
    out.mkdir(parents=True, exist_ok=True)
    for name, text in build(api, version).items():
        (out / name).write_text(text, encoding="utf-8", newline="\n")
        print(f"{name}: {text.count(chr(10))} lines")


if __name__ == "__main__":
    main()
