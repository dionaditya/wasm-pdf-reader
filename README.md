# wasm-pdf-reader

A PDF reader in WebAssembly: it parses, rasterizes and extracts text from a PDF
inside the browser tab. No server, no upload, no CDN — a 2.9 MB module (1.8 MB
gzipped, 0.9 MB of it the Adobe CMap tables) that a vanilla page or a React app
can drive.

Packaged as `@dionaditya/wasm-pdf-reader`. Try it without installing anything:
[vanilla demo](https://dionaditya.github.io/wasm-pdf-reader/vanilla/) ·
[React demo](https://dionaditya.github.io/wasm-pdf-reader/react/).

![The vanilla viewer showing the generated sample](docs/screenshots/vanilla.png)

The C++ core (lexer, xref, object streams, filters, content-stream interpreter,
scanline rasterizer, font subsetting) is the same code that powers the PDF tab
in [wasm-ui](https://github.com/dionaditya/wasm-ui), packaged here on its own.

## What it does

- **Parsing** — classic xref tables and xref streams, object streams, `/Prev`
  chains, incremental updates, xref rebuild for damaged files, Flate/LZW/
  ASCII85/ASCIIHex/RunLength with PNG and TIFF predictors. Object-stream offsets
  are read relative to `/First`, which is how calibre, Acrobat and Ghostscript
  write them.
- **Encryption** — Standard security handler: RC4 40/128-bit, AES-128 and
  AES-256 (R5/R6). Pass the password as an option and it is tried as the user
  password first, then as the owner password (R2–R4 via /O, R5/R6 via /OE with
  /Perms verification):

  ```js
  const doc = await PdfDocument.open(bytes, { password: 'secret' });
  ```

  Files with no password open with `PdfDocument.open(bytes)`. A file that needs
  one fails with `encrypted PDF: a user password is required` (or
  `wrong password`), never a blank page. The demo prompts for the password once
  when the reader asks for it.

  *Verified against qpdf:* `scripts/make_encrypted_fixtures.py` writes one file
  per revision (R2 RC4-40, R4 RC4-128, R4 AES-128, R5 and R6 AES-256, plus an
  owner-password-only file and one with a corrupted `/Perms`), and
  `scripts/verify_encryption.sh` compares the file key this engine derives with
  `qpdf --show-encryption --show-encryption-key`. All thirteen user/owner rows
  match byte-for-byte. Two behaviours are deliberate: an owner-only file opens
  with an empty password (that *is* its user password), and a corrupt `/Perms`
  makes the owner path refuse with a warning naming the permissions block —
  qpdf ignores it and opens anyway; refusing is the safer read of a tampered
  file.
- **Rendering** — paths, fills (nonzero/evenodd), strokes with dash/cap/join,
  clipping, text, Form and Image XObjects, inline images, `/Rotate` pages,
  axial and radial shadings, mesh shadings (types 4/5, approximate), tiling patterns (`/PatternType 1`, cached per
  pattern and scale, clipped to the painted path), shading patterns, all 15 PDF
  blend modes, `/Group /Transparency` compositing with `/ca` and `/CA`,
  `/SMask` soft masks and `/Mask` stencil masks.
  Software rasterizer with 4× vertical supersampling, returning RGBA8.
  Images larger than the destination are box-averaged, so a 5000 px scan drawn
  at 600 px stays readable instead of aliasing.
- **Images** — Flate, DCT (JPEG), CCITT G3/G4 fax, JBIG2, and JPEG 2000 (see
  [Limits](#limits): the JPX path only handles some encoders).
  PDFium's JBIG2 and fax decoders are translated to C with `wasm2c` and linked
  into `pdf-reader.wasm`, so scans decode in the same pass as everything else —
  no second binary, no JS hand-off, no re-render. Stencil `/Mask` streams decode
  through the same path.
- **Colour** — DeviceGray/RGB/CMYK (CMYK through a 9^4 table calibrated against
  the reference renderer, because device CMYK has no profile of its own),
  ICCBased profiles (matrix/TRC and A2B0 LUTs), `/Separation` and `/DeviceN`
  spot inks through their tint transform (sampled, exponential or stitched
  function), `/Indexed`.
- **Fonts** — TrueType (`/FontFile2`), bare CFF and CID-keyed CFF, CFF inside
  an OpenType (`OTTO`) wrapper, and Type3 fonts whose glyphs are content
  streams
  (`/FontFile3 /Type1C`, `/CIDFontType0C`) through an own Type2 interpreter, and
  sfnt-wrapped `/OpenType`. CID fonts resolve glyphs through `/CIDToGIDMap` or
  the CFF charset, after the character code is turned into a CID by the font's
  `/Encoding`: `/Identity-H`, one of the 179 predefined Adobe CMaps
  (`/UniJIS-UCS2-H`, `/GBK-EUC-H`, `/90ms-RKSJ-H`, …) or an embedded CMap
  stream, including `/usecmap` inheritance and mixed 1/2-byte codespaces.
  Embedded subsets that ship without a `cmap` table (common for Arabic and CJK)
  are patched with a synthetic one instead of falling back to a substitute face.
  Advances come from `/W`, falling back to the face's `hmtx`.
- **Annotations** — link annotations (`/URI`, `/GoTo`, direct `/Dest` and named
  destinations) are exposed as data and rendered as clickable overlays; every
  other annotation is drawn from its `/AP` appearance stream, which is where
  signatures, stamps and meterai actually live. Markup annotations that ship
  without an appearance (the common case for highlight, underline, strikeout
  and squiggly) are synthesised from `/QuadPoints` and `/C`, including one mark
  per quad; `/Text` notes without an `/AP` icon get a note icon built from
  `/Rect`. An appearance stream always wins when present.
- **Optional content** — `/OCProperties` `/OFF`/`/ON` decide which OCGs paint;
  hidden `/OC ... BDC` sections and XObjects with an `/OC` entry are skipped.
- **Outline** — `document.outline` returns `[{title, page}]` for the bookmark
  tree.
- **Viewer chrome** — page rail with lazily rendered thumbnails and a print
  action that rasterizes every page at ~120 dpi into a detached sheet, prints it
  and discards it (no print stylesheet can reach into the host page).
- **Text layer** — glyph runs with page-space boxes, so you can select, copy,
  search and draw highlights without a second parser. The viewer mounts a
  transparent `.wpdf-textlayer` over the canvas (each run is a span, stretched
  to the width the engine measured), which makes drag-selection, Ctrl+C and the
  browser's own find-in-page work. Turn it off with `textLayer: false` if you
  only need pixels.
- **No network** — the wasm binary loads from your own origin; the file never
  leaves the tab.

Not supported, and reported through `document.warnings` rather than silently
approximated: text shaping for scripts that are not stored pre-shaped (Arabic,
Indic), Type3 fonts, mesh shadings 6–7 (patch meshes; 4/5 render but are not
yet verified against MuPDF), soft masks attached with `gs /SMask`
(a group's own `/ca` and `/BM` do work), non-isolated transparency groups
(they are composited isolated, which only differs when the group relies on the
page backdrop), vertical writing (`-V` CMaps paint horizontally),
outlines/bookmarks, form fields
and XFA, public-key encryption, and ranged/streamed loading (the whole file is
resident in wasm memory). Annotations render but are not interactive beyond
links: there is no panel for note text, and a highlight uses alpha rather than
a multiply blend, so overlapping marks darken instead of saturating.

## Rendered examples

Every image below is the engine's own output at scale 1 (72 dpi), not a
screenshot of another viewer.

**Annotations without appearance streams** — left: highlight (two quads),
underline, strikeout and squiggly painted from `/QuadPoints` + `/C`; right:
three `/Text` notes whose icon is built from `/Rect` alone:

![Highlight, underline, strikeout and squiggly painted from QuadPoints](docs/screenshots/feature-annotations.png)

**CJK through a predefined CMap** — `/Encoding /UniJIS-UCS2-H` mapping UCS-2
codes to Adobe-Japan1 CIDs, then `/CIDToGIDMap` to the embedded Noto subset.
Ink ratio 0.0047 against MuPDF's 0.0049, mean abs diff 0.08/255:

![日本語のテキスト rendered through UniJIS-UCS2-H](docs/screenshots/feature-cmaps.png)

**JBIG2 scan** — a 19th-century two-column page decoded from a JBIG2 stream
with a global symbol dictionary, drawn at 1:1:

![A scanned page decoded from JBIG2](docs/screenshots/feature-jbig2.png)

## Security notes

- **No `eval`, by construction.** Parsing and rasterizing are C++; no JavaScript
  is generated from document data, and the build sets `-s DYNAMIC_EXECUTION=0`
  so Emscripten cannot emit `eval`/`new Function` either. `dist/` contains
  neither, and neither appears as a string in `pdf-reader.wasm`.
- **CVE-2024-4367 (pdf.js `/FontMatrix` injection) does not apply.** That bug
  exists because pdf.js compiled glyph commands into JS source; we interpret
  Type 1/Type 2 charstrings in C++ and read `/FontMatrix` entries as doubles.
  Checked against the Codean Labs PoC: the file opens, renders, raises no
  warnings, and executes nothing.
- **Bounds instead of trust.** Page bitmaps are capped at 40 MP, images at
  20000 px per side, form nesting at depth 6, and every number read from the
  file goes through one `objNum` coercion. The failure mode for hostile input
  is a refused page, not code execution.

## Limits

- **The whole file is resident.** No ranged loading: the PDF sits in the wasm
  heap while it is open, plus the parsed objects and, per page, one RGBA bitmap.
  For text documents that is roughly the file size; scans add the decoded image.
  Measured: a 78 MB / 16-page PDF of full-page JPEGs opens and renders its first
  page in ~0.8 s on desktop.
- **The wasm heap only grows.** Emscripten cannot return memory to the OS, so the
  figure you see is a high-water mark for the module's lifetime; tearing the
  module down (`resetModule()`) frees it. `npm run test:memory` asserts it
  plateaus rather than climbing per render or per open/close. Measured on a
  JBIG2 sheet at 2x: 112 MB, no drift over rounds. The first JBIG2 image
  pre-grows the heap by 64 MB (the decoder's allocator is safest when it has
  headroom before a decode), and pages are decoded into one reusable RGBA
  buffer, so peak per render is about two page bitmaps, not one per page.
- **The heap is capped at 1 GiB** (`-sMAXIMUM_MEMORY=1073741824`). A 4 GiB
  ceiling looked harmless and broke phones: iOS refuses to grow a module that
  has asked to reserve that much address space, so allocation failed early with
  a misleading `out of memory`. Beyond ~1 GiB, shrink the file instead.
- **One page, 40 MP.** A page is rendered whole, so `scale` is bounded by pixel
  count: 40 MP is the ceiling (a 10000×10000 px page is refused). Failures say
  which limit they hit:
  - `render failed: page needs 10000x10000 px (100.0 MP) at this scale; the
    renderer caps a page at 40 MP`
  - `render failed: not enough memory for a 6000x8000 px page (183 MB); the wasm
    heap could not grow`
- **JPEG 2000 covers one encoder so far.** The bundled JPX decoder renders the
  pdf.js reference fixture correctly, but a codestream written by OpenJPEG —
  what Pillow and most converters emit — decodes to noise: a flat-colour
  64x48 image differs from MuPDF by 14/255 and a grey ramp by 74/255, while the
  same images as JPEG, CCITT or JBIG2 match to ~2/255. Fixable; until then do
  not trust `/JPXDecode` pages.
- **Encrypted files need their password up front.** There is no callback asking
  the handler for one mid-parse; pass `password` to `open()` (the demo prompts
  once and retries).

## Roadmap

[`ROADMAP.md`](ROADMAP.md) carries the ordered gap list to MuPDF-grade output —
ICC colour management (parser landed, not yet wired into every colour path),
mesh shadings, Separation/DeviceN, the JPEG 2000 decoder, Type3/Type1 fonts,
OCG and forms — each with its fixture, acceptance numbers, touchpoints and the non-goals
(shaping, writing, OCR, other formats) that are deliberately out of scope.

## Install

```bash
npm install @dionaditya/wasm-pdf-reader
```

The registry carries `0.1.0`. This repository is the source of truth and is at
`0.2.0` (annotation fallbacks, CJK CMaps, working links); `npm publish` pushes it.
To track the repository instead of the registry, install from a commit — the
built `dist/`, wasm included, is committed, so nothing is compiled on install:

```bash
npm install github:dionaditya/wasm-pdf-reader
```

ESM only. The wasm glue is a sibling of `pdf-reader.wasm`, so Vite, webpack and
esbuild pick up both automatically.

## Use it from plain JavaScript

```js
import { PdfDocument, mountPdfReader } from '@dionaditya/wasm-pdf-reader';
import '@dionaditya/wasm-pdf-reader/styles.css';

// 1. the whole viewer, toolbar included
const viewer = mountPdfReader('#viewer', {
  file: '/contracts/lease.pdf',   // ArrayBuffer, Blob, File, URL or Response
  fit: 'width',                   // 'width' | 'page' | 'none'
  theme: { bg: '#0b0f19', accent: '#7dd3fc' },
});
viewer.goToPage(2);
viewer.search('deposit');

// 2. or just the pixels, and draw them yourself
const doc = await PdfDocument.open(file);
const { imageData } = await doc.render(0, { scale: 2, dpr: devicePixelRatio });
const text = doc.getPageText(0);
doc.close();
```

![Search highlights](docs/screenshots/search.png)

## Use it from React

```jsx
import { PdfReader } from '@dionaditya/wasm-pdf-reader/react';
import '@dionaditya/wasm-pdf-reader/styles.css';

export default function Preview({ file }) {
  return (
    <PdfReader
      file={file}
      fit="width"
      onPageChange={(page) => console.log(page)}
      toolbar={(api) => <MyToolbar {...api} />}
    />
  );
}
```

![The React demo: custom toolbar, page overlay, sidebar reading the text layer](docs/screenshots/react.png)

### What you can change

| prop | what it does |
|---|---|
| `file` | document source: `ArrayBuffer`, `Uint8Array`, `Blob`/`File`, URL, `Response` |
| `page` / `defaultPage` | controlled or uncontrolled page (zero-based) |
| `scale` / `defaultScale` / `fit` | controlled zoom, or `fit="width" \| "page"` |
| `continuous` | stack all pages and render them lazily (IntersectionObserver) |
| `toolbar` | `false`, a node, or `(api) => node` |
| `components` | replace `Page` and `Toolbar` with your own components |
| `theme` | CSS variables: `bg`, `toolbarBg`, `accent`, `radius`, `pageShadow`, … |
| `pageOverlay` | `(page, { document, scale }) => node` — your own overlays |
| `onLink` | `(link, { document, goToPage, pageCount }) => boolean \| void` — link clicks; return `false` to keep the default |
| `search` | a query; matching lines get highlight rectangles |
| `pixelTransform` | post-process the RGBA of every page (invert, tint, watermark) |
| `renderer` | swap the rendering backend entirely (`{ render(doc, page, opts) }`) |
| `renderLoading` / `renderError` / `renderEmpty` | replace the built-in states |

The hooks underneath are public: `usePdfDocument`, `usePdfPage`, `useInView`.
The core has no React dependency at all, and the components are just one way to
lay it out.

## Embedding in a host app

The bundle is plain ESM plus one `.wasm` sibling, so a host that ships static
assets can vendor it with no build step:

- **ownCloud Classic** — [`dionaditya/owncloud_files_pdfviewer`](https://github.com/dionaditya/owncloud_files_pdfviewer)
  (branch `feat/replace-pdfjs`) replaces the PDF.js viewer with this engine. The
  route, the `?file=` contract and the CSP stay the app's; parsing, rasterizing,
  text and annotations are the engine's. The bundle is vendored under
  `js/vendor/wasm-pdf-reader/` with a `VERSION.json` recording this repo's commit
  and a per-file sha256, refreshed by `npm run sync-viewer` (see that directory's
  README). `scripts/sync-viewer.mjs` there is the template for any host that
  wants "pinned, traceable, no install-time build".
- **Any page** — `mountPdfReader('#viewer', { file: url })` for the full viewer,
  or `PdfDocument.open(bytes)` for pixels and text only. Serve
  `dist/wasm/pdf-reader.wasm` beside `dist/loader.js`; a CSP needs
  `script-src 'wasm-unsafe-eval'`.

## API

```js
PdfDocument.open(source, { wasmUrl, wasmBinary, module, filename, password })
  .pageCount                       // number
  .metadata                        // { Title, Author, Subject, Producer, ... }
  .warnings                        // ["rebuilt xref", ...]
  .getPageSize(i)                  // { width, height, rotation, boxX0, boxY0 }
  .render(i, { scale, dpr, pixelTransform, stats })  // { imageData, width, height, ... }
  .renderToCanvas(canvas, i, opts)
  .getText(i)                      // [{ text, x0, y0, x1, y1, ox, oy, adv, fs }]
  .getPageText(i) / .getAllText()
  .getLinks(i)                     // [{ x0, y0, x1, y1, uri?, page? }]
  .search(query, { caseSensitive, limit, pages })    // [{ page, index, length, line, rects }]
  .boxToCss(box, i, scale)         // for overlays
  .close()
```

Full types ship with the package (`dist/index.d.ts`, `dist/react/index.d.ts`).

## Demo

```bash
npm install
node examples/build-react.mjs     # bundles the React demo
node examples/server.mjs 4444      # http://localhost:4444/
```

- `http://localhost:5177/examples/vanilla/`
- `http://localhost:5177/examples/react/`

The GitHub Pages build (`npm run build:demo` → `_site/`) serves the same demos
under a sub-path; `.github/workflows/pages.yml` publishes it on every push.

![Three pages of the generated fixture, rendered by the library](docs/screenshots/pages.png)

## Build from source

Requirements: [emsdk](https://emscripten.org/docs/getting_started/downloads.html)
on `PATH` and Node 18+.

```bash
. ~/emsdk/emsdk_env.sh
npm install
./build.sh            # C++ -> dist/wasm/pdf-reader.mjs + pdf-reader.wasm
npm run build:js      # src/js -> dist/ (ESM, plus JSX -> JS for React)
```

`dist/wasm` is committed, so `npm install` and CI work without emsdk; rerun
`./build.sh` only when the C++ changes.

## Tests

```bash
npm test              # core: parse, raster, text, search, links, annotations, CMaps, encryption (43 checks)
npm run test:e2e      # headless Chrome: demos, screenshots, pixel baseline
npm run test:pages    # the Pages build under a /wasm-pdf-reader/ base path
npm run test:memory   # soak the heap: render, reopen, extract; assert it plateaus
scripts/verify_encryption.sh   # keys vs qpdf for every revision (needs qpdf)
npm run test:visual   # rewrite the pixel baseline after an intended change
```

`test:unit` runs the real wasm module in Node against generated fixtures
(including encrypted RC4/AES-128/AES-256 files — with and without passwords —
and a JBIG2 scan) plus:
`scripts/make-fixture.mjs` (three pages with text, vector art, an image XObject,
clipping and a `/Rotate 90` page) and `scripts/make-objstm-fixture.mjs`, a
calibre-shaped document whose catalog, page tree and font live in an object
stream with a non-zero `/First` and whose xref is an `/XRef` stream with a PNG
predictor. `test:e2e` drives the vanilla and React demos over CDP, asserts ink
coverage and navigation, hashes each page bitmap, and writes the screenshots in
`docs/screenshots/`.

## Layout

```
src/cpp/            reader + rasterizer, CFF/Type2, JPEG 2000, annotations, C ABI
src/cpp/jbig2gen/   PDFium JBIG2/fax decoder, generated from wasm by wasm2c
src/js/             loader, PdfDocument, vanilla viewer, text helpers
src/js/react/       PdfReader, PdfPage, PdfToolbar, hooks
src/types/          hand-written .d.ts for both entry points
scripts/            fixture generator, JS build, demo build
examples/           vanilla and React demos + static server
test/               unit, e2e (CDP), Pages checks, fixtures, baseline
dist/               published output (ESM + wasm)
```

## Licence

MIT for this library. The engine's ~6,500 first-party lines sit on large vendored
and generated components: PDFium's JBIG2/fax (Apache-2.0), nanopdf's JPEG 2000
and CFF tables (Apache-2.0), `stb_truetype`/`stb_image` (MIT / public domain),
wabt (BSD-2), `simde` (MIT), Adobe's CMaps (BSD-3), and eight metric-compatible
font families — DejaVu (Bitstream Vera licence) and Liberation/Carlito/Caladea
(SIL OFL-1.1, subsetted, so Modified Versions).

[THIRD-PARTY.md](THIRD-PARTY.md) maps every file to its licence and states the
split by line count; the full OFL-1.1 and Bitstream Vera texts are in
`LICENSES/`, because those fonts ship inside `pdf-reader.wasm`.
