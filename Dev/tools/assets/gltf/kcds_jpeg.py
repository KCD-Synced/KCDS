"""Baseline JPEG pictures in pure Python: glTF textures saved as .jpg, made the
game's textures by kcds_gltf.py.

Reads baseline (SOF0) and extended sequential (SOF1) Huffman JPEGs of 8-bit samples, grey (1
component) or YCbCr (3), any sampling factors, restart intervals; the colours as JFIF defines
them. Progressive (SOF2) and arithmetic-coded files are refused with what they are: save such a
picture as PNG or as a baseline JPEG.

    decode(data) -> (width, height, rows from the top as blue green red alpha bytes)
"""

import math
import struct

ZIGZAG = (
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
)  # fmt: skip


class JpegError(ValueError):
    """A JPEG this decoder cannot read, and why."""


# The inverse DCT's cosines: COS[x][u] = c(u) cos((2x + 1) u pi / 16) / 2.
COS = [
    [
        (math.sqrt(0.5) if u == 0 else 1.0) * math.cos((2 * x + 1) * u * math.pi / 16) / 2
        for u in range(8)
    ]
    for x in range(8)
]


def _idct(block):
    """The 8 x 8 samples of a block's coefficients (row-major), level-shifted and clamped."""
    rows = [[0.0] * 8 for _ in range(8)]
    for v in range(8):
        line = block[v * 8 : v * 8 + 8]
        if not any(line):
            continue
        for x in range(8):
            cosines = COS[x]
            rows[v][x] = sum(cosines[u] * line[u] for u in range(8))
    out = [0] * 64
    for x in range(8):
        for y in range(8):
            cosines = COS[y]
            value = sum(cosines[v] * rows[v][x] for v in range(8)) + 128
            out[y * 8 + x] = 0 if value < 0 else 255 if value > 255 else int(value + 0.5)
    return out


class _Huffman:
    def __init__(self, counts, symbols):
        self.lookup = {}
        code = 0
        at = 0
        for length in range(1, 17):
            for _ in range(counts[length - 1]):
                self.lookup[(length, code)] = symbols[at]
                at += 1
                code += 1
            code <<= 1


class _Bits:
    """The entropy-coded data: bits from the top, stuffed 0xFF00 bytes as 0xFF, markers stop it."""

    def __init__(self, data, at):
        self.data = data
        self.at = at
        self.value = 0
        self.count = 0
        self.marker = None

    def bit(self):
        if self.count == 0:
            byte = 0
            if self.marker is None and self.at < len(self.data):
                byte = self.data[self.at]
                if byte == 0xFF:
                    following = self.data[self.at + 1] if self.at + 1 < len(self.data) else 0
                    if following == 0:
                        self.at += 2
                    else:
                        self.marker = following
                        byte = 0
                else:
                    self.at += 1
            self.value = byte
            self.count = 8
        self.count -= 1
        return (self.value >> self.count) & 1

    def bits(self, count):
        value = 0
        for _ in range(count):
            value = (value << 1) | self.bit()
        return value

    def decode(self, table):
        code = 0
        for length in range(1, 17):
            code = (code << 1) | self.bit()
            symbol = table.lookup.get((length, code))
            if symbol is not None:
                return symbol
        raise JpegError("a Huffman code that is in no table")

    def restart(self):
        """Past a restart marker: the bits start anew."""
        self.count = 0
        if self.marker is not None and 0xD0 <= self.marker <= 0xD7:
            self.at += 2
            self.marker = None
        else:
            while self.at + 1 < len(self.data):
                if self.data[self.at] == 0xFF and 0xD0 <= self.data[self.at + 1] <= 0xD7:
                    self.at += 2
                    break
                self.at += 1
            self.marker = None


def _extend(value, size):
    return value - (1 << size) + 1 if size and value < (1 << (size - 1)) else value


def decode(data):
    if data[:2] != b"\xff\xd8":
        raise JpegError("not a JPEG")
    at = 2
    quant = {}
    dc_tables = {}
    ac_tables = {}
    frame = None
    restart = 0
    while at < len(data):
        if data[at] != 0xFF:
            at += 1
            continue
        marker = data[at + 1]
        at += 2
        if marker in (0xD8, 0x01) or 0xD0 <= marker <= 0xD7:
            continue
        if marker == 0xD9:
            break
        length = struct.unpack_from(">H", data, at)[0]
        segment = data[at + 2 : at + length]
        if marker == 0xDB:
            pos = 0
            while pos < len(segment):
                precision, table = segment[pos] >> 4, segment[pos] & 15
                size = 2 if precision else 1
                values = struct.unpack_from(
                    ">" + ("H" if size == 2 else "B") * 64, segment, pos + 1
                )
                quant[table] = values
                pos += 1 + 64 * size
        elif marker == 0xC4:
            pos = 0
            while pos < len(segment):
                kind, table = segment[pos] >> 4, segment[pos] & 15
                counts = segment[pos + 1 : pos + 17]
                total = sum(counts)
                symbols = segment[pos + 17 : pos + 17 + total]
                (ac_tables if kind else dc_tables)[table] = _Huffman(counts, symbols)
                pos += 17 + total
        elif marker in (0xC0, 0xC1):
            precision, height, width, count = struct.unpack_from(">BHHB", segment, 0)
            if precision != 8:
                raise JpegError(f"a JPEG of {precision}-bit samples")
            components = []
            for index in range(count):
                ident, sampling, table = struct.unpack_from(">BBB", segment, 6 + index * 3)
                components.append(
                    {"id": ident, "h": sampling >> 4, "v": sampling & 15, "quant": table}
                )
            frame = {"width": width, "height": height, "components": components}
        elif marker in (0xC2, 0xC6, 0xCA, 0xCE):
            raise JpegError("a progressive JPEG (save it as PNG or as a baseline JPEG)")
        elif 0xC3 <= marker <= 0xCF and marker not in (0xC4, 0xC8, 0xCC):
            raise JpegError("a lossless or arithmetic-coded JPEG (save it as PNG)")
        elif marker == 0xDD:
            restart = struct.unpack_from(">H", segment, 0)[0]
        elif marker == 0xDA:
            if frame is None:
                raise JpegError("the picture's data comes before its frame")
            count = segment[0]
            for index in range(count):
                ident, tables = segment[1 + index * 2], segment[2 + index * 2]
                component = next(one for one in frame["components"] if one["id"] == ident)
                component["dc"] = dc_tables[tables >> 4]
                component["ac"] = ac_tables[tables & 15]
            return _scan(data, at + length, frame, quant, restart)
        at += length
    raise JpegError("the JPEG has no picture data")


