"""A World Architect custom map from scratch.

Writes %LOCALAPPDATA%\\KCDS\\maps\\kcds_map_<name>\\level.pak: a flat square of ground, SIZE metres a
side, at HEIGHT metres, with nothing on it, which World Architect then shapes and fills. Built from
the player's own game files: klaster's level description (level info, data libraries, surface
types, environment, time of day, tables) with every layer, building, plant and entity left out,
and the map's own terrain with klaster's plant kinds (none planted). The game folder is never
written: the game client mounts the map from the KCDS folder (Client/runtime/maps/custom_maps.hpp).

The level's files, as WHGame 1.5.5 reads them (the same as the Asset Studio's empty level):
  - C3DEngineLevelLoadTimeslicer::DoStep needs brushlist.txt (step 7), the
    heightmap terrain\\terrain.dat (step 10) and the vis areas terrain\\indoor.dat (step 11);
  - CTerrain::Load_T reads terrain.dat: a 32-byte header (chunk version 29, the
    terrain's size, unit size, sector size, sectors a side, height scale), the model tables
    (vegetation models, klaster's plant kinds; brush models, none; brush materials), the node
    tree and an empty object octree;
  - CTerrainNode load: each node a 44-byte chunk (version 8, holes, its box, base
    height and height step, heightmap side, palette length); a leaf (one sector) carries its
    (side x side) height words (height = base + (word >> 20) * step, the surface in the low
    bits), then the levels of detail's errors and the surface palette;
  - terrain\\cover.ctc (opened): the ground texture, one 256 x 256 sector of two BC1
    layers (colour, normal) for every node, in the node order (a leaf followed by four -1);
  - terrain\\hlods.dat (checked on open): version 2 and the root cluster only.

Usage: python make_custom_map.py <name> [--size 1024] [--height 32] [--game <game folder>]
       [--heightmap <grey picture> [--rise 100]] [--ground grass] [--no-navmesh]

The ground's look: every sector carries the same 15 ground materials (GROUND_MATERIALS), the
--ground one covering the map at first; World Architect paints the others on while the game runs
(Terrain.paint, Client/runtime/terrain/terrain_edits.hpp). A sector's palette holds at most 15
(its samples name a material in 4 bits, 15 none).

The navmesh (recast.pak beside level.pak) lets the game's people walk on the ground: 16 m tiles of
4 x 4 walkable squares at the ground's heights (no navmesh for what is built on it later).

With --heightmap the ground follows a grey picture (8 or 16 bits; black at HEIGHT, white RISE
metres above, the picture's top the north), scaled to the map; it needs numpy and Pillow.
"""

import argparse
import os
import re
import struct
import sys
import zipfile

DEFAULT_GAME = r"C:/Program Files (x86)/Steam/steamapps/common/KingdomComeDeliverance2"
NAME = re.compile(r"^[a-z0-9_]{1,32}$")

