"""Baseline JPEGs in pure Python (kcds_jpeg.py): pictures libjpeg wrote (Pillow, once,
into test_pictures) from the picture made here, read back close to it: every sampling (4:4:4,
4:2:2, 4:2:0), grey, restart markers; progressive files are refused."""

import math
import os
import unittest

import kcds_gltf
import kcds_jpeg

PICTURES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_pictures")


def source(width=37, height=29):
    """The picture the JPEGs were made of: smooth colours, an odd size (blocks cut at the edges)."""
    pixels = []
    for y in range(height):
        for x in range(width):
            pixels.append(
                (
                    int(255 * x / (width - 1)),
                    int(255 * y / (height - 1)),
                    int(128 + 100 * math.sin(x / 5.0) * math.cos(y / 7.0)),
                )
            )
    return width, height, pixels


def read(name):
    with open(os.path.join(PICTURES, name), "rb") as file:
        return file.read()


class JpegTest(unittest.TestCase):
    def assert_close(self, name, grey=False, mean_most=3.0, worst_most=40):
        width, height, bgra = kcds_jpeg.decode(read(name))
        self.assertEqual((width, height), (37, 29))
        _, _, pixels = source()
        errors = []
        for index, (r, g, b) in enumerate(pixels):
            if grey:
                # JFIF's grey of the colours.
                r = g = b = int(0.299 * r + 0.587 * g + 0.114 * b + 0.5)
            got_b, got_g, got_r, alpha = bgra[index * 4 : index * 4 + 4]
            self.assertEqual(alpha, 255)
            errors += [abs(got_r - r), abs(got_g - g), abs(got_b - b)]
        self.assertLess(sum(errors) / len(errors), mean_most, name)
        self.assertLess(max(errors), worst_most, name)

    def test_every_sampling(self):
        for name in ("444.jpg", "422.jpg", "420.jpg"):
            self.assert_close(name)

    def test_grey(self):
        self.assert_close("grey.jpg", grey=True)

    def test_restart_markers(self):
        self.assertIn(b"\xff\xdd", read("restart.jpg"))
        self.assert_close("restart.jpg")
        # The same picture without them reads the same.
        self.assertEqual(kcds_jpeg.decode(read("restart.jpg")), kcds_jpeg.decode(read("420.jpg")))

    def test_refusals(self):
        with self.assertRaises(kcds_jpeg.JpegError) as caught:
            kcds_jpeg.decode(read("progressive.jpg"))
        self.assertIn("progressive", str(caught.exception))
        with self.assertRaises(kcds_jpeg.JpegError):
            kcds_jpeg.decode(b"not a picture")
        # The importer says what is wrong with a picture it cannot take.
        with self.assertRaises(kcds_gltf.GltfError) as caught:
            kcds_gltf.decode_picture(read("progressive.jpg"))
        self.assertIn("a JPEG texture: a progressive JPEG", str(caught.exception))
        with self.assertRaises(kcds_gltf.GltfError) as caught:
            kcds_gltf.decode_picture(b"GIF89a")
        self.assertIn("neither a PNG nor a JPEG", str(caught.exception))

    def test_the_importer_reads_a_jpeg_texture(self):
        width, height, bgra = kcds_gltf.decode_picture(read("444.jpg"))
        self.assertEqual((width, height, len(bgra)), (37, 29, 37 * 29 * 4))


if __name__ == "__main__":
    unittest.main()
