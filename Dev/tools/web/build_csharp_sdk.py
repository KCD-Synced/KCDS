"""The typed C# SDK, generated from the extracted API.

    python Dev/tools/web/build_csharp_sdk.py <api.json> [<out dir>] [--check]

<api.json> is what Dev/tools/web/extract_sdk_api.cjs writes. Writes one C# file per SDK domain to
<out dir> (default Backend/scripting/runtimes/csharp/Kcds.Sdk/Generated): each SDK type in
`Kcds.<Domain>` (Kcds.Core.Events, Kcds.Players.Player, Kcds.Math.Vector3), every member with
its summary as XML docs. With --check, writes nothing and fails when the files there differ from
what the API gives (the parity check: a member the SDK has and C# lacks fails the build).

A type with instance members is a class wrapping the JavaScript object (Kcds.JsObject), made with
`new Vector3(1, 2, 3)` or from what the SDK answers; a type with only statics is a static class.
Member names are the JavaScript names in PascalCase (`player.nickname` is `player.Nickname`).
Where the API gives a type, it is the C# type (number double, boolean bool, an SDK type its class,
X[] an array, Promise<T> a Task); elsewhere JsValue (or object for a parameter). Optional
parameters default to null, which the call leaves out, so the member's own default applies.
"""

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import build_typescript_declarations as ts  # noqa: E402

ROOT = Path(__file__).resolve().parents[3]
DEFAULT_OUT = ROOT / "Backend" / "scripting" / "runtimes" / "csharp" / "Kcds.Sdk" / "Generated"

# Domains as C# namespaces.
NAMESPACES = {
    "core": "Core",
    "players": "Players",
    "characters": "Characters",
    "world": "World",
    "ui": "Ui",
    "math": "Math",
}

# The value globals of a domain: a live value of another type, read each time.
VALUE_TYPES = {"LocalPlayer": "Player"}

# Names a generated member cannot take: members every C# object has.
RESERVED = {
    "Equals",
    "GetHashCode",
    "GetType",
    "MemberwiseClone",
    "Finalize",
}

# The wrapper's own members, which a class of the SDK (a JsObject) cannot take either.
WRAPPER_MEMBERS = {"Handle", "AsValue"}

CSHARP_KEYWORDS = {
    "abstract", "as", "base", "bool", "break", "byte", "case", "catch", "char", "checked", "class",
    "const", "continue", "decimal", "default", "delegate", "do", "double", "else", "enum", "event",
    "explicit", "extern", "false", "finally", "fixed", "float", "for", "foreach", "goto", "if",
    "implicit", "in", "int", "interface", "internal", "is", "lock", "long", "namespace", "new",
    "null", "object", "operator", "out", "override", "params", "private", "protected", "public",
    "readonly", "ref", "return", "sbyte", "sealed", "short", "sizeof", "stackalloc", "static",
    "string", "struct", "switch", "this", "throw", "true", "try", "typeof", "uint", "ulong",
    "unchecked", "unsafe", "ushort", "using", "virtual", "void", "volatile", "while",
}  # fmt: skip


def pascal(name):
    parts = re.split(r"[^A-Za-z0-9]+", name)
    if "_" in name and name == name.upper():
        # A constant's name, SHARED_DIMENSION: its words as SharedDimension.
        parts = [part.lower() for part in parts]
    text = "".join(part[:1].upper() + part[1:] for part in parts if part)
    if not text or text[0].isdigit():
        text = "_" + text
    return text


def identifier(name):
    clean = re.sub(r"[^A-Za-z0-9_]", "_", name) or "value"
    if clean[0].isdigit():
        clean = "_" + clean
    return "@" + clean if clean in CSHARP_KEYWORDS else clean


def xml(text):
    return (text or "").replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def doc_lines(summary, indent, params=(), returns=None, side=None):
    lines = [f"{indent}/// <summary>"]
    text = " ".join((summary or "").split())
    if side in ("server", "client"):
        text = f"{text} ({side.capitalize()} only.)".strip()
    for chunk in wrap(xml(text) or "No description.", 96):
        lines.append(f"{indent}/// {chunk}")
    lines.append(f"{indent}/// </summary>")
    for name, description in params:
        lines.append(
            f'{indent}/// <param name="{name.lstrip("@")}">{xml(" ".join((description or "").split()))}</param>'
        )
    if returns:
        lines.append(f"{indent}/// <returns>{xml(returns)}</returns>")
    return lines


def wrap(text, width):
    words, lines, line = text.split(), [], ""
    for word in words:
        if line and len(line) + 1 + len(word) > width:
            lines.append(line)
            line = word
        else:
            line = f"{line} {word}".strip()
    if line:
        lines.append(line)
    return lines


