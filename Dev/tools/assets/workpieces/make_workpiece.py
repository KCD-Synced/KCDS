"""Makes a smithing workpiece of one of the game's static models (a .cgf), for
Crafting.defineWorkpiece: the anvil takes a character whose skin attachments the hammer strikes
and deforms, so a prop alone cannot lie in the tongs.

    python Dev/tools/assets/workpieces/make_workpiece.py
        --mesh objects/manmade/task_specific_props/metal_industry/smithing/semifinished_helm.cgf
        --out <resource>/stream/objects/kcds/<resource> --name helm_workpiece
        [--game "<KCD2 folder>"] [--fit 1.2] [--flat 0.25]

It writes <name>.skin and <name>_hot.skin: the model's mesh bound to the root bone ("base") of the
game's axe workpiece skeleton, each with the morph target the anvil drives as the piece is worked
("#Axe" and "#hot_01", the axe's attachment names, so the workpiece's like: 'axe' row strikes it).
The mesh starts flattened to --flat of its height (a rough sheet) and the morph raises it to the
model's own shape. It lies where the axe's piece does, --fit times as wide: the anvil judges the
hammer's strikes on the axe piece's strike map, so the model covers that map. The glow skin's
vertex alpha fades from the middle to the rim, as the axe's fades at its edges.

The resource's .cdf binds the two skins as "Axe" and "hot_01" with a material of its own. The
game's files are read from the player's own installation (its Data/*.pak) and never copied into
KCDS: run this on a PC with the game and put the output in the resource's stream folder.
"""

import argparse
import math
import os
import struct
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "blender"))

import kcds_cgf  # noqa: E402
import kcds_skin  # noqa: E402

DEFAULT_GAME = r"C:\Program Files (x86)\Steam\steamapps\common\KingdomComeDeliverance2"
AXE_SKIN = "objects/characters/assets/blacksmithing/blacksmithing_axe.skin"
ROOT_BONE = "base"
STREAM_P3S_C4B_T2S = 15
STREAM_INDICES = 5


class GameFiles:
    """The files of the game's packs, by their lower-case path."""

    def __init__(self, game):
        self.where = {}
        data = os.path.join(game, "Data")
        for name in sorted(os.listdir(data)):
            if not name.lower().endswith(".pak"):
                continue
            try:
                archive = zipfile.ZipFile(os.path.join(data, name))
            except zipfile.BadZipFile:
                continue
            for member in archive.namelist():
                self.where.setdefault(member.lower(), (archive, member))

    def read(self, path):
        found = self.where.get(path.lower().replace("\\", "/"))
        if found is None:
            raise SystemExit(f"the game has no {path}")
        archive, member = found
        return archive.read(member)


def static_mesh(files, model):
    """The drawn triangles of a .cgf (its geometry in the .cgfm next to it): positions, uvs and
    indices of the subsets whose material is drawn."""
    chunks = kcds_skin.read_chunks(files.read(model + "m"))
    streams = {}
    subsets = []
    for chunk in chunks:
        if chunk.kind == kcds_skin.CHUNK_DATA_STREAM:
            _, kind, count, size, _, _ = struct.unpack_from("<6i", chunk.body, 0)
            streams[kind] = (count, size, chunk.body[24:])
        elif chunk.kind == kcds_skin.CHUNK_SUBSETS:
            _, count, _, _ = struct.unpack_from("<4i", chunk.body, 0)
            for k in range(count):
                subsets.append(struct.unpack_from("<5i", chunk.body, 16 + k * 36))
    if STREAM_P3S_C4B_T2S not in streams or STREAM_INDICES not in streams:
        raise SystemExit(f"{model}: no half-float vertices and indices (another kind of mesh)")
    count, size, body = streams[STREAM_P3S_C4B_T2S]
    vertices = [struct.unpack_from("<4e4B2e", body, k * size) for k in range(count)]
    count, size, body = streams[STREAM_INDICES]
    indices = struct.unpack_from(f"<{count}H", body, 0)
    # The largest subset is the model's drawn surface (the others are its physics proxies).
    first_index, index_count, _, _, _ = max(subsets, key=lambda subset: subset[1])
    triangles = indices[first_index : first_index + index_count]
    used = sorted(set(triangles))
    remap = {old: new for new, old in enumerate(used)}
    positions = [tuple(vertices[v][0:3]) for v in used]
    uvs = [tuple(vertices[v][8:10]) for v in used]
    return positions, uvs, [remap[v] for v in triangles]


