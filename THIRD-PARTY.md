# Third-party code

| Component | File | Licence | Origin |
|---|---|---|---|
| stb_image | `src/cpp/stb_image.h` | MIT / Public Domain | Sean Barrett, https://github.com/nothings/stb |
| stb_truetype | `src/cpp/stb_truetype.h` | MIT / Public Domain | Sean Barrett, https://github.com/nothings/stb |
| CCITT G3/G4 fax decoder | `src/cpp/ccitt-decoder.hh` | BSD-3-Clause | The PDFium Authors / Foxit Software Inc., vendored via https://github.com/lighttransport/nanopdf |
| JPEG 2000 decoder | `src/cpp/jpx-decoder.{cc,hh}` | Apache-2.0 | Light Transport Entertainment Inc. (nanopdf) |
| CFF standard strings / encodings | `src/cpp/cff-parser.{cc,hh}` | Apache-2.0 | Light Transport Entertainment Inc. (nanopdf) |
| JBIG2 decoder | `src/js/jbig2/`, `dist/jbig2/` | Apache-2.0 | Mozilla `pdf.js.jbig2` — a wasm build of PDFium's JBIG2 codec; licence texts ship alongside the binary |

The BSD-3-Clause licence of the fax decoder requires that its copyright notice,
conditions and disclaimer be retained in source distributions — they are kept in
the header of `src/cpp/ccitt-decoder.hh`. Apache-2.0 components keep their
notices in place.

## Notes

- The CFF *interpreter* (`src/cpp/cfffont.cpp`) is first-party; only the
  standard-string and encoding tables come from nanopdf.
- `jbig2.wasm` is vendored unmodified from `mozilla/pdf.js`, `external/jbig2/`
  (built from https://github.com/mozilla/pdf.js.jbig2). It loads as a separate
  module via `src/js/jbig2.js` and is not linked into our wasm.
