"""The Asset/Animation Studio's own empty level, kcds_studio.

Built from the player's own game files: klaster's level description with every layer, building,
vegetation and entity left out. The game's level load (C3DEngineLevelLoadTimeslicer::DoStep,
) cannot do without a heightmap (step 10, terrain\\terrain.dat) and vis
areas (step 11, terrain\\indoor.dat): the studio writes its own, a flat 128 m floor and none.

Usage: python make_studio_level.py <output folder kcds_studio> [game folder]
Only World Architect's local mode with KCDS_ASSET_STUDIO=1 loads it (never a server).
"""

import os
import re
import struct
import sys
import zipfile

DEFAULT_GAME = r"C:/Program Files (x86)/Steam/steamapps/common/KingdomComeDeliverance2"


def read_entry(source, item):
    """An entry's bytes: the game's paks name files with either slash in their local headers."""
    for header_name in (item.filename.replace("/", "\\"), item.filename.replace("\\", "/")):
        item.orig_filename = header_name
        try:
            return source.open(item).read()
        except zipfile.BadZipFile:
            continue
    raise zipfile.BadZipFile(item.filename)


def level_description(source):
    """The level, level data, action data and mission files, renamed and emptied."""
    files = {}
    info = source.read("levelinfo.xml").decode("utf-8")
    info = info.replace('Name="data/levels/klaster"', 'Name="data/levels/kcds_studio"')
    files["levelinfo.xml"] = info

    data = source.read("leveldata.xml").decode("utf-8")
    data = data.replace('Name="klaster"', 'Name="kcds_studio"')
    data = re.sub(r"<Layers>.*?</Layers>", "<Layers />", data, flags=re.S)
    data = re.sub(r"<Vegetation>.*?</Vegetation>", "<Vegetation />", data, flags=re.S)
    files["leveldata.xml"] = data

    action = source.read("leveldataaction.xml").decode("utf-8")
    action = re.sub(r'CGFCount="\d+"', 'CGFCount="0"', action)
    files["leveldataaction.xml"] = action.replace('ProgressBarRange="3789"', 'ProgressBarRange="1"')

    mission = source.read("mission_mission0.xml").decode("utf-8")
    mission = re.sub(r'CGFCount="\d+"', 'CGFCount="0"', mission)
    files["mission_mission0.xml"] = mission
    files["objects_mission0.xml"] = "<Objects>\n</Objects>\n"

    for name in (
        "levelprototypes.xml",
        "serialize.xml",
        "lensflarelist.xml",
        "dyntexsrclayeract.xml",
        "shaderslist.txt",
    ):
        files[name] = source.read(name)
    # Step 7 needs a brush list; an empty one is fine.
    files["brushlist.txt"] = ""
    return files


# The studio's own ground: a flat square of TERRAIN_SIZE metres at FLOOR_HEIGHT, in sectors of
# SECTOR_SIZE metres (one root node and four leaves).
TERRAIN_SIZE = 128
SECTOR_SIZE = 64
FLOOR_HEIGHT = 32.0
BOX_MARGIN = 0.5
# The floor's look, footsteps and physics: one of the level's surface types (leveldata.xml
# SurfaceTypes), 37 Materials/terrain/cobblestone/cobble_stones_c_4x4.
FLOOR_SURFACE = 37
# The terrain's levels of detail: the sector halved down to one unit (1 m), 64 -> 1 is six.
LOD_LEVELS = 6


def terrain_node(x, y, size):
    """One terrain node (CTerrainNode load) and its children, flat at FLOOR_HEIGHT.

    The 44-byte chunk: version 8, holes, the heightmap box, the base height and the height step,
    the heightmap side and the surface palette length. A leaf (one sector) carries its
    (side x side) heightmap words, height = base + (word >> 20) * step and the local surface index
    in the low bits: all 0 is flat ground of the palette's first surface. Then the levels of
    detail's errors and the palette, each padded to 4 bytes.
    """
    leaf = size == SECTOR_SIZE
    side = SECTOR_SIZE + 1 if leaf else 0
    # A box with no height at all was culled now and then (the floor not drawn in some starts);
    # klaster's nodes are metres high.
    box = (x, y, FLOOR_HEIGHT - BOX_MARGIN, x + size, y + size, FLOOR_HEIGHT + BOX_MARGIN)
    base, step = (FLOOR_HEIGHT, 0.05) if leaf else (0.0, 0.0)
    out = struct.pack("<hBx6fffhxxi", 8, 0, *box, base, step, side, 1)
    out += bytes(4 * side * side)
    out += struct.pack("<%df" % LOD_LEVELS, *([0.0] * LOD_LEVELS))
    out += bytes([FLOOR_SURFACE, 0, 0, 0])
    if not leaf:
        half = size // 2
        for child_x, child_y in ((x, y), (x + half, y), (x, y + half), (x + half, y + half)):
            out += terrain_node(child_x, child_y, half)
    return out