SECTOR_SIZE = 64
# The levels of detail of a sector: 64 m halved down to one unit (1 m) is six.
LOD_LEVELS = 6
# The ground's look, footsteps and physics: klaster's surface types (leveldata.xml SurfaceTypes,
# their index there) every sector carries, by the name World Architect and Terrain.paint give them.
GROUND_MATERIALS = {
    "grass": 18,  # Materials/terrain/grass/grass_pasture_a
    "meadow": 8,  # grass_pasture_b
    "pasture": 29,  # kh_pasture_a
    "trampled": 16,  # soil/soil_dry_trampled
    "soil": 14,  # soil/soil_a
    "mud": 6,  # soil/soil_wet_a
    "stony_mud": 34,  # soil/stony_mud
    "sand": 10,  # soil/sand_soil
    "gravel": 3,  # stones/gravel_a
    "forest": 4,  # forest/forest_mixed
    "forest_leaves": 12,  # forest/forest_beech_oak
    "cobblestones": 37,  # cobblestone/cobble_stones_c_4x4
    "paving": 22,  # cobblestone/cobble_stones_a_2x2
    "cobblestones_grass": 38,  # cobblestone/cobble_stones_c_grass_4x4
    "river_shore": 15,  # river/river_shore
}
# The ground's colour from afar (the terrain texture under the detail materials), by the material
# that covers the map at first.
FAR_COLOURS = {
    "grass": (76, 88, 52),
    "meadow": (80, 90, 54),
    "pasture": (82, 88, 56),
    "forest": (64, 62, 46),
    "forest_leaves": (70, 62, 44),
    "sand": (128, 116, 92),
}
FAR_COLOUR_STONE = (104, 100, 92)
TEXTURE_SECTOR_SIZE = 256
# terrain.dat's model tables (CTerrain::Load_T): a vegetation model's record, its brush
# material's index in it, a brush model's or material's record, and how much of klaster's file
# holds them (its tables take about 2 MB).
VEGETATION_RECORD = 360
MATERIAL_AT = 336
BRUSH_RECORD = 256
HEAD_BYTES = 8 * 1024 * 1024
# The navmesh (recast/rdnavmesh_1.*, Warhorse's Detour version 14):
# 16 m tiles of NAV_CELLS x NAV_CELLS walkable quads, the walk settings and quantize factor as
# klaster's tiles have them, the polygons' winding (NAV_CLOCKWISE) and Detour's tile sides.
NAV_HEAD = 60
NAV_MAGIC = 0x444E4156
NAV_VERSION = 14
NAV_TILE = 16
NAV_CELLS = 4
NAV_WALK = 1
NAV_BORDER = 0x8000
NAV_SIDE_PLUS_X = 0
NAV_SIDE_PLUS_Y = 2
NAV_SIDE_MINUS_X = 4
NAV_SIDE_MINUS_Y = 6
NAV_WALKABLE_HEIGHT = 1.76
NAV_WALKABLE_RADIUS = 0.32
NAV_WALKABLE_CLIMB = 0.32
NAV_FOURTH = 0.8
NAV_QUANT = 12.5
NAV_BOX_MARGIN = 0.5
NAV_CLOCKWISE = False
TEXTURE_LAYER_FORMAT = 0x16
GROUND_NORMAL = (128, 255, 128)
# A height word: its level (0 to 4095) above bit 20, the surface below; a flat sector's step.
HEIGHT_SHIFT = 20
TOP_LEVEL = 4095
FLAT_STEP = 0.05


def read_entry(source, item, most=-1):
    """An entry's bytes (its first `most` only, when given): the game's paks name files with
    either slash in their local headers."""
    for header_name in (item.filename.replace("/", "\\"), item.filename.replace("\\", "/")):
        item.orig_filename = header_name
        try:
            with source.open(item) as entry:
                return entry.read(most)
        except zipfile.BadZipFile:
            continue
    raise zipfile.BadZipFile(item.filename)


def level_description(source, level):
    """klaster's level, level data, action data and mission files, renamed and emptied."""
    files = {}
    info = source.read("levelinfo.xml").decode("utf-8")
    files["levelinfo.xml"] = info.replace(
        'Name="data/levels/klaster"', f'Name="data/levels/{level}"'
    )
    data = source.read("leveldata.xml").decode("utf-8")
    data = data.replace('Name="klaster"', f'Name="{level}"')
    data = re.sub(r"<Layers>.*?</Layers>", "<Layers />", data, flags=re.S)
    # The editor's plant list: the game takes the plant kinds from terrain.dat (plant_kinds).
    data = re.sub(r"<Vegetation>.*?</Vegetation>", "<Vegetation />", data, flags=re.S)
    files["leveldata.xml"] = data
    action = source.read("leveldataaction.xml").decode("utf-8")
    action = re.sub(r'CGFCount="\d+"', 'CGFCount="0"', action)
    files["leveldataaction.xml"] = re.sub(r'ProgressBarRange="\d+"', 'ProgressBarRange="1"', action)
    mission = source.read("mission_mission0.xml").decode("utf-8")
    files["mission_mission0.xml"] = re.sub(r'CGFCount="\d+"', 'CGFCount="0"', mission)
    files["objects_mission0.xml"] = "<Objects>\n</Objects>\n"
    for name in (
        "levelprototypes.xml",
        "serialize.xml",
        "lensflarelist.xml",
        "dyntexsrclayeract.xml",
        "shaderslist.txt",
    ):
        files[name] = source.read(name)
    files["brushlist.txt"] = ""
    for item in source.infolist():
        name = item.filename.replace("\\", "/")
        if name.startswith("tables/"):
            files[name] = read_entry(source, item)
    return files


