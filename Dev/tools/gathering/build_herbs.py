"""Builds the levels' own herbs from a game installation and a kit sweep.

A level's pickable plants come from two places, and the game's respawn manager registers both as
they stream in around the player (Client/runtime/gathering/herbs.hpp):

- herbs (nettles, sage, thistles, ...) are auto-merged vegetation: the level's
  terrain/merged_meshes_sectors/sector_<x>_<y>_<z>_0.dat files in terrain.pak, one per 16 m cell
  ("n" before a negative index). A file is chunks of [magic 0xCAFEBAB6][cell x, y, z][mesh key]
  [count] and count samples of 12 bytes: the position in the cell (three u16, 0..65535 over 16 m),
  the turn (four i8) and the scale. Nothing in the files says which mesh key is which pickable
  kind, so the sweep below names them: a key whose samples stand where the game registered plants
  of one kind is that kind (CMergedMeshRenderNode);
- the rest (mushrooms) are ordinary vegetation the game registers out to a few hundred metres.

The sweep: a kit client (started with KCDS_TEST_HOOKS) stands at stops over the level and at each
signals Local\\KCD2S.HerbDump.<pid>; the client writes every plant the manager holds then to
%TEMP%\\kcds-herbs-<pid>.csv, one "kind,x,y,z" line each. Stops 500 m apart register the mushrooms;
herbs register only within about 25 m, so a few stops near the merged meshes' samples (a cover of
every mesh key) name the herbs' keys. Each stop's file is kept under its own name in one folder.

    python Dev/tools/gathering/build_herbs.py "<game folder>" <level> <folder of the sweep's files>
        [--also <other level>[=<folder of its sweep's files>] ...]

writes the level's plants to Backend/scripting/runtimes/js/gathering/herbs/<level>.bin.gz: for each
kind [u8 kind][u32 count] and count plants of [i32 dx][i32 dy][i32 dz], each kind's plants sorted,
in centimetres, each from the one before (little-endian; gzip, mtime 0: the same input gives the
same bytes): every sample of a named herb key, and every swept plant that is not a merged mesh. A
binary file because the server reads it into typed arrays at start without a JSON parse's memory.

The levels share their mesh keys (a key is the mesh's), so --also writes the herbs of other levels
from the keys the sweep named. Their mushrooms need a sweep of their own: with "=<folder>" the
other level's sweep adds its plants that are not a merged mesh, and names the keys the first sweep
left unnamed (the first sweep's names win where both name a key).
"""

import argparse
import collections
import csv
import gzip
import json
import re
import struct
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Backend/scripting/runtimes/js/gathering/herbs"
KINDS = ROOT / "Backend/scripting/runtimes/js/gathering/kinds.json"
SECTOR = re.compile(r"merged_meshes_sectors/sector_(n?)(\d+)_(n?)(\d+)_(n?)(\d+)_0\.dat$")
MAGIC = 0xCAFEBAB6
CELL = 16.0
# A swept plant is a merged mesh's sample when they stand this close (metres).
SAME_PLACE = 0.05
# A mesh key is a kind's when this many swept plants name it and this share of them agree.
FEWEST_VOTES = 2
AGREEMENT = 0.9
# The mushrooms (Muchomurka, Hrib, Bedla) are ordinary vegetation the game registers out to a few
# hundred metres, so the sweep has them; no merged mesh is named after them. The fly agaric's
# merged mesh also stands where the game grows no pickable plant (none of its
# samples within 5 m of the player was registered).
MUSHROOMS = {23, 28, 31}


def read(pak, info):
    try:
        return pak.read(info)
    except (KeyError, zipfile.BadZipFile, NotImplementedError):
        # The game's paks name their files with backslashes in the local headers.
        info.orig_filename = info.filename.replace("/", "\\")
        return pak.open(info).read()


def merged_samples(game, level):
    """Every merged-mesh sample of the level: mesh key -> [(x, y, z)]."""
    samples = collections.defaultdict(list)
    with zipfile.ZipFile(Path(game) / "Data/Levels" / level / "terrain.pak") as pak:
        for info in pak.infolist():
            match = SECTOR.search(info.filename.replace("\\", "/"))
            if not match:
                continue
            signs = [-1 if match.group(g) else 1 for g in (1, 3, 5)]
            cells = [int(match.group(g)) for g in (2, 4, 6)]
            origin = [signs[a] * cells[a] * CELL for a in range(3)]
            data = read(pak, info)
            offset = 0
            while offset + 24 <= len(data):
                magic, _, _, _, key, count = struct.unpack_from("<6I", data, offset)
                if magic != MAGIC:
                    break
                offset += 24
                for index in range(count):
                    u = struct.unpack_from("<3H", data, offset + 12 * index)
                    samples[key].append(tuple(origin[a] + u[a] / 65535.0 * CELL for a in range(3)))
                offset += 12 * count
    return samples


