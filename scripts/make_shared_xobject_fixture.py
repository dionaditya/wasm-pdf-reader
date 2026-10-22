#!/usr/bin/env python3
"""Build test/fixtures/shared-xobject.pdf: one image and one form, used twice.

Both pages reference the *same* indirect image XObject and the *same* form
XObject, which is what a letterhead or logo looks like in a real document. It is
the reading-side twin of the mupdf.js #201 report: a stream object reached
through more than one reference must still be a stream, with its bytes, on every
visit.

    ~/pdfvenv/bin/python scripts/make_shared_xobject_fixture.py
"""
import io
import struct
import zlib

import pikepdf
from PIL import Image, ImageDraw
from pikepdf import Array, Dictionary, Name

W, H = 64, 64


def image_bytes():
    img = Image.new("RGB", (W, H), (250, 250, 250))
    d = ImageDraw.Draw(img)
    d.ellipse([4, 4, W - 4, H - 4], fill=(200, 30, 40))          # a red disc
    d.rectangle([W // 2 - 5, 6, W // 2 + 5, H - 6], fill=(20, 30, 120))
    raw = img.tobytes()
    rows = b"".join(b"\x00" + raw[y * W * 3:(y + 1) * W * 3] for y in range(H))
    return zlib.compress(rows, 9)


def main():
    pdf = pikepdf.new()

    im = pdf.make_stream(image_bytes())
    im.Type = Name.XObject
    im.Subtype = Name.Image
    im.Width, im.Height = W, H
    im.ColorSpace = Name.DeviceRGB
    im.BitsPerComponent = 8
    im.Filter = Name.FlateDecode
    im = pdf.make_indirect(im)

    # A form that draws the shared image: also shared by both pages.
    form = pdf.make_stream(b"q 100 0 0 100 0 0 cm /Im0 Do Q")
    form.Type = Name.XObject
    form.Subtype = Name.Form
    form.BBox = Array([0, 0, 100, 100])
    form.Resources = Dictionary(XObject=Dictionary(Im0=im))
    form = pdf.make_indirect(form)

    for i in range(2):
        page = pdf.add_blank_page(page_size=(300, 300))
        page.Resources = Dictionary(XObject=Dictionary(Im0=im, Fm0=form))
        # Draw the image directly twice and the form once, so the same stream is
        # handed to the rasterizer three times per page.
        page.Contents = pdf.make_stream(
            b"q 60 0 0 60 20 200 cm /Im0 Do Q\n"
            b"q 60 0 0 60 120 200 cm /Im0 Do Q\n"
            b"q 1 0 0 1 100 40 cm /Fm0 Do Q\n"
            b"BT /F1 9 Tf 20 20 Td (shared image and form) Tj ET"
        )
        page.Resources.Font = Dictionary(F1=pdf.make_indirect(Dictionary(
            Type=Name.Font, Subtype=Name.Type1, BaseFont=Name.Helvetica,
            Encoding=Name.WinAnsiEncoding)))

    pdf.save("test/fixtures/shared-xobject.pdf")
    print("wrote test/fixtures/shared-xobject.pdf")


if __name__ == "__main__":
    main()
