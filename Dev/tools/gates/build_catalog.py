"""Builds the catalog of the levels' gate series from a game installation.

A gate series is a folder of level layers named series_...gate..., ...bars..., ...portcullis...
or ...drawbridge... with two or more states (open, closed, barricaded, ...); the game shows one
state's layers at a time. For each series of trosecko and kutnohorsko this lists its states with
their layer ids (leveldata.xml, the id a CBrush keeps at + 0x92) and the static mesh entities
(GeomEntity and the like with a .cgf) each state's layers hold, with their guids and placement.
The brushes of those layers live in the level's object tree, not in these files: the client
finds them by layer id around the series' centre.

Left out: the two animated gates (Malesov's drawbridge and Suchdol's portcullis, kGates in
gates.hpp), series with a single state, and series whose leaves are doors (AnimDoor entities,
kept in step by doors/).

    python Dev/tools/gates/build_catalog.py "<game folder>"

writes Backend/scripting/runtimes/js/gates/catalog.json (the server) and
Backend/integration/include/kcd2s/integration/gate_series.hpp (the client).
"""

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
JSON_OUTPUT = ROOT / "Backend/scripting/runtimes/js/gates/catalog.json"
HPP_OUTPUT = ROOT / "Backend/integration/include/kcd2s/integration/gate_series.hpp"
LEVELS = ("trosecko", "kutnohorsko")
SERIES = re.compile(r"^(.*?/series_[^/]*(?:gate|bars|portcullis|drawbridge)[^/]*)/(.+)$", re.I)
# Sub-layers a state's content is split into; the state is the folder above them.
SUBLAYERS = {"_enviro", "_common", "enviro", "common", "interactable", "_interactable"}
ANIMATED = {
    "kutnohorsko": {
        "kmal_malesov/fortress/series_drawbridge",
        "ksuc_suchdol/fortress/series_portcullis",
    }
}
ENTITY = re.compile(rb"<Entity [^>]*>")
FIELD = re.compile(rb'(\w+)="([^"]*)"')
LAYER = re.compile(rb'<Layer Name="([^"]+)" Id="(\d+)"')


def entries(pak_path):
    """Every file of a level.pak as (name, bytes); its local headers are read for the data
    (some entries' central names differ from their local ones)."""
    with zipfile.ZipFile(pak_path) as pak, open(pak_path, "rb") as raw:
        for info in pak.infolist():
            raw.seek(info.header_offset)
            header = raw.read(30)
            name_length, extra_length = struct.unpack("<HH", header[26:30])
            raw.seek(info.header_offset + 30 + name_length + extra_length)
            data = raw.read(info.compress_size)
            if info.compress_type == zipfile.ZIP_DEFLATED:
                data = zlib.decompress(data, -15)
            elif info.compress_type != zipfile.ZIP_STORED:
                continue
            yield info.filename, data


def numbers(text, count, default):
    parts = [float(part) for part in text.split(",")] if text else []
    return parts if len(parts) == count else list(default)


def guid64(text):
    """An EntityGuid "10cbd99b-495b-4d4d" as the 64-bit key the entity system holds: its parts
    laid out in memory in that order, 0x4d4d495b10cbd99b (parse_level_edit_guid)."""
    digits = text.replace("-", "")
    if not re.fullmatch(r"[0-9a-fA-F]{16}", digits):
        return 0
    return int(digits[0:8], 16) | (int(digits[8:12], 16) << 32) | (int(digits[12:16], 16) << 48)


def split_layer(editor_layer):
    """(series path, state) of an EditorLayer path, or None outside a gate series."""
    match = SERIES.match(editor_layer)
    if not match:
        return None
    series, rest = match.group(1), match.group(2).split("/")
    while len(rest) > 1 and rest[-1].lower() in SUBLAYERS:
        rest.pop()
    if len(rest) > 1:
        # A group inside the series (series_gates/palisade/closed): the group is part of it.
        series = series + "/" + "/".join(rest[:-1])
    return series.removeprefix("Main/"), rest[-1]


