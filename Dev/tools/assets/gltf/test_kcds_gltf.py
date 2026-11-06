"""glTF 2.0 to the studio's meshes (kcds_gltf.py): files made here (a .glb with an
embedded PNG, a .gltf with its buffer and picture beside it, a mirrored node), their axes,
winding, texture coordinates and textures, the PNG decoder's filters, and the refusals."""

import contextlib
import io
import json
import os
import struct
import tempfile
import unittest
import zlib

import kcds_gltf

# A quad facing up (glTF +y), 2 x 2 m, at the origin: two triangles, counter-clockwise from above.
QUAD_POSITIONS = [(-1.0, 0.0, 1.0), (1.0, 0.0, 1.0), (1.0, 0.0, -1.0), (-1.0, 0.0, -1.0)]
QUAD_NORMALS = [(0.0, 1.0, 0.0)] * 4
QUAD_UVS = [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)]
QUAD_INDICES = [0, 1, 2, 0, 2, 3]


def png(width, height, rgba_rows, filters=None):
    """A PNG (RGBA, 8 bits) of the rows, each written with its filter (0 to 4)."""

    def paeth(a, b, c):
        p = a + b - c
        pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
        return a if pa <= pb and pa <= pc else (b if pb <= pc else c)

    raw = b""
    previous = bytes(width * 4)
    for y, row in enumerate(rgba_rows):
        kind = (filters or [0] * height)[y]
        out = bytearray()
        for x, value in enumerate(row):
            left = row[x - 4] if x >= 4 else 0
            up = previous[x]
            corner = previous[x - 4] if x >= 4 else 0
            predictor = [0, left, up, (left + up) >> 1, paeth(left, up, corner)][kind]
            out.append((value - predictor) & 0xFF)
        raw += bytes([kind]) + bytes(out)
        previous = row

    def chunk(kind, data):
        return (
            struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
        )

    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(raw))
        + chunk(b"IEND", b"")
    )


def checker():
    rows = []
    for y in range(4):
        row = bytearray()
        for x in range(4):
            row += bytes((255, 0, 0, 255)) if (x + y) % 2 == 0 else bytes((0, 0, 255, 128))
        rows.append(bytes(row))
    return rows


def quad_document(node=None, image=None, with_texture=True):
    """The quad's glTF JSON and its one buffer's bytes (positions, normals, uvs, indices, a PNG)."""
    data = b"".join(struct.pack("<3f", *p) for p in QUAD_POSITIONS)
    data += b"".join(struct.pack("<3f", *n) for n in QUAD_NORMALS)
    data += b"".join(struct.pack("<2f", *uv) for uv in QUAD_UVS)
    data += struct.pack("<6H", *QUAD_INDICES) + b"\0\0"
    views = [
        {"buffer": 0, "byteOffset": 0, "byteLength": 48},
        {"buffer": 0, "byteOffset": 48, "byteLength": 48},
        {"buffer": 0, "byteOffset": 96, "byteLength": 32},
        {"buffer": 0, "byteOffset": 128, "byteLength": 12},
    ]
    document = {
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [node or {"mesh": 0}],
        "meshes": [
            {
                "primitives": [
                    {
                        "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                        "indices": 3,
                        "material": 0,
                    }
                ]
            }
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"},
            {"bufferView": 3, "componentType": 5123, "count": 6, "type": "SCALAR"},
        ],
        "bufferViews": views,
        "materials": [
            {
                "name": "Floor Tiles",
                "pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.25, 0.125, 1]},
            }
        ],
    }
    if with_texture:
        picture = image if image is not None else png(4, 4, checker())
        views.append({"buffer": 0, "byteOffset": len(data), "byteLength": len(picture)})
        data += picture
        document["images"] = [{"bufferView": 4, "mimeType": "image/png"}]
        document["textures"] = [{"source": 0}]
        document["materials"][0]["pbrMetallicRoughness"]["baseColorTexture"] = {"index": 0}
    document["buffers"] = [{"byteLength": len(data)}]
    return document, data


def glb(document, data):
    text = json.dumps(document).encode("utf-8")
    text += b" " * (-len(text) % 4)
    data += b"\0" * (-len(data) % 4)
    body = (
        struct.pack("<II", len(text), 0x4E4F534A)
        + text
        + struct.pack("<II", len(data), 0x004E4942)
        + data
    )
    return struct.pack("<III", 0x46546C67, 2, 12 + len(body)) + body