def swept_plants(folder, kinds):
    """Every plant of a known kind the sweep's files name, once."""
    plants = set()
    for path in sorted(Path(folder).glob("*.csv")):
        with path.open(newline="", encoding="utf-8") as file:
            for row in csv.reader(file):
                if len(row) != 4:
                    continue
                try:
                    kind = int(row[0])
                    x, y, z = (round(float(value), 2) for value in row[1:])
                except ValueError:
                    continue
                if kind in kinds:
                    plants.add((kind, x, y, z))
    return plants


def name_keys(samples, plants):
    """The kind of each mesh key the swept plants name, and the plants that are no sample."""
    cells = collections.defaultdict(list)
    for key, points in samples.items():
        for point in points:
            cells[(int(point[0] // 1), int(point[1] // 1))].append((key, point))
    votes = collections.defaultdict(collections.Counter)
    elsewhere = []
    for kind, x, y, z in plants:
        best = None
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for key, point in cells.get((int(x // 1) + dx, int(y // 1) + dy), []):
                    distance = (
                        (point[0] - x) ** 2 + (point[1] - y) ** 2 + (point[2] - z) ** 2
                    ) ** 0.5
                    if best is None or distance < best[0]:
                        best = (distance, key)
        if best and best[0] <= SAME_PLACE:
            votes[best[1]][kind] += 1
        else:
            elsewhere.append((kind, x, y, z))
    named = {}
    for key, counter in votes.items():
        kind, count = counter.most_common(1)[0]
        agreed = count >= FEWEST_VOTES and count >= AGREEMENT * sum(counter.values())
        if agreed and kind not in MUSHROOMS:
            named[key] = kind
    return named, elsewhere


def encoded(plants):
    """For each kind [u8 kind][u32 count][i32 dx, dy, dz ...]: its plants sorted, in centimetres,
    each from the one before (the first from the origin)."""
    by_kind = collections.defaultdict(list)
    for kind, x, y, z in plants:
        by_kind[kind].append((round(x * 100), round(y * 100), round(z * 100)))
    result = bytearray()
    for kind, points in sorted(by_kind.items()):
        result += struct.pack("<BI", kind, len(points))
        before = (0, 0, 0)
        for point in sorted(points):
            result += struct.pack("<3i", *(point[axis] - before[axis] for axis in range(3)))
            before = point
    return bytes(result)


def write(level, plants):
    """Writes a level's catalog and says what is in it."""
    plants = sorted(plants)
    OUTPUT.mkdir(parents=True, exist_ok=True)
    path = OUTPUT / f"{level}.bin.gz"
    path.write_bytes(gzip.compress(encoded(plants), mtime=0))
    by_kind = collections.Counter(plant[0] for plant in plants)
    print(f"{path}: {len(plants)} plants")
    print("by kind:", dict(sorted(by_kind.items())))


def herbs_of(samples, named):
    """Every sample of a named key, as (kind, x, y, z) to the centimetre."""
    plants = set()
    for key, kind in named.items():
        for x, y, z in samples.get(key, []):
            plants.add((kind, round(x, 2), round(y, 2), round(z, 2)))
    return plants


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("game", type=Path, help="the game's installation folder")
    parser.add_argument("level", help="the level the sweep was made on, e.g. trosecko")
    parser.add_argument("folder", type=Path, help="the folder of the sweep's files")
    parser.add_argument(
        "--also",
        nargs="*",
        default=[],
        help="other levels to write the herbs of, each optionally =<folder of its own sweep>",
    )
    arguments = parser.parse_args()
    kinds = {entry["id"] for entry in json.loads(KINDS.read_text(encoding="utf-8"))}
    samples = merged_samples(arguments.game, arguments.level)
    swept = swept_plants(arguments.folder, kinds)
    named, elsewhere = name_keys(samples, swept)
    print(f"{len(named)} herb meshes named on {arguments.level}")
    write(arguments.level, herbs_of(samples, named) | set(elsewhere))
    for entry in arguments.also:
        write_other_level(arguments.game, entry, named, kinds)


def write_other_level(game, entry, named, kinds):
    """Writes another level from the first sweep's key names and, when given, its own sweep."""
    level, _, folder = entry.partition("=")
    samples = merged_samples(game, level)
    if not folder:
        write(level, herbs_of(samples, named))
        return
    own_named, own_elsewhere = name_keys(samples, swept_plants(folder, kinds))
    combined = dict(own_named)
    combined.update(named)
    added = len(combined) - len(named)
    differing = [key for key in own_named if key in named and own_named[key] != named[key]]
    print(f"{level}: its sweep names {len(own_named)} herb meshes, {added} more than the first")
    if differing:
        print(f"{level}: its sweep names {len(differing)} meshes otherwise, the first sweep wins")
    write(level, herbs_of(samples, combined) | set(own_elsewhere))


if __name__ == "__main__":
    main()
