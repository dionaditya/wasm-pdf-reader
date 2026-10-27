#!/usr/bin/env python3
"""make_sparse_pdf.py — a valid PDF just under 4 GiB that costs no disk.

"Can it open a 4 GB PDF?" is an offset question, not a heap question: the ranged
reader never holds the file, but every offset it uses has to survive the trip
through wasm32 (32-bit `size_t`) and through the JS bridge (one i64 parameter
would force BigInt on every read, so the offset crosses as two i32 halves).

A 4 GiB file of real bytes is neither necessary nor practical, so this writes a
*sparse* one: the header sits at 0, every real object sits in the tail, and the
hole between them is never allocated on disk (`seek` + write → SEEK_HOLE).

The point is where the objects land: xref entries around 2^32 - 1 wake up every
truncation bug (a 32-bit `long` xref offset, `(int)off` in the bridge, `atol` on
the startxref value) which a 120 MB fixture cannot reach.

Run: ~/pdfvenv/bin/python scripts/make_sparse_pdf.py [--size-mib 4095]
Output: test/fixtures/huge-sparse.pdf (gitignored)
"""

import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "test" / "fixtures" / "huge-sparse.pdf"

CONTENT = b"""BT /F1 24 Tf 72 720 Td (Sparse 4 GiB fixture) Tj ET
0.1 0.1 0.1 rg 72 700 468 2 re f
BT /F1 12 Tf 72 680 Td (Offsets near the end of a four-billion-byte file) Tj ET
"""


def objects():
    """Object bodies in file order; 0 is the xref offset placeholder."""
    return [
        b"<</Type/Catalog/Pages 2 0 R>>",
        b"<</Type/Pages/Kids[3 0 R]/Count 1>>",
        b"<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]"
        b"/Resources<</Font<</F1 5 0 R>>>>/Contents 4 0 R>>",
        b"<</Length %d>>\nstream\n" % len(CONTENT) + CONTENT + b"endstream",
        b"<</Type/Font/Subtype/Type1/BaseFont/Helvetica/Encoding/WinAnsiEncoding>>",
    ]


def build_tail(tail_start: int):
    """Return (bytes, offsets) for the tail, with absolute xref offsets."""
    out = bytearray()
    offsets = {}
    for i, body in enumerate(objects(), start=1):
        offsets[i] = tail_start + len(out)
        out += b"%d 0 obj\n" % i + body + b"\nendobj\n"
    xref_at = tail_start + len(out)
    out += b"xref\n0 6\n"
    out += b"0000000000 65535 f \n"
    for i in range(1, 6):
        out += b"%010d 00000 n \n" % offsets[i]
    out += b"trailer\n<</Size 6/Root 1 0 R>>\nstartxref\n%d\n%%%%EOF\n" % xref_at
    return bytes(out), offsets, xref_at


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--size-mib", type=int, default=4095,
                    help="total file size in MiB (default 4095 = 4 GiB - 1 MiB)")
    args = ap.parse_args()
    size = args.size_mib * 1024 * 1024

    header = b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n"
    tail_start = size - 4096          # converged below; must leave room
    for _ in range(4):
        tail, offsets, xref_at = build_tail(tail_start)
        tail_start = size - len(tail)
    tail, offsets, xref_at = build_tail(tail_start)
    assert tail_start + len(tail) == size, (tail_start, len(tail), size)

    OUT.parent.mkdir(parents=True, exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(header)
        f.seek(size - 1)              # hole, not data
        f.write(b"\n")
        f.seek(tail_start)
        f.write(tail)

    print(f"{OUT}  {size} bytes ({size / 2**30:.3f} GiB, sparse)")
    print(f"  tail starts at {tail_start} (2 GiB + {(tail_start - 2**31) / 2**20:.1f} MiB)")
    print(f"  page object 3 at {offsets[3]}, xref at {xref_at}, startxref value {xref_at}")


if __name__ == "__main__":
    main()
