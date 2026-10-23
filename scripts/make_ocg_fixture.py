#!/usr/bin/env python3
"""OCG + outline fixture: two layers, one switched off, plus two bookmarks."""
import importlib.util, os
spec = importlib.util.spec_from_file_location(
    "mk", os.path.join(os.path.dirname(os.path.abspath(__file__)), "make_pattern_fixtures.py"))
mk = importlib.util.module_from_spec(spec); spec.loader.exec_module(mk)
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "test", "fixtures")

# Layer A is visible (red block), layer B is OFF (blue block). A correct render
# shows the red block only.
content = (b"/OC /MC0 BDC 1 0 0 rg 30 30 240 240 re f EMC\n"
           b"/OC /MC1 BDC 0 0 1 rg 30 160 240 110 re f EMC\n")

objs = [
    b"<< /Type /Catalog /Pages 2 0 R /Outlines 6 0 R /OCProperties 8 0 R >>",
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] "
    b"/Resources << /Properties << /MC0 4 0 R /MC1 5 0 R >> >> /Contents 7 0 R >>",
    b"<< /Type /OCG /Name (Latar Merah) >>",
    b"<< /Type /OCG /Name (Latar Biru) >>",
    b"<< /Type /Outlines /First 9 0 R /Last 10 0 R /Count 2 >>",
    mk.stream(content),
    b"<< /OCGs [4 0 R 5 0 R] /D << /OFF [5 0 R] /Order [4 0 R 5 0 R] >> >>",
    b"<< /Title (Latar Merah) /Parent 6 0 R /Next 10 0 R /Dest [3 0 R /Fit] >>",
    b"<< /Title (Halaman Satu) /Parent 6 0 R /Prev 9 0 R /Dest [3 0 R /XYZ 0 300 0] >>",
]
mk.build(objs, os.path.join(OUT, "ocg.pdf"), 1)