def leaf_samples(ground, x, y, height):
    """A sector's height words (x-major: sample (i, j) at j + side * i, as the game keeps them,
    Client/runtime/terrain/terrain_edits.cpp), its base, step, lowest and highest height and its
    levels of detail's errors. Flat without `ground`."""
    side = SECTOR_SIZE + 1
    if ground is None:
        return bytes(4 * side * side), height, FLAT_STEP, height, height, [0.0] * LOD_LEVELS
    import numpy

    block = ground[x : x + side, y : y + side]
    low = float(block.min())
    high = float(block.max())
    step = max((high - low) / TOP_LEVEL, FLAT_STEP)
    levels = numpy.clip(numpy.rint((block - low) / step), 0, TOP_LEVEL).astype(numpy.uint32)
    words = (levels << HEIGHT_SHIFT).astype("<u4").tobytes()
    return words, low, step, low, high, detail_errors(block)


def detail_errors(block):
    """How far each coarser level of detail (every 2nd, 4th, ... sample) strays from the full
    heights: the largest gap between a sample and the coarse grid's bilinear value there."""
    import numpy

    errors = [0.0]
    side = block.shape[0]
    full = numpy.arange(side)
    for level in range(1, LOD_LEVELS):
        stride = 1 << level
        kept = numpy.arange(0, side, stride)
        coarse = block[numpy.ix_(kept, kept)]
        # Bilinear along x, then y, from the kept rows and columns.
        along_x = numpy.array([numpy.interp(full, kept, coarse[:, j]) for j in range(len(kept))])
        smooth = numpy.array([numpy.interp(full, kept, along_x[:, i]) for i in range(side)])
        errors.append(float(numpy.abs(smooth - block).max()))
    return errors


def palette(first):
    """The sectors' surface palette, `first` the material the map starts covered with (the
    samples' local surface 0 is the palette's first byte), padded to whole words."""
    ids = [GROUND_MATERIALS[first]] + [
        surface for name, surface in GROUND_MATERIALS.items() if name != first
    ]
    data = bytes(ids)
    return data + bytes(-len(data) % 4), len(ids)


PALETTE, PALETTE_LENGTH = palette("grass")


def terrain_node(x, y, size, height, ground=None):
    """One node and its children (CTerrainNode load): flat at `height`, or the heights
    of `ground` (metres, [x][y], one a metre)."""
    leaf = size == SECTOR_SIZE
    children = b""
    if leaf:
        words, base, step, low, high, errors = leaf_samples(ground, x, y, height)
        side = SECTOR_SIZE + 1
    else:
        words, base, step, side, errors = b"", 0.0, 0.0, 0, [0.0] * LOD_LEVELS
        low, high = None, None
        half = size // 2
        for child_x, child_y in ((x, y), (x + half, y), (x, y + half), (x + half, y + half)):
            child, child_low, child_high = terrain_node(child_x, child_y, half, height, ground)
            children += child
            low = child_low if low is None else min(low, child_low)
            high = child_high if high is None else max(high, child_high)
    box = (x, y, low, x + size, y + size, high)
    out = struct.pack("<hBx6fffhxxi", 8, 0, *box, base, step, side, PALETTE_LENGTH)
    out += words
    out += struct.pack("<%df" % LOD_LEVELS, *errors)
    out += PALETTE
    return out + children, low, high


def relief(path, size, height, rise):
    """The ground from a grey picture: (size + 1) x (size + 1) heights, [x][y], from `height`
    (black) to `height + rise` (white); the picture's top is the map's north (y up)."""
    try:
        import numpy
        from PIL import Image
    except ImportError:
        sys.exit("--heightmap needs Python's numpy and Pillow (pip install numpy pillow)")
    picture = Image.open(path)
    sixteen = picture.mode in ("I;16", "I;16B", "I;16L", "I")
    picture = picture.convert("I" if sixteen else "L").resize(
        (size + 1, size + 1), Image.Resampling.BICUBIC
    )
    values = numpy.asarray(picture, dtype=numpy.float64)
    top = 65535.0 if sixteen else 255.0
    # Picture rows go down from the top (north); the map's y goes up: rows reversed, then [x][y].
    heights = height + numpy.clip(values / top, 0.0, 1.0) * rise
    return numpy.ascontiguousarray(heights[::-1, :].T)


