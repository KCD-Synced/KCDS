"""Builds the collision kinds of the prop catalog's meshes from a game installation.

A .cgf (the CryEngine chunk file, 0x746 "CrCh") keeps a mesh's collision as physics data chunks
(0x1018, the geometry type at + 92: 0 box, 1 triangle mesh, 4 sphere, 5 cylinder, 6 capsule) that
mesh chunks (0x1000, their four physics data ids at + 92, their vertex count at + 8) point at.
The drawn mesh either collides as it is drawn (its own mesh chunk has vertices and physics), or
through proxies: meshes without vertices under nodes named "$physics_proxy...", a box, cylinder,
capsule, sphere or a rough mesh around it. So each catalog mesh is

  mesh   something collides with the mesh as it is drawn
  proxy  only with its simplified shapes (a hanged body falls through the gaps a proxy leaves:
         witch_stake.cgf has a capsule and a rough mesh, no solid top)
  none   nothing collides with it

    python Dev/tools/props/build_collision.py "<game folder>"

writes Backend/scripting/runtimes/js/props/collision.json: the "mesh" and "none" meshes per
folder, as the catalog stores its paths; every other catalog mesh is "proxy".
"""

import argparse
import json
import struct
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CATALOG = ROOT / "Backend/scripting/runtimes/js/props/catalog.json"
OUTPUT = ROOT / "Backend/scripting/runtimes/js/props/collision.json"

CHUNK_MESH = 0x1000
CHUNK_PHYSICS = 0x1018
MESH_VERTICES = 8
MESH_PHYSICS = 92


def chunk_table(data):
    """The chunk file's chunks as (type, offset, size); empty for another format."""
    if data[:4] != b"CrCh":
        return []
    _, count, table = struct.unpack_from("<III", data, 4)
    chunks = []
    for index in range(count):
        kind, _, _, size, offset = struct.unpack_from("<HHIII", data, table + index * 16)
        chunks.append((kind, offset, size))
    return chunks


def kind_of(data):
    """mesh, proxy or none for a .cgf's bytes; None for a file this tool cannot read."""
    try:
        chunks = chunk_table(data)
        if not chunks:
            return None
        drawn_collides = False
        proxy_collides = False
        for kind, offset, size in chunks:
            if kind != CHUNK_MESH or size < MESH_PHYSICS + 16:
                continue
            vertices = struct.unpack_from("<i", data, offset + MESH_VERTICES)[0]
            physics = struct.unpack_from("<4i", data, offset + MESH_PHYSICS)
            if not any(chunk > 0 for chunk in physics):
                continue
            if vertices > 0:
                drawn_collides = True
            else:
                proxy_collides = True
    except struct.error:
        return None
    if drawn_collides:
        return "mesh"
    return "proxy" if proxy_collides else "none"


def build(game):
    catalog = json.loads(CATALOG.read_text(encoding="utf8"))
    wanted = {f"{folder}/{file}" for folder, files in catalog["folders"].items() for file in files}
    kinds = {}
    for pak_path in sorted((Path(game) / "Data").glob("*.pak")):
        with zipfile.ZipFile(pak_path) as pak:
            for name in pak.namelist():
                path = name.replace("\\", "/").lower()
                if path in wanted and path not in kinds:
                    kind = kind_of(pak.read(name))
                    if kind:
                        kinds[path] = kind
    output = {"source": "Data/*.pak Objects/**/*.cgf physics chunks", "mesh": {}, "none": {}}
    for path in sorted(kinds):
        if kinds[path] in ("mesh", "none"):
            folder, _, file = path.rpartition("/")
            output[kinds[path]].setdefault(folder, []).append(file)
    return output, kinds


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("game", help="the game's installation folder")
    args = parser.parse_args()
    output, kinds = build(args.game)
    OUTPUT.write_text(json.dumps(output, separators=(",", ":")) + "\n", encoding="utf8")
    counts = {
        kind: sum(1 for value in kinds.values() if value == kind)
        for kind in ("mesh", "proxy", "none")
    }
    print(f"{len(kinds)} meshes: {counts}")


if __name__ == "__main__":
    main()