def run(path, name, folder):
    printed = io.StringIO()
    with contextlib.redirect_stdout(printed):
        code = kcds_gltf.main([path, name, folder])
    return code, json.loads(printed.getvalue().splitlines()[-1])


class GltfTest(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.mkdtemp()

    def tearDown(self):
        for root, _, names in os.walk(self.folder, topdown=False):
            for one in names:
                os.remove(os.path.join(root, one))
            os.rmdir(root)

    def write(self, file_name, data):
        path = os.path.join(self.folder, file_name)
        with open(path, "wb") as file:
            file.write(data)
        return path

    def assert_faces_up_in_game(self, mesh):
        """Each triangle turns counter-clockwise seen from the game's +z, and agrees with its
        vertices' tangent frames (tangent x bitangent = normal times the sign)."""
        for at in range(0, len(mesh.indices), 3):
            a, b, c = mesh.indices[at : at + 3]
            pa, pb, pc = mesh.positions[a], mesh.positions[b], mesh.positions[c]
            face = kcds_gltf._cross(
                [pb[k] - pa[k] for k in range(3)], [pc[k] - pa[k] for k in range(3)]
            )
            self.assertGreater(face[2], 0)
            for vertex in (a, b, c):
                tangent, bitangent, sign = mesh.tangents[vertex]
                frame = kcds_gltf._cross(tangent, bitangent)
                self.assertGreater(sum(frame[k] * face[k] for k in range(3)) * sign, 0)

    def test_glb_with_an_embedded_png(self):
        document, data = quad_document(node={"mesh": 0, "translation": [0, 1, 0]})
        path = self.write("tile.glb", glb(document, data))
        code, made = run(path, "tile", self.folder)
        self.assertEqual(code, 0, made)
        self.assertEqual(
            (made["vertices"], made["triangles"], made["materials"], made["textures"]), (4, 2, 1, 1)
        )
        document, buffers, _ = kcds_gltf.load(path)
        mesh, _ = kcds_gltf.build(document, buffers)
        # glTF y up 1 m is the game's z 1 m; glTF +z (towards the viewer) is the game's -y.
        self.assertEqual(
            sorted(mesh.positions),
            sorted([(-1.0, -1.0, 1.0), (1.0, -1.0, 1.0), (1.0, 1.0, 1.0), (-1.0, 1.0, 1.0)]),
        )
        self.assertEqual(sorted(mesh.uvs), sorted(QUAD_UVS))
        self.assert_faces_up_in_game(mesh)
        with open(os.path.join(self.folder, "tile.mtl"), encoding="ascii") as file:
            material = file.read()
        self.assertIn('Name="Floor_Tiles"', material)
        self.assertIn('File="objects/kcds_studio/tile_Floor_Tiles.dds"', material)
        with open(os.path.join(self.folder, "tile_Floor_Tiles.dds"), "rb") as file:
            dds = file.read()
        self.assertEqual(dds[:4], b"DDS ")
        self.assertEqual(struct.unpack_from("<II", dds, 12), (4, 4))
        # The first pixel (red, opaque) as blue green red alpha.
        self.assertEqual(dds[128:132], bytes((0, 0, 255, 255)))

    def test_a_resource_game_folder_names_the_model_and_its_textures(self):
        document, data = quad_document()
        path = self.write("tile.glb", glb(document, data))
        printed = io.StringIO()
        with contextlib.redirect_stdout(printed):
            code = kcds_gltf.main([path, "tile", self.folder, "objects/kcds/chess"])
        self.assertEqual(code, 0, printed.getvalue())
        with open(os.path.join(self.folder, "tile.mtl"), encoding="ascii") as file:
            self.assertIn('File="objects/kcds/chess/tile_Floor_Tiles.dds"', file.read())
        with open(os.path.join(self.folder, "tile.cgf"), "rb") as file:
            self.assertIn(b"objects/kcds/chess/tile", file.read())
        for wrong in ("kcds/chess", "objects/../x", "objects/a b"):
            printed = io.StringIO()
            with contextlib.redirect_stdout(printed):
                code = kcds_gltf.main([path, "tile", self.folder, wrong])
            self.assertEqual(code, 1, wrong)
            self.assertIn("game folder", printed.getvalue())

    def test_gltf_with_its_buffer_and_picture_beside_it(self):
        document, data = quad_document(with_texture=False)
        self.write("quad.bin", data)
        self.write("grid.png", png(4, 4, checker()))
        document["buffers"] = [{"uri": "quad.bin", "byteLength": len(data)}]
        document["images"] = [{"uri": "grid.png"}]
        document["textures"] = [{"source": 0}]
        document["materials"][0]["pbrMetallicRoughness"]["baseColorTexture"] = {"index": 0}
        document["materials"][0]["normalTexture"] = {"index": 0}
        path = self.write("quad.gltf", json.dumps(document).encode("utf-8"))
        code, made = run(path, "quad", self.folder)
        self.assertEqual(code, 0, made)
        self.assertEqual(made["textures"], 2)
        self.assertTrue(os.path.isfile(os.path.join(self.folder, "quad_Floor_Tiles_normal.dds")))
        with open(os.path.join(self.folder, "quad.mtl"), encoding="ascii") as file:
            self.assertIn("%NORMAL_MAP", file.read())

    def test_a_mirrored_node_keeps_its_faces_outward(self):
        document, data = quad_document(node={"mesh": 0, "scale": [-1, 1, 1]}, with_texture=False)
        path = self.write("mirror.glb", glb(document, data))
        document, buffers, _ = kcds_gltf.load(path)
        mesh, _ = kcds_gltf.build(document, buffers)
        self.assert_faces_up_in_game(mesh)

    def test_tangents_made_when_the_file_has_none_agree_with_the_uvs(self):
        document, data = quad_document(with_texture=False)
        path = self.write("plain.glb", glb(document, data))
        document, buffers, _ = kcds_gltf.load(path)
        mesh, _ = kcds_gltf.build(document, buffers)
        for (tangent, bitangent, _), (u, v), position in zip(
            mesh.tangents, mesh.uvs, mesh.positions
        ):
            # u grows with the game's +x here, v falls towards the game's +y (glTF -z).
            self.assertAlmostEqual(tangent[0], 1.0, places=3)
            self.assertAlmostEqual(bitangent[1], 1.0, places=3)

    def test_png_filters_and_palette(self):
        rows = checker()
        for kind in range(5):
            width, height, bgra = kcds_gltf.decode_png(png(4, 4, rows, [kind] * 4))
            self.assertEqual((width, height), (4, 4))
            self.assertEqual(bgra[4:8], bytes((255, 0, 0, 128)), f"filter {kind}")
        mixed = kcds_gltf.decode_png(png(4, 4, rows, [0, 1, 4, 3]))
        self.assertEqual(mixed, kcds_gltf.decode_png(png(4, 4, rows)))

    def test_a_glb_with_a_jpeg_texture(self):
        pictures = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_pictures")
        with open(os.path.join(pictures, "420.jpg"), "rb") as file:
            document, data = quad_document(image=file.read())
        code, made = run(self.write("photo.glb", glb(document, data)), "photo", self.folder)
        self.assertEqual(code, 0, made)
        self.assertEqual(made["textures"], 1)
        with open(os.path.join(self.folder, "photo_Floor_Tiles.dds"), "rb") as file:
            self.assertEqual(struct.unpack_from("<II", file.read(), 12), (29, 37))

    def test_refusals(self):
        document, data = quad_document(image=b"\xff\xd8\xff\xe0 a broken JPEG")
        code, made = run(self.write("jpeg.glb", glb(document, data)), "jpeg", self.folder)
        self.assertEqual(code, 1)
        self.assertIn("a JPEG texture: ", made["error"])
        old = {"asset": {"version": "1.0"}}
        code, made = run(self.write("old.gltf", json.dumps(old).encode()), "old", self.folder)
        self.assertIn("only glTF 2.0", made["error"])
        document, data = quad_document(with_texture=False)
        document["meshes"][0]["primitives"][0]["mode"] = 1
        code, made = run(self.write("lines.glb", glb(document, data)), "lines", self.folder)
        self.assertIn("only triangle meshes", made["error"])
        document, data = quad_document(with_texture=False)
        code, made = run(self.write("ok.glb", glb(document, data)), "bad name", self.folder)
        self.assertIn("the name is", made["error"])
        self.assertFalse(os.path.exists(os.path.join(self.folder, "bad name.cgf")))


if __name__ == "__main__":
    unittest.main()
