#!/usr/bin/env python3
"""Build the encryption fixtures under test/fixtures/.

    ~/pdfvenv/bin/python scripts/make_encrypted_fixtures.py

One small text page re-saved with the Standard security handler at every
revision the reader claims to support, with a user and an owner password:

    enc-r2-rc4.pdf      R2, RC4 40-bit   (user + owner)
    enc-r5-aes256.pdf   R5, AES-256      (user + owner)
    enc-r4-rc4.pdf      R4, RC4 128-bit  (user + owner)
    enc-r4-aes128.pdf   R4, AES-128      (user + owner)
    enc-r6-aes256.pdf   R6, AES-256      (user + owner)
    enc-r6-aes256-user-only.pdf   R6, owner password only — a viewer must
                                  refuse an empty user password
    enc-r6-aes256-badperms.pdf    R6 with one byte of /Perms flipped — the
                                  owner path has to notice, not decrypt garbage

Passwords are `reader-fixture` (user) and `owner-fixture` (owner). The derived
file keys are checked against `qpdf --show-encryption-key` by
`test/enc_probe.cpp`; see README "Encryption, verified".
"""
import pikepdf
from pikepdf import Name, String
from pathlib import Path

OUT = Path("test/fixtures")
USER, OWNER = "reader-fixture", "owner-fixture"


def base():
    pdf = pikepdf.new()
    font = pdf.make_indirect(pikepdf.Dictionary(
        Type=Name.Font, Subtype=Name.Type1, BaseFont=Name.Helvetica,
        Encoding=Name.WinAnsiEncoding))
    page = pdf.add_blank_page(page_size=(300, 200))
    page.Resources = pikepdf.Dictionary(Font=pikepdf.Dictionary(F1=font))
    page.Contents = pdf.make_stream(b"BT /F1 14 Tf 20 100 Td (Encryption fixture) Tj ET")
    return pdf


def save(name, **kw):
    pdf = base()
    pdf.save(OUT / name, **kw)
    print("wrote", OUT / name)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    save("enc-r2-rc4.pdf", encryption=pikepdf.Encryption(
        owner=OWNER, user=USER, R=2, aes=False, metadata=False))
    save("enc-r4-rc4.pdf", encryption=pikepdf.Encryption(
        owner=OWNER, user=USER, R=4, aes=False, metadata=False))
    save("enc-r4-aes128.pdf", encryption=pikepdf.Encryption(
        owner=OWNER, user=USER, R=4, aes=True))
    save("enc-r5-aes256.pdf", encryption=pikepdf.Encryption(
        owner=OWNER, user=USER, R=5))    # deprecated revision, still in the wild
    save("enc-r6-aes256.pdf", encryption=pikepdf.Encryption(
        owner=OWNER, user=USER, R=6))
    save("enc-r6-aes256-user-only.pdf", encryption=pikepdf.Encryption(
        owner=OWNER, user="", R=6))

    # /Perms exists to prove the *owner* path derived the right key: it is the
    # 16-byte encrypted permissions block, checked for the marker "adb" at bytes
    # 9..11. Flip one hex digit of it in place — same length, so every xref
    # offset stays valid — and the owner path must refuse rather than decrypt
    # garbage. The user path does not consult /Perms, so it must still open.
    src = (OUT / "enc-r6-aes256.pdf").read_bytes()
    at = src.index(b"/Perms <")
    digit = at + len(b"/Perms <")
    original = src[digit:digit + 1]
    flipped = b"b" if original != b"b" else b"c"
    (OUT / "enc-r6-aes256-badperms.pdf").write_bytes(
        src[:digit] + flipped + src[digit + 1:])
    print("wrote", OUT / "enc-r6-aes256-badperms.pdf", "(one hex digit of /Perms flipped)")


if __name__ == "__main__":
    main()
