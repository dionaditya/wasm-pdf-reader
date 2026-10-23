#!/usr/bin/env python3
"""Generate the tiling-pattern and transparency fixtures (hand-built PDFs).

Patterns:  a 45-degree two-colour checker, and a hatch fill under text.
Transparency: Multiply logo over a background, a /ca 0.5 group, and a CMYK fill
with alpha — the three shapes that matter for M3.
"""
import os
import zlib

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "test", "fixtures")


def build(objs, path, page_count):
    """objs: list of object bodies (bytes), 1-based; returns a valid PDF."""
    out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = []
    for i, body in enumerate(objs, 1):
        offsets.append(len(out))
        out += b"%d 0 obj\n" % i + body + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n" % (len(objs) + 1)
    out += b"0000000000 65535 f \n"
    for off in offsets:
        out += b"%010d 00000 n \n" % off
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, xref)
    with open(path, "wb") as f:
        f.write(out)
    print(path, len(out), "bytes,", page_count, "pages")


def stream(data, extra=b""):
    if isinstance(data, str):
        data = data.encode("latin-1")
    return b"<< /Length %d %s >>\nstream\n" % (len(data), extra) + data + b"\nendstream"


def make_patterns():
    page_content_1 = b"/Pattern cs /P1 scn 20 20 260 260 re f\n"
    page_content_2 = (b"/Pattern cs /P2 scn 20 20 260 120 re f\n"
                      b"0 0 0 rg BT /F1 18 Tf 30 220 Td (Hatch fill di bawah teks) Tj ET\n"
                      b"0 0 0 rg BT /F1 12 Tf 30 90 Td (teks menimpa pola) Tj ET\n")

    checker = ("1 0 0 rg\n0 0 10 10 re f\n10 10 10 10 re f\n")
    hatch = ("0 0 0.55 RG\n0.9 w\n0 0 m 8 8 l S\n")

    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Resources 7 0 R /Contents 5 0 R >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Resources 7 0 R /Contents 6 0 R >>",
        stream(page_content_1),
        stream(page_content_2),
        b"<< /Pattern << /P1 8 0 R /P2 9 0 R >> /Font << /F1 10 0 R >> >>",
        stream(checker, b"/Type /Pattern /PatternType 1 /PaintType 1 /TilingType 1 "
                         b"/BBox [0 0 20 20] /XStep 20 /YStep 20 "
                         b"/Matrix [0.7071 0.7071 -0.7071 0.7071 0 0] /Resources << >>"),
        stream(hatch, b"/Type /Pattern /PatternType 1 /PaintType 1 /TilingType 1 "
                       b"/BBox [0 0 8 8] /XStep 8 /YStep 8 /Resources << >>"),
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]
    build(objs, os.path.join(OUT, "patterns.pdf"), 2)


def make_transparency():
    # page 1: Multiply logo over a two-tone background
    p1 = ("0.85 0.9 1 rg 0 0 300 300 re f\n"
          "0.2 0.4 0.8 rg 0 0 300 150 re f\n"
          "/GS1 gs /Fm1 Do\n")
    # page 2: the same logo as a /ca 0.5 group (Normal blend)
    p2 = ("0.85 0.9 1 rg 0 0 300 300 re f\n"
          "0.2 0.4 0.8 rg 0 0 300 150 re f\n"
          "/GS2 gs /Fm1 Do\n")
    # page 3: transparent CMYK fill (no group) — the "transparent cmyk" case
    p3 = ("0.85 0.9 1 rg 0 0 300 300 re f\n"
          "0.9 0 0.85 0.05 k /GS2 gs 40 40 220 220 re f\n"
          "0 0.7 0.7 0 k /GS3 gs 120 120 160 160 re f\n")

    # RGB only: this fixture is about blending, not about the default CMYK
    # profile, which is a separate (colour-management) milestone.
    logo = ("1 0.15 0.1 rg 40 40 220 220 re f\n"
            "0.1 0.25 1 rg 90 90 120 120 re f\n")

    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Resources 10 0 R /Contents 6 0 R >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Resources 10 0 R /Contents 7 0 R >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Resources 10 0 R /Contents 8 0 R >>",
        stream(p1),
        stream(p2),
        stream(p3),
        stream(logo, b"/Type /XObject /Subtype /Form /BBox [0 0 300 300] "
                     b"/Group << /S /Transparency /CS /DeviceRGB /I true >> "
                     b"/Resources << >>"),
        b"<< /XObject << /Fm1 9 0 R >> "
        b"/ExtGState << /GS1 << /BM /Multiply >> /GS2 << /ca 0.5 /CA 0.5 >> "
        b"/GS3 << /ca 0.75 >> >> >>",
    ]
    build(objs, os.path.join(OUT, "transparency.pdf"), 3)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    make_patterns()
    make_transparency()