def normals_of(positions, indices):
    """Per vertex the area-weighted mean of its triangles' normals."""
    sums = [[0.0, 0.0, 0.0] for _ in positions]
    for at in range(0, len(indices), 3):
        a, b, c = indices[at : at + 3]
        u = [positions[b][k] - positions[a][k] for k in range(3)]
        v = [positions[c][k] - positions[a][k] for k in range(3)]
        n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
        for vertex in (a, b, c):
            for k in range(3):
                sums[vertex][k] += n[k]
    out = []
    for n in sums:
        length = math.sqrt(sum(part * part for part in n)) or 1.0
        out.append(tuple(part / length for part in n))
    return out


def tangents_of(positions, uvs, normals, indices):
    """Per vertex a tangent and bitangent along the uv, with the handedness."""
    tan = [[0.0, 0.0, 0.0] for _ in positions]
    bit = [[0.0, 0.0, 0.0] for _ in positions]
    for at in range(0, len(indices), 3):
        a, b, c = indices[at : at + 3]
        e1 = [positions[b][k] - positions[a][k] for k in range(3)]
        e2 = [positions[c][k] - positions[a][k] for k in range(3)]
        du1, dv1 = uvs[b][0] - uvs[a][0], uvs[b][1] - uvs[a][1]
        du2, dv2 = uvs[c][0] - uvs[a][0], uvs[c][1] - uvs[a][1]
        det = du1 * dv2 - du2 * dv1
        if abs(det) < 1e-12:
            continue
        r = 1.0 / det
        t = [(e1[k] * dv2 - e2[k] * dv1) * r for k in range(3)]
        s = [(e2[k] * du1 - e1[k] * du2) * r for k in range(3)]
        for vertex in (a, b, c):
            for k in range(3):
                tan[vertex][k] += t[k]
                bit[vertex][k] += s[k]
    out = []
    for n, t, s in zip(normals, tan, bit):
        dot = sum(n[k] * t[k] for k in range(3))
        t = [t[k] - n[k] * dot for k in range(3)]
        length = math.sqrt(sum(part * part for part in t))
        if length < 1e-9:
            t = [1.0, 0.0, 0.0] if abs(n[0]) < 0.9 else [0.0, 1.0, 0.0]
            length = 1.0
        t = [part / length for part in t]
        # The bitangent along the uv's v, and w such that the game's normal, the tangent cross the
        # bitangent times w (as the game's own skins hold it), is the vertex's normal.
        n_cross_t = (
            n[1] * t[2] - n[2] * t[1],
            n[2] * t[0] - n[0] * t[2],
            n[0] * t[1] - n[1] * t[0],
        )
        along_v = 1 if sum(n_cross_t[k] * s[k] for k in range(3)) >= 0 else -1
        b = tuple(part * along_v for part in n_cross_t)
        t_cross_b = (
            t[1] * b[2] - t[2] * b[1],
            t[2] * b[0] - t[0] * b[2],
            t[0] * b[1] - t[1] * b[0],
        )
        sign = 1 if sum(t_cross_b[k] * n[k] for k in range(3)) >= 0 else -1
        out.append((tuple(t), b, sign))
    return out


def piece_box(skin):
    """The x and y box (low, high) of a game workpiece skin's mesh: where the anvil's strike map
    and the tongs expect the piece."""
    for chunk in kcds_skin.read_chunks(skin):
        if chunk.kind != kcds_skin.CHUNK_DATA_STREAM:
            continue
        _, kind, count, size, _, _ = struct.unpack_from("<6i", chunk.body, 0)
        if kind == kcds_skin.STREAM_POSITIONS:
            points = [struct.unpack_from("<3f", chunk.body, 24 + k * size) for k in range(count)]
            low = [min(p[k] for p in points) for k in range(2)]
            high = [max(p[k] for p in points) for k in range(2)]
            return low, high
    raise SystemExit("the workpiece skin has no positions")