def _scan(data, at, frame, quant, restart):
    components = frame["components"]
    if len(components) not in (1, 3):
        raise JpegError(f"a JPEG of {len(components)} colour components (CMYK is not read)")
    width = frame["width"]
    height = frame["height"]
    most_h = max(one["h"] for one in components)
    most_v = max(one["v"] for one in components)
    mcu_width = 8 * most_h
    mcu_height = 8 * most_v
    across = (width + mcu_width - 1) // mcu_width
    down = (height + mcu_height - 1) // mcu_height
    planes = []
    for component in components:
        plane_width = across * component["h"] * 8
        plane_height = down * component["v"] * 8
        planes.append((plane_width, plane_height, bytearray(plane_width * plane_height)))
        component["previous"] = 0
    bits = _Bits(data, at)
    single = len(components) == 1
    if single:
        # One component alone is not interleaved: its own blocks in rows, whatever its sampling.
        component = components[0]
        blocks_across = (width + 7) // 8
        blocks_down = (height + 7) // 8
        plane_width, _, plane = planes[0]
        for index in range(blocks_across * blocks_down):
            if restart and index and index % restart == 0:
                bits.restart()
                component["previous"] = 0
            samples = _block(bits, component, quant)
            bx, by = index % blocks_across, index // blocks_across
            _put(plane, plane_width, bx * 8, by * 8, samples)
    else:
        for mcu in range(across * down):
            if restart and mcu and mcu % restart == 0:
                bits.restart()
                for component in components:
                    component["previous"] = 0
            mx, my = mcu % across, mcu // across
            for component, (plane_width, _, plane) in zip(components, planes):
                for v in range(component["v"]):
                    for h in range(component["h"]):
                        samples = _block(bits, component, quant)
                        x = (mx * component["h"] + h) * 8
                        y = (my * component["v"] + v) * 8
                        _put(plane, plane_width, x, y, samples)
    # A component sampled less than the picture is blended between its samples, each standing at
    # the middle of the pixels it covers (as libjpeg's smooth upsampling does).
    readers = []
    for component, (plane_width, _, plane) in zip(components, planes):
        scale_x = component["h"] / most_h
        scale_y = component["v"] / most_v
        last_x = max(0, math.ceil(width * scale_x) - 1)
        last_y = max(0, math.ceil(height * scale_y) - 1)
        readers.append((plane, plane_width, scale_x, scale_y, last_x, last_y))

    def sample(reader, x, y):
        plane, plane_width, scale_x, scale_y, last_x, last_y = reader
        if scale_x == 1 and scale_y == 1:
            return plane[y * plane_width + x]
        fx = min(max((x + 0.5) * scale_x - 0.5, 0.0), last_x)
        fy = min(max((y + 0.5) * scale_y - 0.5, 0.0), last_y)
        x0, y0 = int(fx), int(fy)
        x1, y1 = min(x0 + 1, last_x), min(y0 + 1, last_y)
        ax, ay = fx - x0, fy - y0
        top = plane[y0 * plane_width + x0] * (1 - ax) + plane[y0 * plane_width + x1] * ax
        bottom = plane[y1 * plane_width + x0] * (1 - ax) + plane[y1 * plane_width + x1] * ax
        return top * (1 - ay) + bottom * ay

    out = bytearray(width * height * 4)
    for y in range(height):
        for x in range(width):
            values = [sample(reader, x, y) for reader in readers]
            if single:
                red = green = blue = values[0]
            else:
                luma, cb, cr = values[0], values[1] - 128, values[2] - 128
                red = luma + 1.402 * cr
                green = luma - 0.344136 * cb - 0.714136 * cr
                blue = luma + 1.772 * cb
            to = (y * width + x) * 4
            out[to] = _byte(blue)
            out[to + 1] = _byte(green)
            out[to + 2] = _byte(red)
            out[to + 3] = 255
    return width, height, bytes(out)


def _byte(value):
    return 0 if value < 0 else 255 if value > 255 else int(value + 0.5)


def _block(bits, component, quant):
    table = quant[component["quant"]]
    coefficients = [0] * 64
    size = bits.decode(component["dc"])
    difference = _extend(bits.bits(size), size) if size else 0
    component["previous"] += difference
    coefficients[0] = component["previous"] * table[0]
    k = 1
    while k < 64:
        symbol = bits.decode(component["ac"])
        run, size = symbol >> 4, symbol & 15
        if size == 0:
            if run == 15:
                k += 16
                continue
            break
        k += run
        if k > 63:
            break
        coefficients[ZIGZAG[k]] = _extend(bits.bits(size), size) * table[k]
        k += 1
    return _idct(coefficients)


def _put(plane, plane_width, x0, y0, samples):
    for y in range(8):
        start = (y0 + y) * plane_width + x0
        plane[start : start + 8] = bytes(samples[y * 8 : y * 8 + 8])