def plant_kinds(game):
    """klaster's plant kinds, as terrain.dat's model tables: the head of klaster's terrain.dat (in
    its terrain.pak) as CTerrain::Load_T reads it is the count and 360-byte records
    of the vegetation models (each the model's path first, its brush material's index at + 336),
    the count and 256-byte records of the brush models, the count and 256-byte records of the
    brush materials. The kinds are kept with the materials they use (the first few) and no brush
    model; the game makes them the level's vegetation groups, which Foliage plants."""
    pak = zipfile.ZipFile(os.path.join(game, "Data", "Levels", "klaster", "terrain.pak"))
    item = next(
        entry
        for entry in pak.infolist()
        if entry.filename.replace("\\", "/").lower() == "terrain/terrain.dat"
    )
    head = read_entry(pak, item, HEAD_BYTES)
    at = 32
    (kinds,) = struct.unpack_from("<i", head, at)
    at += 4
    records = head[at : at + VEGETATION_RECORD * kinds]
    at += VEGETATION_RECORD * kinds
    (brushes,) = struct.unpack_from("<i", head, at)
    at += 4 + BRUSH_RECORD * brushes
    (materials,) = struct.unpack_from("<i", head, at)
    at += 4
    used = 1 + max(
        struct.unpack_from("<i", records, VEGETATION_RECORD * kind + MATERIAL_AT)[0]
        for kind in range(kinds)
    )
    used = max(0, min(used, materials))
    out = struct.pack("<i", kinds) + records
    out += struct.pack("<i", 0)
    out += struct.pack("<i", used) + head[at : at + BRUSH_RECORD * used]
    return out


def terrain(size, height, kinds, ground=None):
    """terrain\\terrain.dat (CTerrain::Load_T); `kinds` its model tables, `ground` the
    heights (flat at `height` without)."""
    body = kinds
    body += terrain_node(0, 0, size, height, ground)[0]
    edge = float(size)
    body += struct.pack("<hh6fi", 8, 0, 0.0, 0.0, 0.0, edge, edge, edge, 0)
    header = struct.pack(
        "<BBBBiifiiff",
        29,
        0,
        6,
        1,
        32 + len(body),
        size,
        1.0,
        SECTOR_SIZE,
        size // SECTOR_SIZE,
        0.003814697265625,
        0.0,
    )
    return header + body


