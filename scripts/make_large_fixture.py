#!/usr/bin/env python3
"""make_large_fixture.py — a PDF big enough to prove ranged loading.

Writes `test/fixtures/large-scan.pdf` (~120 MB until M9.2's gate is met by a
random-access read, not by a lucky page order) and it is NOT committed: the
repo would grow by a tenth. `test/ranged.mjs` generates it on demand and
`.gitignore` keeps it out.

Shape, chosen so that "page 1 renders" cannot secretly mean "the whole file was
read anyway":

  * 240 pages, each with a ~500 KB content stream, so the page you want is
    nowhere near the byte range the parser needs first.
  * a classic xref table at the end (not an xref stream, not linearised), so the
    engine must read the tail, then jump to individual objects by offset.
  * the page tree is a single /Pages node with a 240-entry /Kids array, which is
    what a real producer writes and what a naive "read everything" parser would
    need to buffer.

Run: ~/pdfvenv/bin/python scripts/make_large_fixture.py [--pages 240] [--kb 500]
"""

import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "test" / "fixtures" / "large-scan.pdf"


def content_stream(page_index: int, kb: int) -> bytes:
    """A page that draws a header, a rule and a block of justified-looking text.

    The padding is a PDF comment (`% ...`), which the lexer must skip — so the
    bytes are real work for the parser, not inert filler it can ignore.
    """
    lines = [
        b"BT /F1 18 Tf 72 760 Td (Large fixture - page %d) Tj ET" % (page_index + 1),
        b"0.2 0.2 0.2 rg 72 745 451 1 re f",
    ]
    y = 720
    for i in range(24):
        lines.append(b"BT /F1 9 Tf 72 %d Td (Line %d of body text for this page.) Tj ET" % (y, i + 1))
        y -= 12
    # pad to the requested size with a comment the lexer has to walk
    body = b"\n".join(lines) + b"\n%"
    target = kb * 1024
    if len(body) < target:
        body += b"p" * (target - len(body) - 2)
    return body + b"\n"


def build(pages: int, kb: int) -> bytes:
    out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = {}

    def obj(num: int, body: bytes):
        offsets[num] = len(out)
        out.extend(b"%d 0 obj\n" % num)
        out.extend(body)
        out.extend(b"\nendobj\n")

    # 1 catalog, 2 pages tree, 3 font, then 4.. pages+streams
    page_nums = [3 + 2 * i for i in range(pages)]
    obj(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    kids = b" ".join(b"%d 0 R" % n for n in page_nums)
    obj(2, b"<< /Type /Pages /Count %d /Kids [%s] /MediaBox [0 0 612 792] "
           b"/Resources << /Font << /F1 3 0 R >> >> >>" % (pages, kids))
    obj(3, b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")

    for i, num in enumerate(page_nums):
        stream_num = num + 1
        data = content_stream(i, kb)
        obj(num, b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                b"/Resources << /Font << /F1 3 0 R >> >> /Contents %d 0 R >>" % stream_num)
        obj(stream_num, b"<< /Length %d >>\nstream\n" % len(data) + data + b"endstream")

    xref_at = len(out)
    count = 3 + 2 * pages + 1
    out.extend(b"xref\n0 %d\n" % count)
    out.extend(b"0000000000 65535 f \n")
    for num in range(1, count):
        out.extend(b"%010d 00000 n \n" % offsets.get(num, 0))
    out.extend(b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (count, xref_at))
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pages", type=int, default=240)
    ap.add_argument("--kb", type=int, default=500)
    args = ap.parse_args()
    data = build(args.pages, args.kb)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(data)
    print(f"wrote {OUT.relative_to(ROOT)} — {len(data) / (1024 * 1024):.1f} MB, {args.pages} pages")


if __name__ == "__main__":
    main()
