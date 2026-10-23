#!/usr/bin/env python3
"""Mesh-shading fixture: one type 4 (free-form Gouraud) page and one type 5
(lattice) page, both RGB, built by hand so the geometry is exact."""
import importlib.util
import os

spec = importlib.util.spec_from_file_location(
    "mk", os.path.join(os.path.dirname(os.path.abspath(__file__)), "make_pattern_fixtures.py"))
mk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mk)

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "test", "fixtures")


def mesh_type4():
    # two triangles: (20,20) red -> (280,20) green -> (280,280) blue, and the
    # second half of the square in the same colours, so a correct render shows a
    # smooth diagonal gradient.
    verts = [
        (20, 20, 1.0, 0.0, 0.0), (280, 20, 0.0, 1.0, 0.0), (280, 280, 0.0, 0.0, 1.0),
        (20, 20, 1.0, 0.0, 0.0), (280, 280, 0.0, 0.0, 1.0), (20, 280, 1.0, 1.0, 0.0),
    ]
    # /Decode maps the raw 0..255 code to 0..300 points, so the fixture must
    # encode the *normalised* coordinate, not the point value.
    out = bytearray()
    for x, y, r, g, b in verts:
        out += bytes([int(round(x / 300 * 255)), int(round(y / 300 * 255)),
                      int(r * 255) & 255, int(g * 255) & 255, int(b * 255) & 255])
    return bytes(out)


def mesh_type5():
    # 3x2 lattice, 8-bit, one flag bit + x,y,r,g,b per vertex
    pts = [(20, 20, 1, 0, 0), (150, 40, 1, 1, 0), (280, 20, 0, 1, 0),
           (20, 280, 0, 0, 1), (150, 260, 0, 1, 1), (280, 280, 1, 0, 1)]
    bits = ""
    for x, y, r, g, b in pts:
        xn = int(round(x / 300 * 255))
        yn = int(round(y / 300 * 255))
        bits += "0" + format(xn, "08b") + format(yn, "08b") + \
                format(int(r * 255), "08b") + format(int(g * 255), "08b") + format(int(b * 255), "08b")
    bits += "0" * ((8 - len(bits) % 8) % 8)
    out = bytearray()
    for i in range(0, len(bits), 8):
        out.append(int(bits[i:i + 8], 2))
    return bytes(out)


shared = b"/ShadingType %d /ColorSpace /DeviceRGB /BitsPerCoordinate 8 /BitsPerComponent 8 /Decode [0 300 0 300 0 1 0 1 0 1]"

objs = [
    b"<< /Type /Catalog /Pages 2 0 R >>",
    b"<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Resources << /Shading << /S1 7 0 R >> >> /Contents 5 0 R >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Resources << /Shading << /S2 8 0 R >> >> /Contents 6 0 R >>",
    mk.stream("/S1 sh\n"),
    mk.stream("/S2 sh\n"),
    mk.stream(mesh_type4(), (shared % 4) + b" /BBox [0 0 300 300]"),
    mk.stream(mesh_type5(), (shared % 5) + b" /VerticesPerRow 3 /BBox [0 0 300 300]"),
]
mk.build(objs, os.path.join(OUT, "mesh.pdf"), 2)