def level_series(game, level):
    pak_path = Path(game) / "Data/Levels" / level / "level.pak"
    layer_ids = {}
    series = {}
    files = list(entries(pak_path))
    for name, data in files:
        if name == "leveldata.xml":
            layer_ids = {key.decode(): int(value) for key, value in LAYER.findall(data)}
    for name, data in files:
        if not (name.startswith("layers/") and name.endswith(".xml")):
            continue
        layer = name[len("layers/") : -len(".xml")]
        for tag in ENTITY.finditer(data):
            fields = {
                k.decode(): v.decode("utf-8", "replace") for k, v in FIELD.findall(tag.group(0))
            }
            split = split_layer(fields.get("EditorLayer", ""))
            if not split:
                continue
            key, state_name = split
            if key in ANIMATED.get(level, set()):
                continue
            entry = series.setdefault(key, {"states": {}, "doors": False, "points": []})
            state = entry["states"].setdefault(state_name, {"layers": set(), "entities": []})
            if layer in layer_ids:
                state["layers"].add(layer_ids[layer])
            position = numbers(fields.get("Pos"), 3, (0.0, 0.0, 0.0))
            entry["points"].append(position)
            if fields.get("EntityClass") == "AnimDoor":
                entry["doors"] = True
            mesh = fields.get("Geometry", "").replace("\\", "/").lower()
            if mesh.endswith(".cgf"):
                w, x, y, z = numbers(fields.get("Rotate"), 4, (1.0, 0.0, 0.0, 0.0))
                state["entities"].append(
                    {
                        "guid": guid64(fields.get("EntityGuid", "")),
                        "mesh": mesh,
                        "position": position,
                        "rotation": [x, y, z, w],
                        "scale": numbers(fields.get("Scale"), 3, (1.0, 1.0, 1.0)),
                    }
                )
    catalog = []
    for key in sorted(series):
        entry = series[key]
        if entry["doors"] or len(entry["states"]) < 2:
            continue
        points = entry["points"]
        center = [round(sum(p[axis] for p in points) / len(points), 3) for axis in range(3)]
        states = []
        for state_name in sorted(entry["states"]):
            state = entry["states"][state_name]
            states.append(
                {
                    "name": state_name,
                    "layers": sorted(state["layers"]),
                    "entities": sorted(state["entities"], key=lambda e: (e["mesh"], e["guid"])),
                }
            )
        catalog.append(
            {"key": key, "map": level, "center": center, "states": states, "bar": bar_state(states)}
        )
    return catalog


def bar_state(states):
    """The closed state of a closed/open series whose leaves include a bar: a mesh the open
    state does not have (the wooden bar across the leaves). None without one."""
    if len(states) != 2:
        return None
    closed = [s for s in states if "clos" in s["name"].lower()]
    opened = [s for s in states if "open" in s["name"].lower() and "clos" not in s["name"].lower()]
    if len(closed) != 1 or len(opened) != 1:
        return None
    open_meshes = {e["mesh"] for e in opened[0]["entities"]}
    if any(e["mesh"] not in open_meshes for e in closed[0]["entities"]):
        return closed[0]["name"]
    return None


def cpp_float(value):
    """A float literal to the millimetre (and the rotations' 8 digits)."""
    text = f"{value:.8g}"
    return f"{text}f" if "." in text or "e" in text else f"{text}.0f"