def studio_terrain():
    """terrain\\terrain.dat (CTerrain::Load_T): chunk version 29, no vegetation,
    brushes or brush materials, the flat node tree and an empty object octree (one 32-byte node,
    version 8, no children, no instances)."""
    body = struct.pack("<3i", 0, 0, 0)
    body += terrain_node(0, 0, TERRAIN_SIZE)
    size = float(TERRAIN_SIZE)
    body += struct.pack("<hh6fi", 8, 0, 0.0, 0.0, 0.0, size, size, size, 0)
    header = struct.pack(
        "<BBBBiifiiff",
        29,
        0,
        6,
        1,
        32 + len(body),
        TERRAIN_SIZE,
        1.0,
        SECTOR_SIZE,
        TERRAIN_SIZE // SECTOR_SIZE,
        0.003814697265625,
        0.0,
    )
    return header + body


# The ground texture's sectors: 256 x 256 pixels in two BC1 layers (colour, then normal), as
# klaster's cover.ctc; the floor's colour and its normal straight up, in RGB.
TEXTURE_SECTOR_SIZE = 256
TEXTURE_LAYER_FORMAT = 0x16
FLOOR_COLOUR = (72, 72, 72)
FLOOR_NORMAL = (128, 255, 128)


def bc1_solid(colour, size):
    """A BC1 picture of one colour: every 4 x 4 block both end colours the same, indices 0."""
    red, green, blue = colour
    packed = (red * 31 // 255) << 11 | (green * 63 // 255) << 5 | (blue * 31 // 255)
    block = struct.pack("<HHI", packed, packed, 0)
    return block * ((size // 4) * (size // 4))


def studio_ground_texture():
    """terrain\\cover.ctc (opened): "CRY", version 100, file version 9; two
    layers; the texture index of every terrain node in the node order, a negative entry skipping
    a child, so each leaf is followed by four -1; then each index's sector, the
    layers one after the other."""
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
    leaves = (TERRAIN_SIZE // SECTOR_SIZE) ** 2
    indices = [0]
    for leaf in range(leaves):
        indices += [1 + leaf, -1, -1, -1, -1]
    out += struct.pack("<H%dh" % len(indices), len(indices), *indices)
    sector = bc1_solid(FLOOR_COLOUR, TEXTURE_SECTOR_SIZE)
    sector += bc1_solid(FLOOR_NORMAL, TEXTURE_SECTOR_SIZE)
    return out + sector * (1 + leaves)


def studio_vis_areas():
    """terrain\\indoor.dat: no vis areas (version 6, the 20-byte chunk, no areas or portals)."""
    return struct.pack("<5i", 6, 20, 0, 0, 0)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    out = sys.argv[1]
    game = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_GAME
    klaster = os.path.join(game, "Data", "Levels", "klaster")

    source = zipfile.ZipFile(os.path.join(klaster, "level.pak"))
    files = level_description(source)
    for item in source.infolist():
        name = item.filename.replace("\\", "/")
        if name.startswith("tables/"):
            files[name] = read_entry(source, item)
    # The studio's own ground texture: without one the terrain is not drawn at all.
    files["terrain/cover.ctc"] = studio_ground_texture()
    files["terrain/terrain.dat"] = studio_terrain()
    files["terrain/indoor.dat"] = studio_vis_areas()
    # Distant levels of detail (step 9): only the root cluster, no layers. hlods.dat is version 2
    # (checked on open) and the root's 4 bytes of data, read by offset when streamed.
    files["terrain/hlods.xml"] = (
        '<HLod DataOffset="4" DataSize="4" ProxyIndex="-1" Type="Cluster" Radius="8000" '
        'NearestObserverDistance="0" Name="root" Hash="214082742" />\n'
    )
    files["terrain/hlods.dat"] = struct.pack("<2i", 2, 0)

    os.makedirs(out, exist_ok=True)
    with zipfile.ZipFile(os.path.join(out, "level.pak"), "w", zipfile.ZIP_STORED) as pak:
        for name, content in files.items():
            pak.writestr(name, content)
    print("kcds_studio level.pak:", len(files), "files")


if __name__ == "__main__":
    main()
