#!/usr/bin/env python3
"""Build test/fixtures/cjk-cmaps.pdf: Type0 fonts encoded by real CMaps.

    ~/pdfvenv/bin/python scripts/make_cjk_fixture.py \
        --font /tmp/NotoSansJP.ttf --cmaps /tmp/cmapsrc

Page 1  /Encoding /UniJIS-UCS2-H (predefined table) with a subset of Noto Sans JP
        and a /CIDToGIDMap stream, the way Acrobat/Word write CJK.
Page 2  /Encoding is an embedded CMap *stream* that starts with
        `/UniJIS-UCS2-H usecmap` and overrides three codes.
Page 3  /Encoding is an embedded CMap with mixed 1-byte and 2-byte codespaces,
        which is how the legacy RKSJ/EUC tables look.

The font is a build input, not a repo asset: only the PDF it produces is
committed (a Noto CJK face is 9 MB). MuPDF renders all three pages correctly,
so its output is the reference for the renderer.
"""
import argparse
import io
import re
import zlib
from pathlib import Path

import pikepdf
from pikepdf import Array, Dictionary, Name, String
from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

W, H = 612, 792
# code points used by the fixtures; the CID for each comes from UniJIS-UCS2-H
PAGE1_TEXT = "日本語のテキスト"
EXTRA_CHARS = "あ漢語の日は"


def parse_cmap(path):
    """UniJIS-UCS2-H -> {code: cid}."""
    text = path.read_text("latin-1")
    table = {}
    for block in re.findall(r"begincidrange(.*?)endcidrange", text, re.S):
        for lo, hi, cid in re.findall(r"<([0-9A-Fa-f]+)>\s*<([0-9A-Fa-f]+)>\s*(\d+)", block):
            lo, hi, cid = int(lo, 16), int(hi, 16), int(cid)
            for code in range(lo, hi + 1):
                table[code] = cid + (code - lo)
    return table


def build_subset(font_path, chars):
    font = TTFont(font_path)
    if "fvar" in font:
        instantiateVariableFont(font, {"wght": 400}, inplace=True)
    opts = subset.Options()
    opts.notdef_outline = True
    opts.glyph_names = False
    opts.layout_features = []
    opts.name_IDs = ["*"]
    opts.recalc_bounds = True
    s = subset.Subsetter(options=opts)
    s.populate(unicodes={ord(c) for c in chars})
    s.subset(font)
    cmap = font.getBestCmap()
    upem = font["head"].unitsPerEm
    gid = {ch: font.getGlyphID(cmap[ord(ch)]) for ch in chars}
    adv = {ch: font["hmtx"][cmap[ord(ch)]][0] for ch in chars}
    buf = io.BytesIO()
    font.save(buf)
    return buf.getvalue(), gid, {c: round(a * 1000 / upem) for c, a in adv.items()}, upem


def cid_to_gid_stream(gid_by_char, cid_by_char):
    top = max(cid_by_char.values())
    data = bytearray(2 * (top + 1))
    for ch, cid in cid_by_char.items():
        g = gid_by_char[ch]
        data[2 * cid:2 * cid + 2] = bytes([(g >> 8) & 0xFF, g & 0xFF])
    return bytes(data)


def make_font(pdf, subset_bytes, widths_by_cid, map_bytes, encoding, tounicode=None):
    # pikepdf writes the bytes verbatim, so compress here before tagging the
    # stream: marking raw TTF data /FlateDecode makes every reader choke.
    ff2 = pdf.make_stream(zlib.compress(subset_bytes, 9))
    ff2.Filter = Name.FlateDecode
    fd = pdf.make_indirect(Dictionary(
        Type=Name.FontDescriptor, FontName=Name("/NotoSansJP-Subset"), Flags=4,
        FontBBox=Array([-1000, -350, 2900, 1200]), ItalicAngle=0,
        Ascent=1160, Descent=-288, CapHeight=733, StemV=80,
        FontFile2=ff2,
    ))
    cidfont = pdf.make_indirect(Dictionary(
        Type=Name.Font, Subtype=Name.CIDFontType2, BaseFont=Name("/NotoSansJP-Subset"),
        CIDSystemInfo=Dictionary(Registry=String("Adobe"), Ordering=String("Japan1"), Supplement=7),
        FontDescriptor=fd, DW=1000,
        W=Array([x for cid, w in sorted(widths_by_cid.items()) for x in (cid, Array([w]))]),
        CIDToGIDMap=pdf.make_stream(map_bytes),
    ))
    d = Dictionary(
        Type=Name.Font, Subtype=Name.Type0,
        BaseFont=Name("/NotoSansJP-Subset"),
        DescendantFonts=Array([cidfont]),
    )
    if isinstance(encoding, str):
        d.Encoding = Name(encoding)
    else:
        d.Encoding = pdf.make_stream(encoding)
    if tounicode:
        d.ToUnicode = pdf.make_stream(tounicode)
    return pdf.make_indirect(d)


