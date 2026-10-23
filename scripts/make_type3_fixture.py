#!/usr/bin/env python3
"""Type3 fixture: a glyph that is a content stream (a filled block), so a
correct render shows solid rectangles where the text is drawn."""
import importlib.util, os
spec = importlib.util.spec_from_file_location(
    "mk", os.path.join(os.path.dirname(os.path.abspath(__file__)), "make_pattern_fixtures.py"))
mk = importlib.util.module_from_spec(spec); spec.loader.exec_module(mk)
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "test", "fixtures")

glyph = "600 0 0 0 600 700 d1 0 0 600 700 re f\n"
content = b"BT /T3 40 Tf 30 200 Td (AAA) Tj ET\n"

objs = [
    b"<< /Type /Catalog /Pages 2 0 R >>",
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Resources << /Font << /T3 5 0 R >> >> /Contents 4 0 R >>",
    mk.stream(content),
    b"<< /Type /Font /Subtype /Type3 /FontBBox [0 0 700 700] /FontMatrix [0.001 0 0 0.001 0 0] "
    b"/CharProcs << /A 6 0 R >> /Encoding << /Type /Encoding /Differences [65 /A] >> "
    b"/FirstChar 65 /LastChar 65 /Widths [600] /Resources << >> >>",
    mk.stream(glyph),
]
mk.build(objs, os.path.join(OUT, "type3.pdf"), 1)
