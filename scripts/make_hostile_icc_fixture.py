#!/usr/bin/env python3
"""Generate test/fixtures/hostile-icc-n.pdf — an image XObject whose /ICCBased
profile dict claims an absurd component count.

Why: MuPDF's CVE-2026-3308 (integer overflow in pdf_load_image_imp, pdf-image.c,
VU#951662) is the same bug class as our old image size guard. `drawImage`
computed

    size_t expected = (size_t)iw * ih * ncomp * bpc / 8;

with `ncomp` taken straight from /ICCBased /N. size_t is 32 bits under wasm, so
a chosen /N wraps `expected` down to 1024 bytes: a 2 KB image stream sails past
the "image data short" guard and sample() indexes far outside the buffer. The
pre-fix module traps with "memory access out of bounds" on this 2.8 KB file.

/N 2690593 is picked so that 20000 * 20000 * 2690593 * 8 / 8 == 1024 (mod 2^32)
while staying below INT_MAX, so the (int) cast is exact on every backend.

Run: python3 scripts/make_hostile_icc_fixture.py
"""
import pathlib

N = 2690593
STREAM_BYTES = 2048          # >= the wrapped guard value (1024), so it passes
OUT = pathlib.Path(__file__).resolve().parent.parent / 'test' / 'fixtures' / 'hostile-icc-n.pdf'

content = b"q 200 0 0 200 0 0 cm /Im0 Do Q"
img = b"\x00" * STREAM_BYTES
prof = b"not-an-icc"          # under 132 bytes: iccParse bails, so /N survives

assert (20000 * 20000 * N * 8 // 8) % (2 ** 32) == 1024
assert N < 2 ** 31

objs = [
    b"<< /Type /Catalog /Pages 2 0 R >>",
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
    b"/Resources << /XObject << /Im0 5 0 R >> >> /Contents 4 0 R >>",
    b"<< /Length %d >>\nstream\n%s\nendstream" % (len(content), content),
    b"<< /Type /XObject /Subtype /Image /Width 20000 /Height 20000 "
    b"/BitsPerComponent 8 /ColorSpace [/ICCBased 6 0 R] /Length %d >>\nstream\n%s\nendstream"
    % (len(img), img),
    b"<< /N %d /Length %d >>\nstream\n%s\nendstream" % (N, len(prof), prof),
]

out = bytearray(b"%PDF-1.7\n")
offs = []
for i, body in enumerate(objs, 1):
    offs.append(len(out))
    out += b"%d 0 obj\n" % i + body + b"\nendobj\n"
xref = len(out)
out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
for o in offs:
    out += b"%010d 00000 n \n" % o
out += (b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n"
        % (len(objs) + 1, xref))

OUT.write_bytes(bytes(out))
print(f"wrote {OUT} ({len(out)} bytes)")
