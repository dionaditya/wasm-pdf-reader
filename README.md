# wasm-pdf-reader

A PDF reader in WebAssembly: it parses, rasterizes and extracts text from a PDF
inside the browser tab. No server, no upload, no CDN — a ~1.5 MB module that a
vanilla page or a React app can drive.

![The vanilla viewer showing the generated sample](docs/screenshots/vanilla.png)

The C++ core (lexer, xref, object streams, filters, content-stream interpreter,
scanline rasterizer, font subsetting) is the same code that powers the PDF tab
in [wasm-ui](https://github.com/dionaditya/wasm-ui), packaged here on its own.

## What it does

- **Parsing** — classic xref tables and xref streams, object streams, `/Prev`
  chains, incremental updates, xref rebuild for damaged files, Flate/LZW/
  ASCII85/ASCIIHex/RunLength with PNG and TIFF predictors.
- **Rendering** — paths, fills (nonzero/evenodd), strokes with dash/cap/join,
  clipping, text with the standard-14 metrics, Type0/Identity-H with embedded
  TrueType, Form and Image XObjects (Flate and baseline JPEG), inline images,
  `/Rotate` pages. Software rasterizer with 4× vertical supersampling, returning
  RGBA8.
- **Text layer** — glyph runs with page-space boxes, so you can select, copy,
  search and draw highlights without a second parser.
- **No network** — the wasm binary loads from your own origin; the file never
  leaves the tab.

Not supported (deliberately, not silently approximated): encryption, CFF/Type1
glyph programs (those fall back to a substituted face), JPX/JBIG2 images,
patterns, soft masks and annotations. A viewer can read `document.warnings` to
surface what was skipped.

## Install

```bash
npm install wasm-pdf-reader
```

ESM only. The wasm glue is a sibling of `pdf-reader.wasm`, so Vite, webpack and
esbuild pick up both automatically.

## Use it from plain JavaScript

```js
import { PdfDocument, mountPdfReader } from 'wasm-pdf-reader';
import 'wasm-pdf-reader/styles.css';

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
import { PdfReader } from 'wasm-pdf-reader/react';
import 'wasm-pdf-reader/styles.css';

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
| `pageOverlay` | `(page, { document, scale }) => node` — annotations, watermarks |
| `search` | a query; matching lines get highlight rectangles |
| `pixelTransform` | post-process the RGBA of every page (invert, tint, watermark) |
| `renderer` | swap the rendering backend entirely (`{ render(doc, page, opts) }`) |
| `renderLoading` / `renderError` / `renderEmpty` | replace the built-in states |

The hooks underneath are public: `usePdfDocument`, `usePdfPage`, `useInView`.
The core has no React dependency at all, and the components are just one way to
lay it out.

## API

```js
PdfDocument.open(source, { wasmUrl, wasmBinary, module, filename })
  .pageCount                       // number
  .metadata                        // { Title, Author, Subject, Producer, ... }
  .warnings                        // ["rebuilt xref", ...]
  .getPageSize(i)                  // { width, height, rotation, boxX0, boxY0 }
  .render(i, { scale, dpr, pixelTransform, stats })  // { imageData, width, height, ... }
  .renderToCanvas(canvas, i, opts)
  .getText(i)                      // [{ text, x0, y0, x1, y1, ox, oy, adv, fs }]
  .getPageText(i) / .getAllText()
  .search(query, { caseSensitive, limit, pages })    // [{ page, index, length, line, rects }]
  .boxToCss(box, i, scale)         // for overlays
  .close()
```

Full types ship with the package (`dist/index.d.ts`, `dist/react/index.d.ts`).

## Demo

```bash
npm install
node examples/build-react.mjs     # bundles the React demo
node examples/server.mjs          # http://localhost:5177/
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
npm test              # core: parse, geometry, raster, text, search (17 checks)
npm run test:e2e      # headless Chrome: demos, screenshots, pixel baseline
npm run test:pages    # the Pages build under a /wasm-pdf-reader/ base path
npm run test:visual   # rewrite the pixel baseline after an intended change
```

`test:unit` runs the real wasm module in Node against a generated fixture
(`scripts/make-fixture.mjs`: three pages with text, vector art, an image
XObject, clipping and a `/Rotate 90` page). `test:e2e` drives the vanilla and
React demos over CDP, asserts ink coverage and navigation, hashes each page
bitmap, and writes the screenshots in `docs/screenshots/`.

## Layout

```
src/cpp/            PDF reader and rasterizer (pdfdoc, pdfraster, C ABI)
src/js/             loader, PdfDocument, vanilla viewer, text helpers
src/js/react/       PdfReader, PdfPage, PdfToolbar, hooks
src/types/          hand-written .d.ts for both entry points
scripts/            fixture generator, JS build, demo build
examples/           vanilla and React demos + static server
test/               unit, e2e (CDP), Pages checks, fixtures, baseline
dist/               published output (ESM + wasm)
```

## Licence

MIT for this library. The embedded fallback faces are Liberation (SIL OFL),
Carlito and Caladea (SIL OFL); `stb_truetype.h` is public domain / MIT.