class Types:
    """How the API's type names read in C#."""

    def __init__(self, api_types, classes):
        self.paths = {}
        for api_type in api_types:
            domain = api_type.get("domain")
            if domain in NAMESPACES and api_type["name"] in classes:
                self.paths[api_type["name"]] = f"Kcds.{NAMESPACES[domain]}.{api_type['name']}"

    def csharp(self, text, fallback="JsValue"):
        typescript = ts.ts_type(type_text(text), fallback="")
        if not typescript:
            return fallback
        try:
            return self.convert(typescript.strip())
        except ValueError:
            return fallback

    def convert(self, text):
        options = [option.strip() for option in split_top(text, "|")]
        nullable = any(option in ("null", "undefined") for option in options)
        options = [option for option in options if option not in ("null", "undefined")]
        if not options:
            return "JsValue"
        if len(options) > 1:
            return "JsValue"
        single = self.single(options[0])
        if nullable and single not in ("JsValue", "object?") and not single.endswith("?"):
            return single + "?"
        return single

    def single(self, text):
        if text.endswith("[]"):
            inner = self.single(text[:-2].strip("() "))
            return f"{inner.rstrip('?')}[]" if inner != "JsValue" else "JsValue"
        generic = re.fullmatch(r"([\w$.]+)<(.*)>", text)
        if generic:
            name, inner = generic.groups()
            if name == "Promise":
                result = self.convert(inner)
                return "Task" if result == "void" else f"Task<{result}>"
            if name in ("Array", "ReadonlyArray"):
                return self.single(inner + "[]")
            return "JsValue"
        mapping = {
            "number": "double",
            "boolean": "bool",
            "string": "string",
            "void": "void",
            "any": "JsValue",
            "unknown": "JsValue",
            "true": "bool",
            "false": "bool",
        }
        if text in mapping:
            return mapping[text]
        if text in self.paths:
            return self.paths[text]
        if text[:1] in "'\"":
            return "string"
        return "JsValue"


def type_text(text):
    """The type of a "Type: what it is" text (a return the docs describe after the type)."""
    text = text or ""
    depth = 0
    for index, character in enumerate(text):
        if character in "<([{":
            depth += 1
        elif character in ">)]}":
            depth -= 1
        elif character == ":" and depth == 0 and text[index + 1 : index + 2] == " ":
            return text[:index].strip()
    return text


# A property whose type is a function, `say: (text, options?) => boolean`: a method in C#.
FUNCTION_TYPE = re.compile(r"^\((.*)\)\s*=>\s*(.+)$")


def as_method(member):
    """A property of function type as the method it is, or None for any other property."""
    match = FUNCTION_TYPE.match((member.get("type") or "").strip())
    if member.get("kind") != "property" or not match:
        return None
    params = []
    for raw in split_top(match.group(1), ","):
        name = raw.strip()
        if not name:
            continue
        params.append(
            {
                "name": name.rstrip("?"),
                "type": "",
                "optional": name.endswith("?"),
                "default": None,
                "description": "",
            }
        )
    return {**member, "kind": "method", "params": params, "returns": match.group(2).strip()}


def split_top(text, separator):
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
    return parts


def optional_param(param):
    described = (param.get("description") or "").lower()
    return (
        param.get("name") == "options"
        or param.get("optional")
        or param.get("default") is not None
        or "=" in (param.get("name") or "")
        or described.startswith("optional")
        or "(optional" in described
        or "default" in described
    )


FUNCTION_NAMES = {"fn", "handler", "callback", "cb", "listener", "onTick", "onChange"}


def is_function_param(param):
    name = (param.get("name") or "").split("=")[0].strip()
    kind = param.get("type") or ""
    if "=>" in kind:
        return True
    # A parameter named as a handler is one; a listener only when its description does not make it
    # a player (Voice.setListenerMuted's listener is the player who listens).
    if name == "listener":
        return "player" not in (param.get("description") or "").lower()
    return name in FUNCTION_NAMES


def parameters(member, types):
    """C# parameters and the call's arguments for a member."""
    declared, arguments, docs = [], [], []
    optional_seen = False
    for param in member.get("params", []):
        raw = param.get("name") or "value"
        if raw.startswith("..."):
            name = identifier(raw[3:] or "args")
            declared.append(f"params object?[] {name}")
            arguments.append(f"..{name}")
            docs.append((name, param.get("description")))
            break
        name = identifier(raw.split("=")[0].strip())
        kind = types.csharp(param.get("type"), fallback="object?")
        if kind in ("void", "JsValue"):
            kind = "object?"
        if is_function_param(param):
            # A handler: any C# lambda with typed parameters, `(Player player) => ...`.
            kind = "Delegate"
        optional = optional_param(param) or optional_seen
        if optional:
            optional_seen = True
            if kind != "object?" and not kind.endswith("?"):
                kind += "?"
            declared.append(f"{kind} {name} = null")
            arguments.append(f"Sdk.Optional({name})")
        else:
            declared.append(f"{kind} {name}")
            arguments.append(name)
        docs.append((name, param.get("description")))
    return declared, arguments, docs


