"""A deck of playing cards as the game's own models.

Writes into <resource>/stream/objects/kcds/<resource name>/ (the only folder the client takes a
resource's models from):

  <id>.cgf / .mtl   one card: its face on top (+z), the shared back below, a paper edge
  back.cgf / .mtl   the same card with the back on both sides, for cards nobody may see
  <id>.dds          the face, 256 x 512 BC1 with mips (drawn at 10 pixels a millimetre)
  back.dds          the back

(the names a TabletopDeck's models folder holds).

Card ids: rank A 2 3 4 5 6 7 8 9 T J Q K and suit S H D C (AS, TH, QD, ...), and the jokers RJ
(red) and BJ (black); the file names are lower case (as.cgf). The card is 64 x 89 mm with
3 mm round corners and lies on its back at the model's origin: x across, y along, z up.

The pictures are drawn with Pillow from plain shapes and the OFL fonts the launcher ships
(Cinzel, EB Garamond); nothing is copied from another deck.

    python Dev/tools/assets/cards/make_cards.py --resource <folder> [--only AS,back] [--png <folder>]
"""

import argparse
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "blender"))

import kcds_cgf  # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
FONTS = os.path.join(REPO, "Client", "runtime", "branding", "assets", "fonts")

WIDTH_MM = 64.0
HEIGHT_MM = 89.0
THICK_MM = 0.3
CORNER_MM = 3.0
CORNER_STEPS = 5
PIXELS_PER_MM = 10
TEXTURE_SIZE = (256, 512)

RANKS = "A23456789TJQK"
SUITS = "SHDC"
JOKERS = ("RJ", "BJ")
RANK_LABEL = {"T": "10"}

PAPER = (239, 229, 203)
PAPER_EDGE = (214, 200, 166)
INK = (40, 32, 26)
RED = (156, 30, 26)
BLACK = (30, 26, 22)
GOLD = (184, 146, 64)
BACK_FIELD = (112, 26, 30)
BACK_DARK = (70, 14, 18)


def card_ids():
    return [rank + suit for suit in SUITS for rank in RANKS] + list(JOKERS)


def suit_colour(suit):
    return RED if suit in "HD" else BLACK


def font(name, size):
    return ImageFont.truetype(os.path.join(FONTS, name), size)


# --- suits ---------------------------------------------------------------------------------------


def draw_suit(draw, suit, cx, cy, size, colour, flip=False):
    """A suit sign `size` pixels tall centred at (cx, cy); `flip` turns it upside down."""
    s = size / 2.0
    sign = -1 if flip else 1

    def point(x, y):
        return (cx + x * s, cy + sign * y * s)

    def blob(x, y, radius):
        px, py = point(x, y)
        r = radius * s
        draw.ellipse((px - r, py - r, px + r, py + r), fill=colour)

    if suit == "D":
        draw.polygon([point(0, -1), point(0.72, 0), point(0, 1), point(-0.72, 0)], fill=colour)
    elif suit == "H":
        blob(-0.46, -0.42, 0.5)
        blob(0.46, -0.42, 0.5)
        draw.polygon([point(-0.94, -0.28), point(0.94, -0.28), point(0, 0.98)], fill=colour)
    elif suit == "S":
        blob(-0.44, 0.2, 0.46)
        blob(0.44, 0.2, 0.46)
        draw.polygon([point(-0.88, 0.32), point(0.88, 0.32), point(0, -1)], fill=colour)
        draw.polygon([point(0, 0.3), point(0.36, 1), point(-0.36, 1)], fill=colour)
    elif suit == "C":
        blob(0, -0.5, 0.42)
        blob(-0.48, 0.14, 0.42)
        blob(0.48, 0.14, 0.42)
        blob(0, 0.06, 0.24)
        draw.polygon([point(0, 0.1), point(0.34, 1), point(-0.34, 1)], fill=colour)


# --- faces ---------------------------------------------------------------------------------------

