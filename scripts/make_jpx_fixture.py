#!/usr/bin/env python3
"""Build test/fixtures/photo-jpx.pdf: one page whose image is JPEG 2000.

    ~/pdfvenv/bin/python scripts/make_jpx_fixture.py

The picture is synthesised (a colour chart, not a photograph) so the fixture is
reproducible without shipping a bitmap: smooth gradients exercise the JPX
decoder's colour path, the thin bars and circles expose any subsampling error,
and the same page rendered by MuPDF is the reference.
"""
import io

import pikepdf
from PIL import Image, ImageDraw
from pikepdf import Array, Dictionary, Name

W_PX, H_PX = 640, 420


def chart():
    img = Image.new("RGB", (W_PX, H_PX))
    d = ImageDraw.Draw(img)
    for y in range(H_PX):                       # vertical hue ramp
        for x in range(0, W_PX, 8):
            d.rectangle([x, y, x + 8, y], fill=(x * 255 // W_PX, y * 255 // H_PX,
                                                (255 - x * 255 // W_PX)))
    d.rectangle([24, 24, W_PX - 24, H_PX - 24], outline=(255, 255, 255), width=3)
    for i in range(6):                          # colour wheel
        cx = 110 + i * 85
        d.ellipse([cx - 34, 70, cx + 34, 138], fill=(255 - i * 40, i * 40, 90))
    for i, w in enumerate([1, 2, 3, 4, 6, 9, 13]):   # stroke ladder
        d.rectangle([60, 200 + i * 16, 300, 200 + i * 16 + w], fill=(20, 20, 20))
    for i in range(9):                          # high-frequency wedge
        x0 = 340 + i * 33
        d.rectangle([x0, 200, x0 + 16, 320], fill=(10, 10, 10))
    d.rectangle([60, 330, 580, 390], fill=(255, 255, 255))
    d.text((70, 345), "JPEG 2000 (JPXDecode) - synthetic chart", fill=(0, 0, 0))
    return img


def main():
    img = chart()
    buf = io.BytesIO()
    img.save(buf, format="JPEG2000", quality_mode="rates", quality_layers=[20])
    jp2 = buf.getvalue()

    pdf = pikepdf.new()
    page = pdf.add_blank_page(page_size=(612, 792))
    # draw at 1.5x so the rasterizer has to resample, not blit
    scaled_w, scaled_h = W_PX * 1.5, H_PX * 1.5
    x0 = (612 - scaled_w) / 2
    y0 = (792 - scaled_h) / 2
    im = pdf.make_stream(jp2)
    im.Type = Name.XObject
    im.Subtype = Name.Image
    im.Width = W_PX
    im.Height = H_PX
    im.ColorSpace = Name.DeviceRGB
    im.BitsPerComponent = 8
    im.Filter = Name.JPXDecode
    page.Resources = Dictionary(XObject=Dictionary(Im0=pdf.make_indirect(im)))
    page.Contents = pdf.make_stream(
        f"q {scaled_w} 0 0 {scaled_h} {x0} {y0} cm /Im0 Do Q".encode())
    pdf.save("test/fixtures/photo-jpx.pdf")
    print(f"wrote test/fixtures/photo-jpx.pdf ({len(jp2) / 1024:.0f} KB of JP2)")


if __name__ == "__main__":
    main()