def call_arguments(arguments):
    """Arguments as C# expressions; a trailing params array spreads into them."""
    if arguments and arguments[-1].startswith(".."):
        rest = arguments[-1][2:]
        head = ", ".join(arguments[:-1])
        if head:
            return f"Values.Concat(new object?[] {{ {head} }}, {rest})"
        return rest
    return ", ".join(arguments)


def return_type(member, types):
    returns = (member.get("returns") or "").strip()
    if returns in ("", "void"):
        return "void" if returns == "void" else "JsValue"
    return types.csharp(returns)


def member_name(js_name, owner, taken):
    name = pascal(js_name)
    wrapper_name = name in WRAPPER_MEMBERS and name in taken
    if name in RESERVED or name == owner or wrapper_name:
        name += "Value"
    while name in taken:
        name += "_"
    taken.add(name)
    return name


def method_lines(api_type, member, types, static, owner, taken, path):
    js_name = member["name"]
    name = member_name(js_name, owner, taken)
    declared, arguments, docs = parameters(member, types)
    result = return_type(member, types)
    lines = doc_lines(
        member.get("summary"), "    ", docs, member.get("returns"), member.get("side")
    )
    args = call_arguments(arguments)
    separator = ", " if args else ""
    if js_name == "toString" and not static and not declared:
        lines.append('    public override string ToString() => CallMember<string>("toString");')
        return lines
    signature = f"public {'static ' if static else ''}{result} {name}({', '.join(declared)})"
    if static:
        domain, type_name = path
        target = f'"{domain}", "{type_name}", "{js_name}"'
        if result == "void":
            lines.append(f"    {signature} => Sdk.Call({target}{separator}{args});")
        else:
            lines.append(f"    {signature} => Sdk.Call<{result}>({target}{separator}{args});")
    else:
        if result == "void":
            lines.append(f'    {signature} => CallMember("{js_name}"{separator}{args});')
        else:
            lines.append(f'    {signature} => CallMember<{result}>("{js_name}"{separator}{args});')
    return lines


def property_lines(member, types, static, owner, taken, path):
    js_name = member["name"]
    name = member_name(js_name, owner, taken)
    kind = types.csharp(member.get("type"))
    if kind == "void":
        kind = "JsValue"
    lines = doc_lines(member.get("summary"), "    ", side=member.get("side"))
    if static:
        domain, type_name = path
        target = f'"{domain}", "{type_name}", "{js_name}"'
        lines.append(f"    public static {kind} {name}")
        lines.append("    {")
        lines.append(f"        get => Sdk.Get<{kind}>({target});")
        if not member.get("readonly"):
            lines.append(f"        set => Sdk.Set({target}, value);")
        lines.append("    }")
    else:
        lines.append(f"    public {kind} {name}")
        lines.append("    {")
        lines.append(f'        get => GetMember<{kind}>("{js_name}");')
        if not member.get("readonly"):
            lines.append(f'        set => SetMember("{js_name}", value);')
        lines.append("    }")
    return lines


def members_of(api_type):
    seen = set()
    for member in api_type.get("members", []):
        if member.get("kind") not in ("method", "property") or "." in member["name"]:
            continue
        static = ts.is_static(api_type["name"], member)
        key = (member["name"], static)
        if key in seen:
            continue
        seen.add(key)
        yield member, static


