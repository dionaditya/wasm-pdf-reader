#!/usr/bin/env python3
"""Build test/fixtures/annot-quads.pdf: annotations that carry no /AP.

Page 1: yellow highlight over two lines, red underline, strikeout and squiggly,
        all described only by /QuadPoints (no appearance streams).
Page 2: three /Text notes (no /AP), so the engine has to draw the note icon.

Reference renders come from MuPDF:

    ~/pdfvenv/bin/python - <<'PY'
    import pymupdf
    d = pymupdf.open('test/fixtures/annot-quads.pdf')
    for i in range(len(d)):
        d[i].get_pixmap(matrix=pymupdf.Matrix(1, 1), colorspace=pymupdf.csGRAY).save(f'/tmp/ref{i}.png')
    PY
"""
import pikepdf
from pikepdf import Array, Dictionary, Name

W, H = 612, 792
LINES = [
    (72, 700, "The quick brown fox jumps over the lazy dog."),
    (72, 676, "Highlighted text is painted from /QuadPoints."),
    (72, 652, "Underline, strikeout and squiggly use the same data."),
    (72, 628, "No appearance stream is associated with any of them."),
]


def content():
    ops = ["BT", "/F1 14 Tf", "16 TL"]
    for x, y, text in LINES:
        ops.append(f"1 0 0 1 {x} {y} Tm ({text}) Tj")
    ops += ["ET", "0.4 w", "0 0 0 RG", "72 600 m 480 600 l S"]
    ops.append("BT /F1 10 Tf 1 0 0 1 72 570 Tm (Annotations without /AP) Tj ET")
    return "\n".join(ops).encode("latin-1")


def quad(x0, y0, x1, y1):
    """Upper-left, upper-right, lower-left, lower-right."""
    return Array([x0, y1, x1, y1, x0, y0, x1, y0])


def main():
    pdf = pikepdf.new()
    font = pdf.make_indirect(Dictionary(
        Type=Name.Font, Subtype=Name.Type1, BaseFont=Name.Helvetica,
        Encoding=Name.WinAnsiEncoding,
    ))

    def page(annots, extra=()):
        p = pdf.add_blank_page(page_size=(W, H))
        p.Resources = Dictionary(Font=Dictionary(F1=font))
        data = content()
        p.Contents = pdf.make_stream(data)
        p.Annots = Array(annots)
        return p

    # ── page 1: text-markup annotations, no /AP ───────────────────────────
    p1 = page([
        # two quads -> one highlight across both lines
        pdf.make_indirect(Dictionary(
            Type=Name.Annot, Subtype=Name.Highlight, Rect=Array([72, 694, 480, 722]),
            QuadPoints=Array(list(quad(72, 702, 480, 718)) + list(quad(72, 678, 470, 694))),
            C=Array([1, 0.9, 0]), CA=0.45, F=4,
        )),
        pdf.make_indirect(Dictionary(
            Type=Name.Annot, Subtype=Name.Underline, Rect=Array([72, 648, 480, 668]),
            QuadPoints=quad(72, 652, 480, 668), C=Array([1, 0, 0]), F=4,
        )),
        pdf.make_indirect(Dictionary(
            Type=Name.Annot, Subtype=Name.StrikeOut, Rect=Array([72, 624, 480, 644]),
            QuadPoints=quad(72, 628, 480, 644), C=Array([0, 0, 1]), F=4,
        )),
        pdf.make_indirect(Dictionary(
            Type=Name.Annot, Subtype=Name.Squiggly, Rect=Array([72, 600, 480, 620]),
            QuadPoints=quad(72, 604, 480, 620), C=Array([0, 0.6, 0]), F=4,
        )),
    ])

    # ── page 2: text notes, no /AP (the icon must be synthesised) ────────
    notes = []
    for i, name in enumerate(["Note", "Comment", "Help"]):
        notes.append(pdf.make_indirect(Dictionary(
            Type=Name.Annot, Subtype=Name.Text, Rect=Array([460, 690 - i * 40, 480, 710 - i * 40]),
            Contents=f"Note number {i + 1}".encode(), Name=Name("/" + name),
            C=Array([1, 0.85, 0]), F=4,
        )))
    txt = (
        "BT /F1 14 Tf 1 0 0 1 72 700 Tm (Three text notes, no appearance streams.) Tj ET\n"
        "BT /F1 10 Tf 1 0 0 1 72 670 Tm (A viewer has to draw the icon from /Rect.) Tj ET"
    ).encode("latin-1")
    p2 = pdf.add_blank_page(page_size=(W, H))
    p2.Resources = Dictionary(Font=Dictionary(F1=font))
    p2.Contents = pdf.make_stream(txt)
    p2.Annots = Array(notes)

    # ── page 3: /AP wins over the fallback ───────────────────────────────
    # A highlight that carries both /QuadPoints and an appearance stream: the
    # appearance draws one thin diagonal line, so a bug that also fills the
    # quad shows up as a large ink jump.
    ap = pdf.make_stream(b"0 0 0 RG 1 w 0 0 m 408 80 l S Q")
    ap.Type = Name.XObject
    ap.Subtype = Name.Form
    ap.BBox = Array([0, 0, 408, 80])
    ap.Resources = Dictionary()
    p3 = pdf.add_blank_page(page_size=(W, H))
    p3.Resources = Dictionary(Font=Dictionary(F1=font))
    p3.Contents = pdf.make_stream(b"BT /F1 12 Tf 1 0 0 1 72 700 Tm (The appearance stream wins.) Tj ET")
    p3.Annots = Array([pdf.make_indirect(Dictionary(
        Type=Name.Annot, Subtype=Name.Highlight, Rect=Array([72, 620, 480, 700]),
        QuadPoints=quad(72, 620, 480, 700), C=Array([1, 1, 0]),
        AP=Dictionary(N=ap), F=4,
    ))])

    # ── page 4: rotated page, marks must follow the page CTM ─────────────
    p4 = pdf.add_blank_page(page_size=(W, H))
    p4.Rotate = 90
    p4.Resources = Dictionary(Font=Dictionary(F1=font))
    p4.Contents = pdf.make_stream(
        b"BT /F1 14 Tf 1 0 0 1 72 700 Tm (Rotated page with a highlight.) Tj ET"
        b"\nBT /F1 12 Tf 1 0 0 1 72 660 Tm (The mark follows the page CTM.) Tj ET")
    p4.Annots = Array([
        pdf.make_indirect(Dictionary(
            Type=Name.Annot, Subtype=Name.Highlight, Rect=Array([72, 694, 380, 716]),
            QuadPoints=quad(72, 694, 380, 716), C=Array([0, 0.8, 1]), CA=0.5, F=4,
        )),
        pdf.make_indirect(Dictionary(
            Type=Name.Annot, Subtype=Name.Text, Rect=Array([430, 620, 450, 640]),
            Contents=b"Rotated note", C=Array([1, 0.85, 0]), F=4,
        )),
    ])

    pdf.save("test/fixtures/annot-quads.pdf")
    print("wrote test/fixtures/annot-quads.pdf")


if __name__ == "__main__":
    main()