# Pip places of the number cards: x in -1, 0, 1 (the columns), y from -1 (top) to 1 (bottom).
PIPS = {
    "2": [(0, -1), (0, 1)],
    "3": [(0, -1), (0, 0), (0, 1)],
    "4": [(-1, -1), (1, -1), (-1, 1), (1, 1)],
    "5": [(-1, -1), (1, -1), (0, 0), (-1, 1), (1, 1)],
    "6": [(-1, -1), (1, -1), (-1, 0), (1, 0), (-1, 1), (1, 1)],
    "7": [(-1, -1), (1, -1), (0, -0.5), (-1, 0), (1, 0), (-1, 1), (1, 1)],
    "8": [(-1, -1), (1, -1), (0, -0.5), (-1, 0), (1, 0), (0, 0.5), (-1, 1), (1, 1)],
    "9": [
        (-1, -1),
        (1, -1),
        (-1, -1 / 3),
        (1, -1 / 3),
        (0, 0),
        (-1, 1 / 3),
        (1, 1 / 3),
        (-1, 1),
        (1, 1),
    ],
    "T": [
        (-1, -1),
        (1, -1),
        (0, -2 / 3),
        (-1, -1 / 3),
        (1, -1 / 3),
        (-1, 1 / 3),
        (1, 1 / 3),
        (0, 2 / 3),
        (-1, 1),
        (1, 1),
    ],
}

FACE_TITLE = {"J": "KNAVE", "Q": "QUEEN", "K": "KING"}


def blank_face(width, height):
    picture = Image.new("RGB", (width, height), PAPER)
    draw = ImageDraw.Draw(picture)
    inset = 18
    draw.rounded_rectangle(
        (inset, inset, width - inset, height - inset), radius=18, outline=PAPER_EDGE, width=4
    )
    return picture, draw


def draw_index(picture, label, suit, colour):
    """The rank and a small suit in the top left corner and, turned round, in the bottom right."""
    width, height = picture.size
    corner = Image.new("RGBA", (110, 170), (0, 0, 0, 0))
    draw = ImageDraw.Draw(corner)
    letters = font("Cinzel-Bold.ttf", 62 if len(label) == 1 else 50)
    box = draw.textbbox((0, 0), label, font=letters)
    draw.text((55 - (box[0] + box[2]) / 2, 10 - box[1]), label, font=letters, fill=colour)
    if suit:
        draw_suit(draw, suit, 55, 112, 52, colour)
    picture.paste(corner, (30, 34), corner)
    turned = corner.rotate(180)
    picture.paste(turned, (width - 30 - 110, height - 34 - 170), turned)


def draw_number(picture, draw, rank, suit, colour):
    width, height = picture.size
    left, right = width * 0.32, width * 0.68
    top, bottom = height * 0.22, height * 0.78
    size = 92 if rank in "2345" else 84
    for x, y in PIPS[rank]:
        cx = (left + right) / 2 + x * (right - left) / 2
        cy = (top + bottom) / 2 + y * (bottom - top) / 2
        draw_suit(draw, suit, cx, cy, size, colour, flip=y > 0)


def draw_ace(picture, draw, suit, colour):
    width, height = picture.size
    cx, cy = width / 2, height / 2
    radius = 190
    draw.ellipse((cx - radius, cy - radius, cx + radius, cy + radius), outline=GOLD, width=6)
    draw.ellipse(
        (cx - radius + 14, cy - radius + 14, cx + radius - 14, cy + radius - 14),
        outline=PAPER_EDGE,
        width=3,
    )
    draw_suit(draw, suit, cx, cy, 250, colour)


def draw_crown(draw, cx, cy, size, colour):
    s = size / 2.0
    points = [
        (cx - s, cy + s * 0.5),
        (cx - s, cy - s * 0.4),
        (cx - s * 0.5, cy + s * 0.05),
        (cx, cy - s * 0.7),
        (cx + s * 0.5, cy + s * 0.05),
        (cx + s, cy - s * 0.4),
        (cx + s, cy + s * 0.5),
    ]
    draw.polygon(points, fill=colour)
    for x, y in ((cx - s, cy - s * 0.4), (cx, cy - s * 0.7), (cx + s, cy - s * 0.4)):
        draw.ellipse((x - s * 0.14, y - s * 0.14, x + s * 0.14, y + s * 0.14), fill=colour)