def tounicode_for(codes):
    body = "\n".join(f"<{c:04X}> <{c:04X}>" for c in sorted(codes))
    return (
        "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
        "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
        "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n"
        f"{len(codes)} beginbfchar\n{body}\nendbfchar\n"
        "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n"
    ).encode("latin-1")


EMBED_CMAP = """%!PS-Adobe-3.0 Resource-CMap
%%DocumentNeededResources: ProcSet (CIDInit)
/CIDInit /ProcSet findresource begin
12 dict begin
begincmap
/CIDSystemInfo << /Registry (Adobe) /Ordering (Japan1) /Supplement 7 >> def
/CMapName /Custom-UCS2-H def
/CMapType 1 def
/UniJIS-UCS2-H usecmap
1 begincodespacerange
<0000> <FFFF>
endcodespacerange
1 begincidrange
<E041> <E041> {base}
endcidrange
1 begincidchar
<E042> {second}
endcidchar
endcmap
CMapName currentdict /CMap defineresource pop
end
end
"""

MIXED_CMAP = """%!PS-Adobe-3.0 Resource-CMap
/CIDInit /ProcSet findresource begin
12 dict begin
begincmap
/CIDSystemInfo << /Registry (Adobe) /Ordering (Japan1) /Supplement 7 >> def
/CMapName /Custom-Mixed-H def
/CMapType 1 def
2 begincodespacerange
<20> <7E>
<8140> <9FFC>
endcodespacerange
1 begincidrange
<8140> <8141> {base}
endcidrange
1 begincidchar
<41> {single}
endcidchar
endcmap
CMapName currentdict /CMap defineresource pop
end
end
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", default="/tmp/NotoSansJP.ttf")
    ap.add_argument("--cmaps", default="/tmp/cmapsrc")
    ap.add_argument("--out", default="test/fixtures/cjk-cmaps.pdf")
    args = ap.parse_args()

    table = parse_cmap(Path(args.cmaps) / "Adobe-Japan1-7" / "CMap" / "UniJIS-UCS2-H")
    chars = PAGE1_TEXT + EXTRA_CHARS
    subset_bytes, gid, width1000, _ = build_subset(args.font, chars)
    cid_by_char = {c: table[ord(c)] for c in chars}
    widths_by_cid = {cid_by_char[c]: width1000[c] for c in chars}
    map_bytes = cid_to_gid_stream(gid, cid_by_char)

    pdf = pikepdf.new()
    f1 = make_font(pdf, subset_bytes, widths_by_cid, map_bytes, "/UniJIS-UCS2-H",
                   tounicode_for([ord(c) for c in PAGE1_TEXT]))
    # page 2: inheritance for あ (U+3042), overrides for 漢/語
    f2 = make_font(pdf, subset_bytes, widths_by_cid, map_bytes,
                   EMBED_CMAP.format(base=cid_by_char["漢"],
                                     second=cid_by_char["語"]).encode("latin-1"))
    # page 3: 1-byte code 0x41 -> 日, 2-byte <8141> -> の
    f3 = make_font(pdf, subset_bytes, widths_by_cid, map_bytes,
                   MIXED_CMAP.format(base=cid_by_char["の"], single=cid_by_char["日"]).encode("latin-1"))

    def hex_text(s):
        return "".join(f"{ord(c):04X}" for c in s)

    # Captions use a plain Helvetica: the CJK fonts are deliberately restricted
    # to the glyphs their CMaps reach, so drawing Latin through them would only
    # produce .notdef boxes and confuse the visual check.
    helv = pdf.make_indirect(Dictionary(
        Type=Name.Font, Subtype=Name.Type1, BaseFont=Name.Helvetica,
        Encoding=Name.WinAnsiEncoding))

    pages = [
        (f1, f"BT /F1 24 Tf 72 700 Td <{hex_text(PAGE1_TEXT)}> Tj ET\n"
             "BT /Helv 12 Tf 72 660 Td (predefined CMap: /UniJIS-UCS2-H) Tj ET"),
        (f2, "BT /F2 24 Tf 72 700 Td <3042> Tj <E041> Tj <E042> Tj ET\n"
             "BT /Helv 12 Tf 72 660 Td (embedded CMap: usecmap inherits U+3042, then overrides E041/E042) Tj ET"),
        (f3, "BT /F3 24 Tf 72 700 Td (A) Tj <8141> Tj ET\n"
             "BT /Helv 12 Tf 72 660 Td (embedded CMap: one-byte 41, two-byte 8141) Tj ET"),
    ]
    for font, content in pages:
        p = pdf.add_blank_page(page_size=(W, H))
        p.Resources = Dictionary(Font=Dictionary(F1=font, F2=font, F3=font, Helv=helv))
        p.Contents = pdf.make_stream(content.encode("latin-1"))

    pdf.save(args.out)
    print("wrote", args.out)
    for c in PAGE1_TEXT:
        print(f"  {c} U+{ord(c):04X} -> CID {cid_by_char[c]} -> GID {gid[c]}")


if __name__ == "__main__":
    main()
