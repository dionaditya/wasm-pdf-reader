#!/usr/bin/env python3
"""make_outline_cycle_fixture.py — an outline tree that points at itself.

The MuPDF advisory GHSA for mupdf < 1.26.2 is an uncontrolled recursion in
`strip_outline`: a crafted `/Outlines` tree with cyclic `/Next` (and, in the
same shape, cyclic `/First`) references recurses forever and crashes the
process. This fixture reproduces the shape for our own outline walker, which is
independent code (`Document::outlineJson()` in src/cpp/pdfdoc.cpp).

Three cycles, all legal PDF object references and all cheap to parse:

  * `/First` self-reference on a child item  (recursion — the dangerous one)
  * `/Next` ring of two items                (bounded by the 5000-item guard)
  * a `/Pages` node that is its own /Kids entry (queue walk, no recursion)

Every page is otherwise empty, so a build that survives simply reports an
outline with a couple of bookmarks and page 1 blank.

Run: ~/pdfvenv/bin/python scripts/make_outline_cycle_fixture.py
Out:  test/fixtures/outline-cycle.pdf
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "test" / "fixtures" / "outline-cycle.pdf"

objects = {
    1: b"<< /Type /Catalog /Pages 2 0 R /Outlines 5 0 R >>",
    # /Kids points back at the node itself: a page-tree walk without a visited
    # set spins here instead of finishing.
    2: b"<< /Type /Pages /Kids [2 0 R 3 0 R] /Count 1 >>",
    3: b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] >>",
    5: b"<< /Type /Outlines /First 6 0 R /Last 8 0 R /Count 3 >>",
    # 6's /First is itself -> unbounded recursion in a naive walk
    6: b"<< /Title (cycle-first) /Parent 5 0 R /First 6 0 R /Next 7 0 R >>",
    # 7 <-> 8 is a /Next ring -> unbounded loop if the guard is missing
    7: b"<< /Title (cycle-next-a) /Parent 5 0 R /Next 8 0 R >>",
    8: b"<< /Title (cycle-next-b) /Parent 5 0 R /Next 7 0 R >>",
}

out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
offsets = {}
for num in sorted(objects):
    offsets[num] = len(out)
    out += f"{num} 0 obj\n".encode() + objects[num] + b"\nendobj\n"

xref = len(out)
count = max(objects) + 1
out += f"xref\n0 {count}\n".encode()
out += b"0000000000 65535 f \n"
for num in range(1, count):
    off = offsets.get(num, 0)
    if off:
        out += f"{off:010d} 00000 n \n".encode()
    else:
        out += b"0000000000 65535 f \n"

out += (
    f"trailer\n<< /Size {count} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n"
).encode()

OUT.parent.mkdir(parents=True, exist_ok=True)
OUT.write_bytes(bytes(out))
print(f"wrote {OUT.relative_to(ROOT)} ({len(out)} bytes)")
