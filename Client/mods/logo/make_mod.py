#!/usr/bin/env python3
"""Build the KCDS branding pak (our main-menu logo and our deck of playing cards).

The pak holds `Libs/UI/Textures/KCDLogo.dds`, our "Synced" variant of KCD2's main-menu logo, and
`objects/kcds_cards/` (the playing cards scripts deal with CardDeck, drawn by
Dev/tools/assets/cards/make_cards.py). It
ships in the Client release next to the game-client DLL as `assets/kcds-branding.pak`; the DLL
mounts it at runtime through the engine's CryPak, only in a game started by our launcher. Nothing
is ever written into the game installation.

The logo asset is DXT5 (BC3), 1024x512, no mipmaps. The game's original 128-byte DDS header is
kept byte for byte (so CryEngine's loader sees the exact flags, linear size and NVTT signature it
wrote) and only the compressed pixels are replaced. The header comes from the installed game, so
the pak is built when a release is packaged and is never committed. A build without the game (the
Linux release build) passes those 128 header bytes as a file instead (--logo-header),
which CI writes from a protected variable.

Usage:
    python make_mod.py --game "C:/Program Files (x86)/Steam/steamapps/common/KingdomComeDeliverance2"
        [--out <path to kcds-branding.pak>]
    python make_mod.py --logo-header <file with the 128 header bytes> [--out ...]

Requires Pillow (DXT5 DDS encoding).
"""

import argparse
import io
from pathlib import Path
import sys
import zipfile

HERE = Path(__file__).resolve().parent
SOURCE_PNG = HERE / "synced-logo.png"
# An already extracted original logo next to this script is used instead of the game pak.
EXTRACTED_ORIGINAL = HERE / "KCDLogo.orig.dds"
# Path inside the game's data root, and inside our pak.
LOGO_PATH = "Libs/UI/Textures/KCDLogo.dds"
GAME_DATA_PAK = Path("Data") / "IPL_GameData.pak"
PAK_NAME = "kcds-branding.pak"
# The deck of playing cards in the same pak, and the tool that draws it.
CARDS_FOLDER = "objects/kcds_cards"
CARDS_TOOL = HERE.parents[2] / "Dev" / "tools" / "assets" / "cards"
DEFAULT_OUT = HERE / "build" / "assets" / PAK_NAME

DDS_HEADER_SIZE = 128
LOGO_SIZE = (1024, 512)
# DXT5 stores a 4x4 block in 16 bytes: one byte a pixel, after the header.
LOGO_DDS_SIZE = DDS_HEADER_SIZE + LOGO_SIZE[0] * LOGO_SIZE[1]


class BrandingError(Exception):
    pass


def read_original_logo(game_root):
    if EXTRACTED_ORIGINAL.is_file():
        return EXTRACTED_ORIGINAL.read_bytes()
    game_pak = Path(game_root) / GAME_DATA_PAK
    if not game_pak.is_file():
        raise BrandingError(f"game pak not found: {game_pak}")
    with zipfile.ZipFile(game_pak) as archive:
        return archive.read(LOGO_PATH)


def encode_logo():
    from PIL import Image

    image = Image.open(SOURCE_PNG).convert("RGBA")
    if image.size != LOGO_SIZE:
        image = image.resize(LOGO_SIZE, Image.LANCZOS)
    encoded = io.BytesIO()
    image.save(encoded, format="DDS", pixel_format="DXT5")
    return encoded.getvalue()


def original_header(game_root, header_file):
    """The game logo's 128-byte DDS header, from the game or from a file that holds only it."""
    if header_file is not None:
        header = Path(header_file).read_bytes()
        if len(header) != DDS_HEADER_SIZE or not header.startswith(b"DDS "):
            raise BrandingError(
                f"{header_file} is not the logo's {DDS_HEADER_SIZE}-byte DDS header"
            )
        return header
    original = read_original_logo(game_root)
    # DXT5 1024x512 without mipmaps has a fixed size; a mismatch means the format drifted.
    if len(original) != LOGO_DDS_SIZE:
        raise BrandingError(
            f"original KCDLogo.dds has {len(original)} bytes, not {LOGO_DDS_SIZE}; "
            "check the dimensions and mipmaps"
        )
    return original[:DDS_HEADER_SIZE]


def build_logo_dds(game_root, header_file=None):
    header = original_header(game_root, header_file)
    ours = encode_logo()
    if len(ours) != LOGO_DDS_SIZE:
        raise BrandingError(
            f"encoded size {len(ours)} differs from {LOGO_DDS_SIZE}; "
            "check the dimensions and mipmaps"
        )
    return header + ours[DDS_HEADER_SIZE:]


def card_files():
    """KCDS's own deck of playing cards (CardDeck), at its game path
    objects/kcds_cards/, drawn by Dev/tools/assets/cards/make_cards.py."""
    sys.path.insert(0, str(CARDS_TOOL))
    import make_cards

    for name, data in make_cards.deck_files(CARDS_FOLDER):
        yield f"{CARDS_FOLDER}/{name}", data


def build_pak(game_root, header_file=None):
    """Return the bytes of kcds-branding.pak: the logo and the deck of cards."""
    dds = build_logo_dds(game_root, header_file)
    pak = io.BytesIO()
    # A fixed entry time keeps the pak byte-identical for the same logo, so the launcher's
    # SHA-256 update plan does not download it again when nothing changed.
    with zipfile.ZipFile(pak, "w", zipfile.ZIP_DEFLATED) as archive:
        for path, data in [(LOGO_PATH, dds), *card_files()]:
            entry = zipfile.ZipInfo(path, date_time=(1980, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(entry, data)
    return pak.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--game", help="KCD2 install root (read only)")
    source.add_argument(
        "--logo-header", type=Path, help="file with the game logo's 128-byte DDS header"
    )
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT, help="pak file to write")
    options = parser.parse_args()

    out = options.out.resolve()
    if options.game:
        game = Path(options.game).resolve()
        if game == out or game in out.parents:
            print(
                "make_mod: the pak must not be written into the game installation",
                file=sys.stderr,
            )
            sys.exit(1)
    try:
        pak = build_pak(options.game, options.logo_header)
    except BrandingError as error:
        print(f"make_mod: {error}", file=sys.stderr)
        sys.exit(1)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(pak)
    print(f"built {out} ({len(pak)} bytes)")


if __name__ == "__main__":
    main()