def write_hpp(catalog):
    lines = [
        "// Generated by Dev/tools/gates/build_catalog.py from the game's levels; do not edit.",
        "// The levels' gate series: see gates.hpp for how they are shown.",
        "#pragma once",
        "#include <array>",
        "#include <cstdint>",
        "#include <string_view>",
        "",
        "namespace kcd2s::integration {",
        "",
        "struct GateLeafEntity {",
        "  std::uint64_t guid;  // its EntityGuid",
        "  std::string_view mesh;",
        "  std::array<float, 3> position;",
        "  std::array<float, 4> rotation;  // x, y, z, w",
        "  std::array<float, 3> scale;",
        "};",
        "",
        "struct GateSeriesState {",
        "  std::string_view name;",
        "  const std::uint16_t* layers;  // its level layers' ids",
        "  std::size_t layer_count;",
        "  const GateLeafEntity* entities;  // the static mesh entities its layers hold",
        "  std::size_t entity_count;",
        "};",
        "",
        "struct GateSeries {",
        "  std::string_view key;  // the series' path in the level, the server API's name",
        "  std::string_view map;",
        "  std::array<float, 3> center;",
        "  const GateSeriesState* states;",
        "  std::size_t state_count;",
        "  // The state whose leaves include a bar across the gate (a mesh the open state does not",
        "  // have), which the server may take out and put back; -1 none.",
        "  int bar_state;",
        "};",
        "",
        "namespace gate_series_data {",
    ]
    for s_index, series in enumerate(catalog):
        for t_index, state in enumerate(series["states"]):
            prefix = f"s{s_index}_{t_index}"
            layers = ", ".join(str(layer) for layer in state["layers"]) or "0"
            lines.append(f"inline constexpr std::uint16_t {prefix}_layers[] = {{{layers}}};")
            if state["entities"]:
                lines.append(f"inline constexpr GateLeafEntity {prefix}_entities[] = {{")
                for e in state["entities"]:
                    position = ", ".join(cpp_float(v) for v in e["position"])
                    rotation = ", ".join(cpp_float(v) for v in e["rotation"])
                    scale = ", ".join(cpp_float(v) for v in e["scale"])
                    lines.append(
                        f'    {{0x{e["guid"]:016x}ull, "{e["mesh"]}", {{{position}}}, {{{rotation}}}, '
                        f"{{{scale}}}}},"
                    )
                lines.append("};")
        lines.append(f"inline constexpr GateSeriesState s{s_index}_states[] = {{")
        for t_index, state in enumerate(series["states"]):
            prefix = f"s{s_index}_{t_index}"
            entities = f"{prefix}_entities" if state["entities"] else "nullptr"
            layer_count = len(state["layers"])
            lines.append(
                f'    {{"{state["name"]}", {prefix}_layers, {layer_count}, {entities}, '
                f'{len(state["entities"])}}},'
            )
        lines.append("};")
    lines.append("}  // namespace gate_series_data")
    lines.append("")
    lines.append(f"inline constexpr std::array<GateSeries, {len(catalog)}> kGateSeries = {{{{")
    for s_index, series in enumerate(catalog):
        center = ", ".join(cpp_float(v) for v in series["center"])
        names = [state["name"] for state in series["states"]]
        bar = names.index(series["bar"]) if series["bar"] else -1
        lines.append(
            f'    {{"{series["key"]}", "{series["map"]}", {{{center}}}, '
            f"gate_series_data::s{s_index}_states, {len(series['states'])}, {bar}}},"
        )
    lines.append("}};")
    lines.append("")
    lines.append("}  // namespace kcd2s::integration")
    HPP_OUTPUT.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    # The pinned formatter lays it out as the format check expects.
    clang = shutil.which(os.environ.get("KCDS_CLANG_FORMAT", "clang-format"))
    if clang:
        subprocess.run([clang, "-i", str(HPP_OUTPUT)], check=True)
    else:
        print("clang-format not found: format gate_series.hpp before review")


def first_guid(series):
    """A series has no entity of its own: its identity is the lowest GUID of what it shows."""
    guids = [e["guid"] for state in series["states"] for e in state["entities"]]
    return f"{min(guids):016x}" if guids else ""


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("game", help="the game's folder (the one holding Data/)")
    arguments = parser.parse_args()
    catalog = []
    for level in LEVELS:
        catalog.extend(level_series(arguments.game, level))
    server = [
        {
            "key": s["key"],
            "map": s["map"],
            "center": s["center"],
            "guid": first_guid(s),
            "states": [state["name"] for state in s["states"]],
            "bar": s["bar"],
        }
        for s in catalog
    ]
    JSON_OUTPUT.write_text(json.dumps(server, indent=2) + "\n", encoding="utf-8", newline="\n")
    # The pinned formatter lays it out as the format check expects (KCDS_PRETTIER: its script).
    prettier = os.environ.get("KCDS_PRETTIER")
    command = [shutil.which("node") or "node", prettier] if prettier else None
    if not command and shutil.which("npx"):
        command = [shutil.which("npx"), "--no-install", "prettier"]
    if command:
        config = str(ROOT / ".prettierrc.json")
        subprocess.run([*command, "--config", config, "--write", str(JSON_OUTPUT)], check=False)
    write_hpp(catalog)
    for series in catalog:
        states = ", ".join(
            f'{s["name"]}({len(s["layers"])}L/{len(s["entities"])}E)' for s in series["states"]
        )
        print(f'{series["map"]:12} {series["key"]}: {states}')
    print(f"{len(catalog)} series")


if __name__ == "__main__":
    main()
