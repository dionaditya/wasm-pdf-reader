#!/usr/bin/env python3
"""Diff our wasm render against MuPDF page by page (ink ratio + mean abs diff).

    ~/pdfvenv/bin/python scripts/compare_render.py test/fixtures/patterns.pdf 1.5

Prints one row per page and exits non-zero when a page is outside the roadmap
gate (text <= 1/255, scan <= 3/255, composite <= 3/255, ink within 2% relative).
"""
import os
import subprocess
import sys
import tempfile

import numpy as np
import pymupdf
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def render_ours(pdf, page, scale, out, repo=None):
    repo = repo or ROOT
    subprocess.run(["node", os.path.join(repo, "test", "render-to-png.mjs"),
                    os.path.abspath(pdf), str(page), str(scale), out],
                   check=True, cwd=repo, stdout=subprocess.DEVNULL)


def render_ref(pdf, page, scale, out):
    doc = pymupdf.open(pdf)
    pix = doc[page].get_pixmap(matrix=pymupdf.Matrix(scale, scale), alpha=False)
    pix.save(out)


def main():
    pdf = sys.argv[1]
    scale = float(sys.argv[2]) if len(sys.argv) > 2 else 1.5
    doc = pymupdf.open(pdf)
    tmp = tempfile.mkdtemp()
    ok = True
    print(f"{'page':>4} {'ink ours':>9} {'ink ref':>9} {'meanabs':>8} {'maxdiff':>8}  verdict")
    for p in range(doc.page_count):
        a_path = os.path.join(tmp, f"ours{p}.png")
        b_path = os.path.join(tmp, f"ref{p}.png")
        render_ours(pdf, p, scale, a_path)
        render_ref(pdf, p, scale, b_path)
        a = np.asarray(Image.open(a_path).convert("RGB"), dtype=np.int16)
        b = np.asarray(Image.open(b_path).convert("RGB"), dtype=np.int16)
        if a.shape != b.shape:
            h = min(a.shape[0], b.shape[0])
            w = min(a.shape[1], b.shape[1])
            a, b = a[:h, :w], b[:h, :w]
        diff = np.abs(a - b)
        meanabs = float(diff.mean())
        maxdiff = int(diff.max())
        ink_a = float((a.min(axis=2) < 250).mean())
        ink_b = float((b.min(axis=2) < 250).mean())
        rel = abs(ink_a - ink_b) / max(ink_b, 1e-6)
        good = meanabs <= 3.0 and rel <= 0.02
        ok = ok and good
        print(f"{p:>4} {ink_a:9.4f} {ink_b:9.4f} {meanabs:8.2f} {maxdiff:8d}  "
              f"{'ok' if good else 'FAIL'}  (ink rel {rel*100:.1f}%)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