def node_indices(size, counter):
    """The ground texture's index of every node in the node order; a leaf's four missing
    children are -1 each."""
    index = counter[0]
    counter[0] += 1
    if size == SECTOR_SIZE:
        return [index, -1, -1, -1, -1]
    out = [index]
    for _ in range(4):
        out += node_indices(size // 2, counter)
    return out


def bc1_solid(colour, size):
    """A BC1 picture of one colour: every 4 x 4 block both end colours the same, indices 0."""
    red, green, blue = colour
    packed = (red * 31 // 255) << 11 | (green * 63 // 255) << 5 | (blue * 31 // 255)
    return struct.pack("<HHI", packed, packed, 0) * ((size // 4) * (size // 4))


def navmesh_config(game):
    """The 60-byte head of klaster's main navmesh (recast/rdnavmesh_1.nav in its recast.pak): the
    navmesh's settings, the tile count last (+56). Copied, our count put in."""
    pak = zipfile.ZipFile(os.path.join(game, "Data", "Levels", "klaster", "recast.pak"))
    item = next(
        entry
        for entry in pak.infolist()
        if entry.filename.replace("\\", "/").lower() == "recast/rdnavmesh_1.nav"
    )
    return read_entry(pak, item, NAV_HEAD)[: NAV_HEAD - 4]


def nav_tile(tile_x, tile_y, heights):
    """One 16 m navmesh tile, Warhorse's Detour version 14 as the game reads it: a
    112-byte header, the vertices as whole float x y z (no packed ones), then 28-byte polygons:
    a grid of NAV_CELLS x NAV_CELLS quads at the ground's heights, each with its 4 neighbours
    (the next quad's index + 1, or on the tile's edge 0x8000 | its side)."""
    cells = NAV_CELLS
    step = NAV_TILE / cells
    left = tile_x * NAV_TILE
    bottom = tile_y * NAV_TILE
    vertices = []
    for j in range(cells + 1):
        for i in range(cells + 1):
            vertices.append(
                (left + i * step, bottom + j * step, heights(left + i * step, bottom + j * step))
            )
    polys = b""
    for j in range(cells):
        for i in range(cells):
            corner = j * (cells + 1) + i
            # Counter-clockwise seen from above (x right, y up); edge k runs from corner k to k + 1:
            # bottom (-y), right (+x), top (+y), left (-x).
            ring = (corner, corner + 1, corner + 1 + cells + 1, corner + cells + 1)
            neighbours = (
                (j - 1) * cells + i + 1 if j > 0 else NAV_BORDER | NAV_SIDE_MINUS_Y,
                j * cells + i + 2 if i < cells - 1 else NAV_BORDER | NAV_SIDE_PLUS_X,
                (j + 1) * cells + i + 1 if j < cells - 1 else NAV_BORDER | NAV_SIDE_PLUS_Y,
                j * cells + i if i > 0 else NAV_BORDER | NAV_SIDE_MINUS_X,
            )
            if NAV_CLOCKWISE:
                # The same quad the other way round: edges left, top, right, bottom.
                ring = (ring[0], ring[3], ring[2], ring[1])
                neighbours = (neighbours[3], neighbours[2], neighbours[1], neighbours[0])
            polys += struct.pack("<6H6H4B", *ring, 0, 0, *neighbours, 0, 0, NAV_WALK, 4, 0, 1)
    poly_count = cells * cells
    low = min(vertex[2] for vertex in vertices) - NAV_BOX_MARGIN
    high = max(vertex[2] for vertex in vertices) + NAV_BOX_MARGIN
    header = struct.pack(
        "<17i4f3f3ff",
        NAV_MAGIC,
        NAV_VERSION,
        tile_x,
        tile_y,
        0,
        0,
        poly_count,
        len(vertices),
        0,
        poly_count * 4 * 2,
        poly_count,
        0,
        0,
        0,
        0,
        0,
        poly_count,
        NAV_WALKABLE_HEIGHT,
        NAV_WALKABLE_RADIUS,
        NAV_WALKABLE_CLIMB,
        NAV_FOURTH,
        left,
        bottom,
        low,
        left + NAV_TILE,
        bottom + NAV_TILE,
        high,
        NAV_QUANT,
    )
    body = b"".join(struct.pack("<3f", *vertex) for vertex in vertices)
    return header + body + polys


def navmesh(size, height, ground, game):
    """recast/rdnavmesh_1.hdr and .nav for the whole map: one tile per 16 m square, its records
    (tile x, tile y, offset into the .nav, 1, the tile's size) in the .hdr."""

    def heights(x, y):
        if ground is None:
            return height
        i = min(max(int(round(x)), 0), size)
        j = min(max(int(round(y)), 0), size)
        return float(ground[i, j])

    tiles = size // NAV_TILE
    config = navmesh_config(game)
    nav = config + struct.pack("<i", tiles * tiles)
    index = b""
    for tile_y in range(tiles):
        for tile_x in range(tiles):
            tile = nav_tile(tile_x, tile_y, heights)
            index += struct.pack("<5i", tile_x, tile_y, len(nav), 1, len(tile))
            nav += tile
    return index, nav


def ground_texture(size, colour):
    """terrain\\cover.ctc: "CRY", version 100, file version 9, two layers, the node indices, then
    a sector of both layers for every node."""
    layer = struct.pack(
        "<HHB3xi",
        TEXTURE_SECTOR_SIZE,
        0,
        TEXTURE_LAYER_FORMAT,
        TEXTURE_SECTOR_SIZE * TEXTURE_SECTOR_SIZE // 2,
    )
    out = b"CRY\0" + struct.pack("<BBH", 100, 0, 9)
    out += struct.pack("<HHf", 2, 0, 1.0)
    out += layer * 2
    counter = [0]
    indices = node_indices(size, counter)
    out += struct.pack("<H%dh" % len(indices), len(indices), *indices)
    sector = bc1_solid(colour, TEXTURE_SECTOR_SIZE)
    sector += bc1_solid(GROUND_NORMAL, TEXTURE_SECTOR_SIZE)
    return out + sector * counter[0]


def main():
    parser = argparse.ArgumentParser(description="A World Architect custom map from scratch.")
    parser.add_argument("name", help="letters, digits and _ (the map is kcds_map_<name>)")
    parser.add_argument("--size", type=int, default=1024, help="metres a side: 128 to 2048")
    parser.add_argument("--height", type=float, default=32.0, help="the ground's height, metres")
    parser.add_argument(
        "--heightmap", default=None, help="a grey picture of the ground (black low, white high)"
    )
    parser.add_argument(
        "--rise", type=float, default=100.0, help="metres from black to white (with --heightmap)"
    )
    parser.add_argument(
        "--no-navmesh",
        dest="navmesh",
        action="store_false",
        help="no navmesh (people then cannot walk on the map)",
    )
    parser.add_argument(
        "--ground",
        default="grass",
        choices=sorted(GROUND_MATERIALS),
        help="the material the map starts covered with",
    )
    parser.add_argument("--game", default=DEFAULT_GAME)
    parser.add_argument("--out", default=None, help="the maps folder (default: the KCDS one)")
    args = parser.parse_args()
    if not NAME.match(args.name):
        sys.exit("the name is 1 to 32 lower-case letters, digits or _")
    if args.size not in (128, 256, 512, 1024, 2048):
        sys.exit("the size is 128, 256, 512, 1024 or 2048 metres")
    level = f"kcds_map_{args.name}"
    maps = args.out or os.path.join(os.environ["LOCALAPPDATA"], "KCDS", "maps")
    source = zipfile.ZipFile(os.path.join(args.game, "Data", "Levels", "klaster", "level.pak"))
    files = level_description(source, level)
    global PALETTE, PALETTE_LENGTH
    PALETTE, PALETTE_LENGTH = palette(args.ground)
    files["terrain/cover.ctc"] = ground_texture(
        args.size, FAR_COLOURS.get(args.ground, FAR_COLOUR_STONE)
    )
    ground = None
    spawn = args.height
    if args.heightmap:
        ground = relief(args.heightmap, args.size, args.height, args.rise)
        middle = args.size // 2
        spawn = float(ground[middle, middle])
    files["terrain/terrain.dat"] = terrain(args.size, args.height, plant_kinds(args.game), ground)
    files["terrain/indoor.dat"] = struct.pack("<5i", 6, 20, 0, 0, 0)
    files["terrain/hlods.xml"] = (
        '<HLod DataOffset="4" DataSize="4" ProxyIndex="-1" Type="Cluster" Radius="8000" '
        'NearestObserverDistance="0" Name="root" Hash="214082742" />\n'
    )
    files["terrain/hlods.dat"] = struct.pack("<2i", 2, 0)
    navigation = None
    if args.navmesh:
        # recast.pak beside level.pak, which the game client opens where the game looks for a
        # custom map's navmesh (Client/runtime/maps/custom_maps.cpp): people walk on the ground.
        navigation = navmesh(args.size, args.height, ground, args.game)
    folder = os.path.join(maps, level)
    os.makedirs(folder, exist_ok=True)
    # A running game keeps the packs of a map it opened locked (its recast.pak): nothing is written
    # then, or the map would be half new, half old (level.pak rewritten, then
    # PermissionError on recast.pak).
    for existing in ("level.pak", "recast.pak", "map.json"):
        target = os.path.join(folder, existing)
        if os.path.exists(target):
            try:
                with open(target, "r+b"):
                    pass
            except PermissionError:
                sys.exit(
                    f"{level}: {existing} is in use (a running game opened this map): close the "
                    "game or choose another name"
                )
    # Deflated (CryPak reads zip's deflate): a flat map is mostly zeros, about 1 MB instead of 50,
    # small enough for a server's resource to send to its players (8 MiB a file).
    pack = os.path.join(folder, "level.pak")
    with zipfile.ZipFile(pack, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as pak:
        for name, content in files.items():
            pak.writestr(name, content)
    navigation_pack = os.path.join(folder, "recast.pak")
    if navigation:
        with zipfile.ZipFile(navigation_pack, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as pak:
            pak.writestr("Recast/RDNavmesh_1.hdr", navigation[0])
            pak.writestr("Recast/RDNavmesh_1.nav", navigation[1])
    elif os.path.exists(navigation_pack):
        os.remove(navigation_pack)
    with open(os.path.join(folder, "map.json"), "w", encoding="utf-8") as about:
        about.write(
            '{"name": "%s", "size": %d, "height": %g, "ground": "%s"}\n'
            % (level, args.size, spawn, args.ground)
        )
    print(
        f"{level}: {args.size} m at {args.height} m, {len(files)} files in {folder}"
        f" ({os.path.getsize(pack)} bytes)"
    )


main()
