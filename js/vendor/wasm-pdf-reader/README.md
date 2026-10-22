# wasm-pdf-reader (vendored)

Prebuilt viewer bundle used by `/apps/files_pdfviewer/wasm`. Nothing here is
compiled at install time: the WebAssembly module is checked in.

## What it is

A PDF parser and rasterizer written in C++ and compiled to WebAssembly, plus a
vanilla-JS viewer on top of it. It replaces the parsing and rendering half of the
PDF.js integration; the Files app integration, the route and the `?file=`
contract are unchanged.

Properties that matter for this app:

- No JavaScript is generated from document data — charstrings and font matrices
  are interpreted in C++ — and the module is built with
  `-sDYNAMIC_EXECUTION=0`, so `eval`/`new Function` do not exist in the shipped
  artifacts. The page keeps `unsafe-eval` disabled in its CSP.
- One `pdf-reader.wasm` (1.8 MB, 0.9 MB gzipped). JBIG2 scans use a separate
  104 KB module that is fetched only when a document contains one.
- Supports RC4/AES-128/AES-256 encrypted files (password passed by the host),
  CFF/CID fonts, JPEG 2000, JBIG2, CCITT fax, SMasks, annotations and links.

## Files

| file | purpose |
| --- | --- |
| `index.js` | entry point, re-exports `PdfDocument`, `mountPdfReader` |
| `mount.js`, `document.js`, `loader.js`, `text.js` | viewer, document API, wasm loader, text layer |
| `styles.css` | viewer styles |
| `wasm/pdf-reader.{mjs,wasm}` | the engine |
| `jbig2/` | optional JBIG2 decoder (Apache-2.0, from PDF.js) |

## Rebuilding

From the upstream project (`https://github.com/dionaditya/wasm-pdf-reader`):

```bash
. ~/emsdk/emsdk_env.sh && ./build.sh && npm run build:js
cp dist/{index.js,document.js,loader.js,mount.js,text.js,styles.css,jbig2.js} .
cp -r dist/wasm dist/jbig2 .
```

Requires Emscripten at release time only.

## Licence

MIT for the engine and viewer, with Apache-2.0 (PDF.js JBIG2, PDFium fax) and
BSD-3-Clause (nanopdf JPEG 2000 and CFF tables) components; see
`THIRD-PARTY.md` / the `LICENSE_*` files next to the modules.
