#!/usr/bin/env python3
"""AcroForm fixture: three widgets with no /AP, so the renderer has to
synthesize their appearance (background, border, value, tick)."""
import importlib.util, os
spec = importlib.util.spec_from_file_location(
    "mk", os.path.join(os.path.dirname(os.path.abspath(__file__)), "make_pattern_fixtures.py"))
mk = importlib.util.module_from_spec(spec); spec.loader.exec_module(mk)
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "test", "fixtures")

objs = [
    b"<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [6 0 R 7 0 R 8 0 R] /DA (/Helv 0 Tf 0 g) >> >>",
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Resources << /Font << /Helv 9 0 R >> >> "
    b"/Contents 4 0 R /Annots [6 0 R 7 0 R 8 0 R] >>",
    mk.stream("0.9 0.9 0.9 rg 20 20 260 260 re f\n"),
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    b"<< /Type /Annot /Subtype /Widget /FT /Tx /T (nama) /V (Ulfah) "
    b"/Rect [30 200 270 240] /F 4 /DA (/Helv 12 Tf 0 g) /MK << /BC [0 0 0] /BG [1 1 1] >> >>",
    b"<< /Type /Annot /Subtype /Widget /FT /Btn /T (setuju) /V /Yes /AS /Yes "
    b"/Rect [30 150 60 180] /F 4 /MK << /BC [0 0 0] /BG [1 1 1] >> >>",
    b"<< /Type /Annot /Subtype /Widget /FT /Ch /T (kelas) /V (B) "
    b"/Rect [30 90 270 130] /F 4 /MK << /BC [0 0 0] /BG [1 1 1] >> "
    b"/Opt [(A) (B) (C)] >>",
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
]
mk.build(objs, os.path.join(OUT, "form.pdf"), 1)