def type_lines(api_type, types, classes):
    name = api_type["name"]
    domain = api_type["domain"]
    path = (domain, name)
    lines = doc_lines(api_type.get("summary"), "", side=api_type.get("side"))
    # A class's members cannot take the wrapper's names; a static class has no wrapper.
    taken = set(WRAPPER_MEMBERS) if name in classes else set()
    if name in VALUE_TYPES:
        value = VALUE_TYPES[name]
        lines.append(f"public static class {name}")
        lines.append("{")
        lines.append("    /// <summary>The value now (it changes as the game reports).</summary>")
        lines.append(
            f'    public static {types.paths.get(value, "JsValue")}? Current => Sdk.Live<{types.paths.get(value, "JsValue")}?>("{domain}", "{name}");'
        )
        lines.append("}")
        return lines
    body = []
    if name in classes:
        base = api_type.get("base") or ""
        parent = types.paths.get(base, "JsObject")
        lines.append(f"public partial class {name} : {parent}")
        body.append(
            "    /// <summary>Wraps the SDK's JavaScript object (done for you where the SDK answers one).</summary>"
        )
        body.append(f"    public {name}(JSObject handle)")
        body.append("        : base(handle)")
        body.append("    {")
        body.append("    }")
        body.append("")
        body.append(
            f"    /// <summary>A new {name}, made by the SDK's constructor (<c>new {name}(...)</c> in JavaScript).</summary>"
        )
        body.append(f"    public {name}(params object?[] args)")
        body.append(f'        : base(Sdk.Construct("{domain}", "{name}", args))')
        body.append("    {")
        body.append("    }")
    else:
        lines.append(f"public static class {name}")
    for member, static in members_of(api_type):
        if name not in classes and not static:
            continue
        body.append("")
        member = as_method(member) or member
        if member["kind"] == "method":
            body += method_lines(api_type, member, types, static, name, taken, path)
        else:
            body += property_lines(member, types, static, name, taken, path)
    lines.append("{")
    lines += body
    lines.append("}")
    return lines


def event_lines(api_types):
    """The events' names as constants, each with its arguments in its docs."""
    events = {}
    for api_type in api_types:
        for member in api_type.get("members", []):
            if member.get("kind") == "event" and member["name"] not in events:
                events[member["name"]] = member
    lines = [
        "/// <summary>",
        "/// The SDK's events by name, for <c>Events.On(EventNames.PlayerReady, (Player player) => ...)</c>.",
        "/// A handler's parameters take the event's arguments in order, converted to their types.",
        "/// </summary>",
        "public static class EventNames",
        "{",
    ]
    taken = set()
    for event_name in sorted(events):
        member = events[event_name]
        arguments = ", ".join(param.get("name") or "" for param in member.get("params", []))
        summary = member.get("summary") or ""
        text = f"{summary} Arguments: ({arguments})." if arguments else summary
        lines += doc_lines(text, "    ", side=member.get("side"))
        lines.append(
            f'    public const string {member_name(event_name, "EventNames", taken)} = "{event_name}";'
        )
        lines.append("")
    if lines[-1] == "":
        lines.pop()
    lines.append("}")
    return lines


HEADER = [
    "// <auto-generated>",
    "// The KCDS SDK for C#, generated from the SDK's own API by Dev/tools/web/build_csharp_sdk.py.",
    "// Do not edit: regenerate it (the parity check fails when it differs from the API).",
    "// </auto-generated>",
    "#nullable enable",
    "#pragma warning disable CS1573, CS1572, CS0108, CS0114",
    "using System;",
    "using System.Runtime.InteropServices.JavaScript;",
    "using System.Threading.Tasks;",
    "using Kcds.Runtime;",
    "",
]


def build(api):
    api_types = [t for t in api["types"] if t.get("domain") in NAMESPACES]
    classes = set()
    for api_type in api_types:
        if any(not static for _member, static in members_of(api_type)):
            classes.add(api_type["name"])
    # A base type is a class too (Player's Entity).
    for api_type in api_types:
        if api_type.get("base"):
            classes.add(api_type["base"])
    types = Types(api_types, classes)
    files = {}
    for domain, namespace in NAMESPACES.items():
        lines = HEADER + [f"namespace Kcds.{namespace};", ""]
        for api_type in api_types:
            if api_type["domain"] != domain:
                continue
            if not api_type.get("members") and api_type["name"] not in VALUE_TYPES:
                continue
            lines += type_lines(api_type, types, classes) + [""]
        if domain == "core":
            lines += event_lines(api["types"]) + [""]
        files[f"{namespace}.cs"] = "\n".join(lines).rstrip() + "\n"
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("api")
    parser.add_argument("out", nargs="?", default=str(DEFAULT_OUT))
    parser.add_argument("--check", action="store_true")
    options = parser.parse_args()
    api = json.loads(Path(options.api).read_text(encoding="utf-8"))
    out = Path(options.out)
    files = build(api)
    if options.check:
        stale = []
        for name, text in files.items():
            path = out / name
            if not path.is_file() or path.read_text(encoding="utf-8").replace("\r\n", "\n") != text:
                stale.append(name)
        extra = [path.name for path in out.glob("*.cs") if path.name not in files]
        if stale or extra:
            sys.exit(
                f"build_csharp_sdk: the C# SDK differs from the API ({', '.join(stale + extra)}); regenerate it"
            )
        print("build_csharp_sdk: the C# SDK matches the API")
        return
    out.mkdir(parents=True, exist_ok=True)
    for name, text in files.items():
        (out / name).write_text(text, encoding="utf-8", newline="\n")
        print(f"{name}: {text.count(chr(10))} lines")


if __name__ == "__main__":
    main()