def glow_alpha(position, centre, radius):
    """The hot skin's alpha at a vertex: the glow full in the middle of the piece and fading to a
    third at its rim, as the axe's hot skin fades at its edges (alpha shows the glow over the
    cold metal)."""
    distance = math.hypot(position[0] - centre[0], position[1] - centre[1]) / radius
    fade = min(1.0, max(0.0, (distance - 0.35) / 0.65))
    fade = fade * fade * (3 - 2 * fade)
    return int(round(255 - fade * 170))


def workpiece_mesh(positions, uvs, indices, box, fit, flat, material):
    """The rough sheet the anvil starts from, and each vertex's delta to the model's shape: the
    model centred on the game piece's box and as wide as `fit` times its larger side (so the
    hammer's strikes land on what is drawn), its lowest point on z 0, then pressed to `flat` of
    its height."""
    low = [min(p[k] for p in positions) for k in range(3)]
    high = [max(p[k] for p in positions) for k in range(3)]
    centre = [(low[k] + high[k]) / 2 for k in range(2)]
    width = max(high[0] - low[0], high[1] - low[1])
    target = [(box[0][k] + box[1][k]) / 2 for k in range(2)]
    scale = fit * max(box[1][0] - box[0][0], box[1][1] - box[0][1]) / width
    shaped = []
    for x, y, z in positions:
        shaped.append(
            (
                (x - centre[0]) * scale + target[0],
                (y - centre[1]) * scale + target[1],
                (z - low[2]) * scale,
            )
        )
    rough = [(x, y, z * flat) for x, y, z in shaped]
    deltas = [tuple(s[k] - r[k] for k in range(3)) for s, r in zip(shaped, rough)]
    mesh = kcds_skin.SkinnedMesh()
    mesh.positions = rough
    mesh.uvs = list(uvs)
    mesh.indices = list(indices)
    mesh.normals = normals_of(rough, indices)
    mesh.tangents = tangents_of(rough, mesh.uvs, mesh.normals, indices)
    mesh.weights = [[(ROOT_BONE, 1.0)] for _ in rough]
    mesh.subsets = [kcds_cgf.Subset(0, len(indices), 0, len(rough), material)]
    # The hot skin's vertex colours as the axe's hot_01 has them: black, the alpha the glow's.
    if material == 1:
        radius = width * scale / 2
        mesh.colours = [(0, 0, 0, glow_alpha(p, target, radius)) for p in rough]
    # Named as the axe workpiece's (blacksmithing_axe.mtl): the game lights "hot_01" as it heats.
    mesh.materials = ["metal", "hot_01"]
    mesh.physicalize = [kcds_cgf.PHYSICALIZE_NONE, kcds_cgf.PHYSICALIZE_NONE]
    return mesh, deltas


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--mesh", required=True, help="the game's .cgf, objects/...")
    parser.add_argument("--out", required=True, help="the folder the .skin files go to")
    parser.add_argument("--name", required=True, help="the files' name, such as helm_workpiece")
    parser.add_argument("--material", help="the .mtl's game path without .mtl (default: <out>)")
    parser.add_argument("--game", default=DEFAULT_GAME, help="the game's folder")
    parser.add_argument("--fit", type=float, default=1.2)
    parser.add_argument("--flat", type=float, default=0.25)
    options = parser.parse_args()
    files = GameFiles(options.game)
    axe = files.read(AXE_SKIN)
    skeleton = kcds_skin.Skeleton.from_skin(axe)
    box = piece_box(axe)
    positions, uvs, indices = static_mesh(files, options.mesh)
    material = options.material
    if not material:
        stream_root = options.out.replace("\\", "/")
        at = stream_root.lower().find("objects/")
        if at < 0:
            raise SystemExit("--out is not under a stream folder's objects/: pass --material")
        material = stream_root[at:].rstrip("/") + "/" + options.name
    os.makedirs(options.out, exist_ok=True)
    for suffix, morph_name, sub_material in (("", "#Axe", 0), ("_hot", "#hot_01", 1)):
        mesh, deltas = workpiece_mesh(
            positions, uvs, indices, box, options.fit, options.flat, sub_material
        )
        data = kcds_skin.write_skin(
            mesh, skeleton, material, node_name=options.name, morph=(morph_name, deltas)
        )
        path = os.path.join(options.out, options.name + suffix + ".skin")
        with open(path, "wb") as out:
            out.write(data)
        print(f"{path}: {len(mesh.positions)} vertices, {len(indices) // 3} triangles")


if __name__ == "__main__":
    main()