def draw_court(picture, draw, rank, suit, colour):
    """Knave, queen and king: a gold frame, the rank's letter large, a crown or a cap above, the
    suit below, the title on a band; the bottom half is the top half turned round."""
    width, height = picture.size
    half = Image.new("RGBA", (width, height // 2), (0, 0, 0, 0))
    hd = ImageDraw.Draw(half)
    frame = (150, 150, width - 150, height // 2 + 40)
    hd.rectangle(frame, outline=GOLD, width=6)
    hd.rectangle(
        (frame[0] + 12, frame[1] + 12, frame[2] - 12, frame[3] + 40), outline=colour, width=2
    )
    cx = width / 2
    if rank == "J":
        hd.ellipse((cx - 44, 180, cx + 44, 236), fill=colour)
        hd.rectangle((cx - 62, 222, cx + 62, 236), fill=colour)
    else:
        draw_crown(hd, cx, 214, 110 if rank == "K" else 90, GOLD)
    letters = font("Cinzel-Bold.ttf", 150)
    box = hd.textbbox((0, 0), rank, font=letters)
    hd.text((cx - (box[0] + box[2]) / 2, 250 - box[1]), rank, font=letters, fill=colour)
    draw_suit(hd, suit, cx, 438, 60, colour)
    title = font("EBGaramond-Regular.ttf", 30)
    box = hd.textbbox((0, 0), FACE_TITLE[rank], font=title)
    hd.text((cx - (box[0] + box[2]) / 2, 150 - 44 - box[1]), FACE_TITLE[rank], font=title, fill=INK)
    picture.paste(half, (0, 0), half)
    turned = half.rotate(180)
    picture.paste(turned, (0, height - height // 2), turned)
    draw.line((frame[0], height / 2, frame[2], height / 2), fill=GOLD, width=4)


def draw_joker(picture, draw, colour):
    width, height = picture.size
    cx, cy = width / 2, height / 2
    star = []
    for at in range(10):
        radius = 170 if at % 2 == 0 else 70
        angle = -math.pi / 2 + at * math.pi / 5
        star.append((cx + radius * math.cos(angle), cy + radius * math.sin(angle)))
    draw.polygon(star, fill=colour)
    draw.ellipse((cx - 40, cy - 40, cx + 40, cy + 40), fill=GOLD)
    letters = font("Cinzel-Bold.ttf", 44)
    for at, letter in enumerate("JOKER"):
        box = draw.textbbox((0, 0), letter, font=letters)
        x = 62 - (box[0] + box[2]) / 2
        y = 70 + at * 54 - box[1]
        draw.text((x, y), letter, font=letters, fill=colour)
        draw.text(
            (width - 62 - (box[0] + box[2]) / 2, height - 70 - 5 * 54 + at * 54 - box[1]),
            letter,
            font=letters,
            fill=colour,
        )


def face_picture(card):
    width = int(WIDTH_MM * PIXELS_PER_MM)
    height = int(HEIGHT_MM * PIXELS_PER_MM)
    picture, draw = blank_face(width, height)
    if card in JOKERS:
        draw_joker(picture, draw, RED if card == "RJ" else BLACK)
        return picture
    rank, suit = card[0], card[1]
    colour = suit_colour(suit)
    draw_index(picture, RANK_LABEL.get(rank, rank), suit, colour)
    if rank == "A":
        draw_ace(picture, draw, suit, colour)
    elif rank in "JQK":
        draw_court(picture, draw, rank, suit, colour)
    else:
        draw_number(picture, draw, rank, suit, colour)
    return picture


def back_picture():
    width = int(WIDTH_MM * PIXELS_PER_MM)
    height = int(HEIGHT_MM * PIXELS_PER_MM)
    picture = Image.new("RGB", (width, height), PAPER)
    draw = ImageDraw.Draw(picture)
    margin = 26
    draw.rectangle((margin, margin, width - margin, height - margin), fill=BACK_FIELD)
    step = 40
    for x in range(-height, width + height, step):
        draw.line((x, 0, x + height, height), fill=BACK_DARK, width=3)
        draw.line((x, height, x + height, 0), fill=BACK_DARK, width=3)
    draw.rectangle((0, 0, width, margin), fill=PAPER)
    draw.rectangle((0, height - margin, width, height), fill=PAPER)
    draw.rectangle((0, 0, margin, height), fill=PAPER)
    draw.rectangle((width - margin, 0, width, height), fill=PAPER)
    draw.rectangle((margin, margin, width - margin, height - margin), outline=GOLD, width=8)
    draw.rectangle(
        (margin + 18, margin + 18, width - margin - 18, height - margin - 18), outline=GOLD, width=3
    )
    cx, cy = width / 2, height / 2
    for radius, fill in ((120, GOLD), (104, BACK_FIELD), (84, GOLD), (70, BACK_DARK)):
        draw.ellipse((cx - radius, cy - radius, cx + radius, cy + radius), fill=fill)
    for at in range(8):
        angle = at * math.pi / 4
        x = cx + 46 * math.cos(angle)
        y = cy + 46 * math.sin(angle)
        draw.ellipse((x - 14, y - 14, x + 14, y + 14), fill=GOLD)
    draw.ellipse((cx - 18, cy - 18, cx + 18, cy + 18), fill=GOLD)
    return picture


def texture_bytes(picture):
    small = picture.resize(TEXTURE_SIZE, Image.LANCZOS).convert("RGBA")
    r, g, b, a = small.split()
    bgra = Image.merge("RGBA", (b, g, r, a)).tobytes()
    return kcds_cgf.write_dds_bc1(TEXTURE_SIZE[0], TEXTURE_SIZE[1], bgra, srgb=True)


# --- the card's mesh ------------------------------------------------------------------------------


def outline():
    """The card's edge, counter-clockwise from above, in metres: four quarter circles."""
    half_w = WIDTH_MM / 2000.0
    half_h = HEIGHT_MM / 2000.0
    radius = CORNER_MM / 1000.0
    corners = [
        (half_w - radius, half_h - radius, 0),
        (-half_w + radius, half_h - radius, 90),
        (-half_w + radius, -half_h + radius, 180),
        (half_w - radius, -half_h + radius, 270),
    ]
    points = []
    for cx, cy, start in corners:
        for step in range(CORNER_STEPS + 1):
            angle = math.radians(start + 90.0 * step / CORNER_STEPS)
            points.append((cx + radius * math.cos(angle), cy + radius * math.sin(angle)))
    return points


def card_mesh():
    """Three subsets: 0 the face (top), 1 the back (bottom), 2 the edge."""
    mesh = kcds_cgf.Mesh()
    half_w = WIDTH_MM / 2000.0
    half_h = HEIGHT_MM / 2000.0
    top = THICK_MM / 1000.0
    ring = outline()

    def face(z, normal_up, material):
        first_index = len(mesh.indices)
        first = len(mesh.positions)
        across = (1, 0, 0) if normal_up else (-1, 0, 0)
        up = (0, 1, 0)
        mesh.positions.append((0.0, 0.0, z))
        mesh.uvs.append((0.5, 0.5))
        mesh.tangents.append((across, up, 1))
        for x, y in ring:
            u = (x + half_w) / (2 * half_w)
            v = 1 - (y + half_h) / (2 * half_h)
            if not normal_up:
                u = 1 - u
            mesh.positions.append((x, y, z))
            mesh.uvs.append((u, v))
            mesh.tangents.append((across, up, 1))
        count = len(ring)
        for at in range(count):
            a = first + 1 + at
            b = first + 1 + (at + 1) % count
            if normal_up:
                mesh.indices += [first, a, b]
            else:
                mesh.indices += [first, b, a]
        mesh.subsets.append(
            kcds_cgf.Subset(
                first_index,
                len(mesh.indices) - first_index,
                first,
                len(mesh.positions) - first,
                material,
            )
        )

    face(top, True, 0)
    face(0.0, False, 1)

    first_index = len(mesh.indices)
    first = len(mesh.positions)
    count = len(ring)
    for at in range(count):
        x0, y0 = ring[at]
        x1, y1 = ring[(at + 1) % count]
        length = math.hypot(x1 - x0, y1 - y0) or 1.0
        across = ((x1 - x0) / length, (y1 - y0) / length, 0)
        up = (0, 0, 1)
        base = len(mesh.positions)
        for x, y, z, u, v in (
            (x0, y0, 0.0, 0, 1),
            (x1, y1, 0.0, 1, 1),
            (x1, y1, top, 1, 0),
            (x0, y0, top, 0, 0),
        ):
            mesh.positions.append((x, y, z))
            mesh.uvs.append((u, v))
            mesh.tangents.append((across, up, 1))
        mesh.indices += [base, base + 1, base + 2, base, base + 2, base + 3]
    mesh.subsets.append(
        kcds_cgf.Subset(
            first_index, len(mesh.indices) - first_index, first, len(mesh.positions) - first, 2
        )
    )
    mesh.materials += ["card_face", "card_back", "card_edge"]
    mesh.physicalize += [kcds_cgf.PHYSICALIZE_NONE] * 3
    return mesh


def card_materials(folder, face_texture):
    edge = tuple(part / 255.0 for part in PAPER_EDGE)
    return [
        {"name": "card_face", "texture": face_texture, "surface": "mat_cloth"},
        {"name": "card_back", "texture": f"{folder}/back.dds", "surface": "mat_cloth"},
        {"name": "card_edge", "colour": edge, "surface": "mat_cloth"},
    ]


def deck_files(folder, wanted=None, pictures_out=None):
    """The deck's files as (file name, bytes): per card <name>.cgf, .mtl and .dds, the back's too.
    `folder` is the game path they are read from (objects/kcds_cards, objects/kcds/<resource>);
    `wanted` a set of card ids (and 'back') to make, all by default."""
    mesh = card_mesh()
    pictures = []
    if wanted is None or "back" in wanted:
        pictures.append(("back", back_picture()))
    for card in card_ids():
        if wanted is None or card in wanted:
            pictures.append((card.lower(), face_picture(card)))
    for name, picture in pictures:
        if pictures_out:
            picture.save(os.path.join(pictures_out, f"{name}.png"))
        material = kcds_cgf.write_mtl(card_materials(folder, f"{folder}/{name}.dds"))
        yield f"{name}.dds", texture_bytes(picture)
        yield f"{name}.cgf", kcds_cgf.write_cgf(mesh, f"{folder}/{name}", node_name=name)
        yield f"{name}.mtl", material.encode("ascii")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--resource", required=True, help="the resource's folder (its name is its last part)"
    )
    parser.add_argument("--only", help="comma separated card ids (and 'back') to make")
    parser.add_argument("--png", help="also save the full-size pictures here as .png")
    args = parser.parse_args()
    # A resource's stream models live under objects/kcds/<its name>/ (the client refuses others).
    name = os.path.basename(os.path.normpath(args.resource))
    folder = f"objects/kcds/{name}"
    out = os.path.join(args.resource, "stream", *folder.split("/"))
    os.makedirs(out, exist_ok=True)
    if args.png:
        os.makedirs(args.png, exist_ok=True)
    wanted = set(args.only.split(",")) if args.only else None
    for file_name, data in deck_files(folder, wanted, args.png):
        with open(os.path.join(out, file_name), "wb") as file:
            file.write(data)
        print(file_name, flush=True)


if __name__ == "__main__":
    main()
