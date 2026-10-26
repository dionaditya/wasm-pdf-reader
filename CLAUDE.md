# CLAUDE.md — working notes for Claude Code

Read this before touching anything. It is the operating manual for this
repository: the build/test loop, a brief on every file, the invariants that keep
the engine honest, and how to finish a milestone so the next session does not
have to rediscover it.

Project in one paragraph: `@dionaditya/wasm-pdf-reader` is a PDF parser,
rasterizer and text extractor written in C++ and compiled to WebAssembly with
Emscripten, plus a framework-free JS viewer and a React binding. No server, no
network, no `eval`. `dist/` (JS **and** `dist/wasm/*.wasm`) is committed, so the
package installs without a compiler; `./build.sh` regenerates it.

Read in this order: `README.md` (capabilities, limits, comparison) → `ROADMAP.md`
(what is left, with acceptance numbers) → this file (how to work) →
`docs/INTEGRATION.md` (how hosts embed it).

---

## 1. Fast loop

```bash
cd ~/wasm-pdf-reader
npm install

# C++ changed? rebuild the wasm (needs emsdk; check ~/emsdk/emsdk_env.sh)
. ~/emsdk/emsdk_env.sh >/dev/null 2>&1
./build.sh                      # -> dist/wasm/pdf-reader.mjs + pdf-reader.wasm
./build.sh --debug              # assertions + readable stack traces

# JS changed (or after ./build.sh, to refresh dist/index.js version)
npm run build:js                # src/js -> dist/, JSX -> JS for React

# React demo bundle is a committed artifact
node examples/build-react.mjs

# Tests
node test/unit.mjs              # 62 checks, no browser, ~seconds
node test/e2e.mjs               # 14 steps in headless Chrome (see §3)
node --expose-gc test/memory.mjs
node test/pages.mjs             # builds _site and serves it under /wasm-pdf-reader/
scripts/verify_encryption.sh    # needs qpdf; diffs derived keys per revision
```

Rules of thumb:

- **JS-only change** → `npm run build:js` + `node test/unit.mjs` +
  `node examples/build-react.mjs` + `node test/e2e.mjs`.
- **C++ change** → `./build.sh` first, then the same set, plus the MuPDF diff for
  the affected fixture (`scripts/compare_render.py`).
- Do not edit `build.sh` while a build is running: bash reads the script
  incrementally and a half-written line dies with `unexpected EOF`, which looks
  like a code break.
- `dist/` is committed on purpose. If you change it, commit it.

## 2. Verify a render numerically before claiming anything

MuPDF is the oracle (`~/pdfvenv/bin/python` + `pymupdf`):

```bash
node test/render-to-png.mjs test/fixtures/mesh.pdf 1 1 /tmp/ours.png
~/pdfvenv/bin/python scripts/compare_render.py test/fixtures/mesh.pdf 1
```

`compare_render.py` prints ink fraction, mean luminance and mean-abs-diff per
page and exits non-zero outside the gate. The gates: text pages ≤ 1/255 mean abs
diff, scans ≤ 3/255, ink ratio within 2 % relative, plus a `vision_analyze` pass on
a zoomed crop. Numbers say *where*, vision says *what*. A render that **beats**
the reference (ink where MuPDF paints nothing) is a red flag, not a win — it
usually means a malformed fixture or a permissive decoder.

## 3. How to run the end-to-end test

`node test/e2e.mjs` (or `npm run test:e2e`). It is self-contained: picks a free
port, spawns `examples/server.mjs` on it, launches headless Chrome over the
DevTools Protocol, drives both demos, screenshots them into
`docs/screenshots/`, and compares per-page pixel hashes with
`test/baseline/visual.json`.

**Prerequisites**

1. `dist/wasm/pdf-reader.wasm` exists (the script exits with code 2 and says so).
2. A Chrome/Chromium binary. Discovery order: `$CHROME`, the Playwright cache
   (`~/.cache/ms-playwright/chromium-1248/chrome-linux64/chrome`), then
   `/usr/bin/google-chrome`, `/usr/bin/chromium`, `/usr/bin/chromium-browser`.
   Override with `CHROME=/path/to/chrome node test/e2e.mjs`. If the binary needs
   extra shared libraries, an extracted bundle at
   `~/.local/chromedeps/root/usr/lib/x86_64-linux-gnu` is picked up
   automatically (`LD_LIBRARY_PATH` is set for you). To create such a bundle
   without sudo: `apt-get download <lib>` + `dpkg-deb -x <deb> /tmp/debs/root`.
3. **A fresh React demo bundle** if you touched `src/js/react/` or rebuilt
   `dist/react/`: `node examples/build-react.mjs`. A stale
   `examples/react/bundle.js` fails the three React steps with
   `timed out waiting for react first paint` while vanilla steps pass — that is
   the signature, not a real bug.
4. The fixture `test/fixtures/reader-sample.pdf` and its expectations
   `reader-sample.json` (both committed).

**What a good run prints**

```
chrome Chrome/…
  ok  wasm module reports the expected page count
  ok  page 1 paints real ink into the canvas
  ok  canvas is sized for the fit-width scale
  ok  navigation renders a different page
  ok  zoom changes the bitmap, fit-width restores it
  ok  search highlights matches on the page
  ok  clearing the search removes the highlights
  ok  viewer capability probes return booleans, not undefined
  ok  continuous mode stacks every page
  ok  per-page pixel hashes are stable
  ok  page sheet renders all three pages in one image
  ok  React component mounts and renders a page
  ok  React text extraction returns the page text
  ok  React metadata lands in the sidebar

14 steps passed
screenshots: docs/screenshots/
```

Exit code is non-zero if any step failed; every failure is listed with its
message, and the remaining steps still run (useful: one broken step does not hide
the others).

**When it fails**

- *`per-page pixel hashes are stable`* — the rasterizer changed. First decide
  whether the change was intended. If it was, refresh the baseline
  (`npm run test:visual`) and say so in the commit message; if it was not, you
  probably changed a shared path. Attribute it properly: build `HEAD` in a
  `git worktree` (`ln -s ~/wasm-pdf-reader/node_modules node_modules`,
  `node examples/build-react.mjs`) and run e2e there — identical hashes prove your
  tree is not the cause.
- *`timed out waiting for react first paint`* — stale `examples/react/bundle.js`
  (see above), or `dist/react/` was rebuilt without `npm run build:js`.
- *`no Chrome/Chromium found`* — set `CHROME=`.
- *`demo server did not start`* — port collision or a syntax error in
  `examples/server.mjs`; the captured server log is printed in the error.
- Steps that need a *new file loaded*: `viewer.setFile()` immediately after
  `__demoReady` is clobbered by the demo's own initial document finishing later.
  Wait ~1.2 s after boot, call `setFile`, wait again, then assert on
  `viewer.document.pageCount`.

**Debugging outside the suite**

```bash
node test/debug-demo.mjs        # vanilla demo in Chrome, prints console + exceptions
node test/probe.mjs             # C ABI from Node: open, page sizes, render, text
node examples/server.mjs 4444   # http://localhost:4444/ (serves the repo root)
```

`test/cdp.mjs` is the whole browser harness (a dependency-free CDP client:
`launchChrome`, `newPage`, `eval`, `waitFor`, `screenshot`, `setViewport`,
`goto`). Reuse it for any new browser assertion instead of adding Playwright.

## 4. File-by-file

### C++ engine — `src/cpp/`

| file | what it is |
|---|---|
| `pdfdoc.cpp` (2.1k lines) | Object model, lexer, xref tables **and** xref streams, `/Prev` chains, incremental updates, xref rebuild, object streams, all filters (Flate/LZW/ASCII85/ASCIIHex/RunLength + PNG/TIFF predictors), standard security handler (RC4-40/128, AES-128, R5/R6 AES-256), page tree, images (DCT via stb, CCITT, JBIG2 hand-off, JPX hand-off), font loading (TT, CFF, CID, Type3, CMaps), text extraction, AcroForm fields, OCG properties, document metadata. Most "the file does not open" bugs live here. |
| `pdfraster.cpp` (3.1k lines) | The content-stream interpreter and the rasterizer: graphics state, paths, fills/strokes, clips, dashes, text (`showText`), images, form XObjects, tiling patterns, shadings (axial/radial + mesh 4/5), blend modes, transparency groups, annotation painting, OCG visibility. Most "it renders wrong" bugs live here. |
| `wpdf_api.cpp` | The only wasm-aware code: the C ABI (`_wpdf_*`) that `src/js/document.js` calls, the reusable RGBA bitmap, JSON serialisation of text/links/form/outline/warnings, error strings. Add an export here when JS needs new data. |
| `pdfcrypto.hpp` (18 KB, header-only) | MD5, SHA-256/384/512, RC4, AES-CBC. The R5/R6 key derivation (Algorithm 2.B) is here — every printable-ASCII boundary is tested by `test/enc_probe.cpp`. |
| `pdfannots.cpp` | Link annotations (`/URI`, `/GoTo`, direct and named destinations) and appearance synthesis for markup (`/QuadPoints` + `/C`) and `/Text` notes that ship no `/AP`. |
| `cfffont.cpp` + `cfffont.hpp` | CFF/CID-CFF parsing and the Type2 charstring interpreter, including CID→GID via charset and `hmtx` advances. |
| `cff-parser.cc` | Vendored nanopdf CFF table parser (Apache-2.0). Attribution in `THIRD-PARTY.md`; do not "clean it up". |
| `cmap.cpp` + `cmap.hpp` | Predefined Adobe CMaps (lookup into the packed blob) **and** embedded CMap streams: `begincodespacerange`, `begincidrange`, `begincidchar`, `usecmap`, mixed 1/2-byte codespaces. |
| `iccp.cpp` + `iccp.hpp` | ICC parsing: matrix/TRC profiles and A2B0 LUTs. Currently wired into **images only**; fills still use `cmykToRgb()`. |
| `jbig2native.cpp` + `.hpp` | The wasm2c bridge for PDFium's JBIG2/fax decoder: `wasm_rt_init`, `__wasm_call_ctors`, trap handler via `longjmp`, 64 MB pre-grow, re-read `data` after growth. |
| `jpx-decoder.cc` (65 KB) | Vendored nanopdf JPEG 2000 decoder. Broken for most encoders — see ROADMAP M6. |
| `stb_image.h`, `stb_truetype.h` | Vendored stb (MIT/public domain). `pdfdoc.cpp` defines `STB_IMAGE_IMPLEMENTATION` with JPEG only. |
| `jbig2gen/` | Generated + vendored: `jbig2.c` (wasm2c output, **never edit**), `wasm-rt*.c/h`, `simde/` (SIMD shims the generated code needs). Regeneration instructions are in `jbig2native.cpp`. |
| `cmaps_data.cpp` | Generated: 878,855 bytes of packed Adobe CMap data (BSD-3). Built by `scripts/build-cmaps.py`. |
| `fonts_data.h` | Generated: subsetted DejaVu/Liberation/Carlito/Caladea faces compiled into the wasm. `OFL`/Bitstream-Vera licences in `LICENSES/`. |
| `pdf_glyphs.h` | Generated: substitute outlines for non-embedded standard fonts. |
| `pdf_widths.h` | Generated: standard-14 AFM widths (3 families × 4 styles). |
| `cmyk_lut.h` | Generated: 9⁴ CMYK→RGB table calibrated against MuPDF (`scripts/make_cmyk_lut.py`). Device CMYK has no profile, so this table *is* the conversion. |

### JS layer — `src/js/`

| file | what it is |
|---|---|
| `document.js` (440 lines) | `PdfDocument`: the public class. Copies the source bytes into wasm (`copyIntoWasm` handles ArrayBuffer/TypedArray/Blob/File/URL/Response), wraps every `_wpdf_*` call, parses JSON payloads, exposes `pageCount`, `metadata`, `warnings`, `outline`, `form`, `render`, `renderToCanvas`, `getText`, `search`, `getLinks`, `boxToCss`, `setField`, `close`. Renders are serialised (one bitmap). |
| `loader.js` | Module instantiation: one shared instance per page, `wasmUrl`/`wasmBinary`/`module` escape hatches, cache-busted fetch, one retry on `LinkError`, `resetModule()`. |
| `mount.js` (900 lines) | The framework-free viewer: toolbar, page rail with lazy thumbnails, continuous mode (IntersectionObserver), text layer, link layer, search highlights, selection painting, print (rasterizes to a detached sheet), theme, keyboard/wheel handlers. Returns a controller (`goToPage`, `setScale`, `zoomIn/Out`, `fitWidth/Page`, `setFile`, `search`, `setThumbnails`, `setContinuous`, `print`, `rerender`, `destroy`). |
| `text.js` | Text layer helpers: `groupLines` (runs → lines/blocks), `itemsToText` (copy semantics: `<br>`, 0.14 em space rule), `searchItems`, `boxToCss` (PDF y-up → CSS). |
| `index.js` | Public entry: re-exports + `version`. |
| `styles.css` (345 lines) | All viewer CSS, scoped to `.wpdf-root`, driven by `--wpdf-*` variables. |
| `react/PdfReader.jsx` | `<PdfReader>`: composition of hooks + geometry + fit logic + controlled/uncontrolled page and scale. Every prop is documented in the README table. |
| `react/PdfPage.jsx` | One page: canvas + text layer + link layer + overlay slot + selection. |
| `react/PdfToolbar.jsx` | Default toolbar (`toolbarLabels` for i18n). |
| `react/hooks.js` | `usePdfDocument` (open/close lifecycle, status, reload), `usePdfPage` (renders into a canvas, device vs CSS pixels, `renderer`/`pixelTransform` hooks), `useInView` (continuous mode). |
| `react/index.js` | React entry point: components + hooks + core re-exports. |
| `src/types/*.d.ts` | Hand-written types for both entries; `scripts/build-js.mjs` copies them into `dist/`. |

### Tests — `test/`

| file | what it is |
|---|---|
| `unit.mjs` (616 lines, 62 checks) | The real contract: opens every fixture, asserts page counts, ink in regions, colours, text, links, encryption, CMaps, OCG, forms, patterns, shadings, JBIG2, memory shape. Add one assertion per feature. |
| `e2e.mjs` | Browser suite, §3. Also writes the README screenshots. |
| `cdp.mjs` | Dependency-free Chrome DevTools Protocol client + launcher. |
| `pages.mjs` | Builds `_site/` and serves it under `/wasm-pdf-reader/` to catch root-absolute path bugs before Pages does. |
| `memory.mjs` | Soak: render/open/close/text in a loop, assert the heap plateaus (`--expose-gc`). Keep green when touching the JBIG2 pre-grow or the bitmap reuse. |
| `render-to-png.mjs` | Dev helper: wasm pixels → PNG (`<pdf> <page> <scale> <out>`). |
| `probe.mjs` | Dev helper: exercise the C ABI directly from Node. |
| `debug-demo.mjs` | Dev helper: load the vanilla demo in Chrome and print console output/exceptions. |
| `enc_probe.cpp`, `cmap_probe.cpp` | Native probes: derive an encryption key / dump CMap lookups, diffed by scripts. |
| `fixtures/` | Committed PDFs, one per feature (`mesh.pdf`, `patterns.pdf`, `spot.pdf`, `ocg.pdf`, `form.pdf`, `type3.pdf`, `cjk-cmaps.pdf`, `jb2globals.pdf`, six encryption revisions, …). Generated by `scripts/make_*`; regenerate, never hand-edit bytes. |
| `baseline/visual.json` | Per-page pixel hashes for e2e. Refresh only for intended rendering changes. |

### Scripts — `scripts/`

| file | what it is |
|---|---|
| `build-js.mjs` | `src/js` → `dist/`: copies core modules verbatim (no bundler), esbuild only for JSX, copies CSS + `.d.ts`, rewrites the version constant from `package.json`. |
| `build-demo.mjs` | `_site/` for GitHub Pages: landing page, both demos, `dist/`, fixture; rewrites root-absolute paths for the sub-path. |
| `competitive-parity.mjs` | Source of truth for the README "How it compares" tables and percentages. Edit scores **only** with the capability, then paste the regenerated table. |
| `compare_render.py` | MuPDF diff: ink fraction, mean luminance, mean-abs-diff per page; non-zero exit outside the gate. |
| `make-fixture.mjs`, `make-objstm-fixture.mjs` | Base fixtures: 3 pages with text/vector/image/clip/`/Rotate 90`; calibre-shaped object-stream + xref-stream file. |
| `make_*.py` | One per feature (`annot`, `cjk`, `cmyk_lut`, `encrypted`, `form`, `jpx`, `ocg`, `pattern`, `shading`, `shared_xobject`, `spot`, `type3`). Each writes its PDF into `test/fixtures/`; the script is the spec. |
| `build-cmaps.py` | Adobe CMaps → `cmaps_data.cpp` varint blob. |
| `verify_encryption.sh` | Compares our derived file key with `qpdf --show-encryption-key` for every revision, user and owner. |

### Examples, docs, root

| path | what it is |
|---|---|
| `examples/vanilla/index.html` | The framework-free demo (also the e2e target). |
| `examples/react/` | React demo (`main.jsx`, `index.html`) **and the committed `bundle.js`** — rebuild it after touching React sources. |
| `examples/build-react.mjs` | esbuild → `examples/react/bundle.js`. |
| `examples/server.mjs` | Static server for the repo root (`node examples/server.mjs 4444`), no caching. |
| `examples/selftest.html` | Manual smoke page: opens three awkward fixtures and prints ink. |
| `docs/INTEGRATION.md` | Host integration: asset resolution, bundlers, Next/Vite/webpack/CRA, Vue/Svelte/Angular, React props/hooks, workers, CSP, pitfalls. |
| `docs/screenshots/` | README images, **regenerated by e2e** — do not hand-craft. |
| `dist/` | Published output, committed: `index.js`, `document.js`, `loader.js`, `mount.js`, `text.js`, `react/`, `styles.css`, `index.d.ts`, `wasm/pdf-reader.{mjs,wasm}`. `dist/jbig2.js` + `dist/wasm/jbig2/` are unreferenced leftovers and should be deleted, not imitated. |
| `build.sh` | emcc invocation (release/`--debug`), the C ABI exports list, `-s DYNAMIC_EXECUTION=0`, `-sMAXIMUM_MEMORY=1073741824`. |
| `THIRD-PARTY.md`, `LICENSES/` | Licence map + line-count split. Vendoring or generating a file is a licensing change: add the row and, if required, the licence text. |
| `README.md`, `ROADMAP.md` | Public claims and the ordered gap list. Both are part of every deliverable. |

## 5. Invariants that bite (short list, with the reason)

- **`Mat::mul(X)` applies `this` first** — `a.mul(b).mul(c)` runs a→b→c. Composing a
  tile/form transform the intuitive way paints every pixel outside its buffer
  (blank buffer, fill counter still increments).
- **Premultiply before sampling** transparent buffers, or edges drag to black.
- **Reset blend/alpha entering an isolated group**, or a leaked `/BM /Multiply`
  renders the group solid black.
- **Forms clip to `/BBox`**; producers rely on it.
- **Operands are pushed in order** — `d` reads the array at `stack[size-2]`.
- **`BT` resets only `Tm`/`Tlm`**; font, size, spacing, render mode live in the
  graphics state.
- **CID advances**: fall back to `hmtx`, not `/DW 1000`; `adv1000 = adv * (1/upem) * 1000`.
- **Face cache keys on the caller's buffer**, not a patched copy.
- **1-bpc images index by byte-padded `rowBytes`.**
- **Heap is capped at 1 GiB**; probe `malloc` before a canvas `assign` because
  `-fno-exceptions` turns a failed allocation into an abort.
- **Mesh vertices carry a 1-bit edge flag, type 4 included** — fixture and reader
  must agree, or "matching" MuPDF proves nothing.
- **Text layer**: absolute spans, so emit `<br>` between baselines, a space when the
  gap > 0.14 em, `white-space: pre` on the layer; selection is painted by hand
  because the browser paints one box per span.
- **Overlays are `pointer-events: none`** — `.wpdf-link` must set `auto` itself.
- **`/URI` needs a scheme allowlist** (`http/https/mailto/tel`, internal `#page-`)
  before it reaches an anchor: `javascript:` in a link annotation is DOM XSS.
- **Continuous mode**: append the text/link/selection layers inside `renderInto`,
  and do not vertically centre the page stack (`justify-content: flex-start`).
- **Fit-width can run away** if the viewport is a flex item — clamp to ~98 % of the
  window width and set `min-width: 0`.

## 6. Finishing a milestone (definition of done)

For `M{N}` in `ROADMAP.md`:

1. Fixture first: a committed `scripts/make_*.py` (or real-producer file) that
   fails today.
2. Implement; keep the numeric gate from the milestone's *Accept* line, checked
   with `scripts/compare_render.py` **and** a `vision_analyze` crop.
3. `node test/unit.mjs` gains at least one pixel/API assertion; stays green.
4. `node test/e2e.mjs` green; refresh `test/baseline/visual.json` only if the
   change is intended, and say so in the commit.
5. `README.md`: capability bullet + the "not supported" paragraph + the *Limits*
   section if a ceiling moved. Re-measure counts (`62 checks`, `14 e2e steps`,
   `ls -l dist/wasm/`) — do not copy the previous README, it accumulates stale
   claims.
6. `ROADMAP.md`: mark the milestone, record what landed and what did not, keep the
   remaining defects.
7. If the capability changed, update `scripts/competitive-parity.mjs` (score +
   `gap` line) and paste the regenerated table into the README.
8. If a vendored/generated file was added or replaced: `THIRD-PARTY.md` row and
   `LICENSES/` entry, plus provenance in the file's header.
9. If the public API changed: `src/types/*.d.ts`, the README API block, and
   `docs/INTEGRATION.md`.
10. Deploy (below) and sha-verify.

## 7. Deploy (two branches)

```bash
git push origin main
node scripts/build-demo.mjs
# gh-pages worktree at /tmp/ghpages (git worktree add /tmp/ghpages gh-pages)
cp -r _site/. /tmp/ghpages/
# stage only: dist index.html react vanilla reader-sample.pdf .nojekyll
git -C /tmp/ghpages commit -m "deploy: <what>"
git -C /tmp/ghpages push
```

Then verify what the server actually returns — GitHub Pages serves the previous
file from cache for up to a minute:

```bash
curl -s "https://dionaditya.github.io/wasm-pdf-reader/dist/wasm/pdf-reader.wasm?v=$RANDOM" \
  | sha256sum
sha256sum dist/wasm/pdf-reader.wasm
```

For a JS/CSS-only change, sha-compare `dist/mount.js` and `dist/styles.css` too —
the wasm hash will not catch a missed JS deploy. And confirm the push landed
(`git -C /tmp/ghpages log --oneline -1`).

## 8. Never

- Claim a fix from a successful build. Render it, diff against MuPDF, then look at
  the demo in a real browser.
- Write a "replace the viewer in <host>" document before checking which pdf.js
  version that host actually ships. A patched dependency kills the premise.
- Say a feature is supported while `ROADMAP.md` calls it unverified (mesh 4/5,
  JPX) — or keep a "not supported" line for something that landed (Type3,
  outlines and form fields were all stale once).
- Hand-edit a generated file (`jbig2.c`, `cmaps_data.cpp`, `fonts_data.h`,
  `cmyk_lut.h`) or a generated table in the README.
- `pkill -f <pattern>` a server you started from a shell: the pattern matches the
  invoking `bash -c` line and kills your own shell. Keep the PID.
- Publish with a half-bumped version: `npm publish` here dies with `EOTP`
  (2FA), so the bump touches `package.json`, `src/js/index.js`, `dist/index.js`
  (via `npm run build:js`) and `package-lock.json`. Leave the actual publish to
  the maintainer and say the registry is behind.

## 9. Prompt templates

Starting a session:

> Work on M{N} in `~/wasm-pdf-reader` (see `ROADMAP.md` and `CLAUDE.md`). Read the
> milestone, then: fixture first, implement, prove it with
> `scripts/compare_render.py` plus the milestone's *Accept* numbers, add unit
> checks, keep `test/e2e.mjs` green, update README + ROADMAP + the parity table,
> then deploy and sha-verify.

Fixing a render defect:

> `<fixture>` renders wrong: <describe>. Compare against MuPDF with
> `node test/render-to-png.mjs` + `scripts/compare_render.py`, find the operator
> in `src/cpp/pdfraster.cpp`, fix, and add an assertion to `test/unit.mjs`. Do not
> change the fixture unless it is malformed — say so first if it is.

Debugging a host integration:

> Read `docs/INTEGRATION.md`. The host is <framework>. The symptom is <error>.
> Check asset resolution first (`wasmUrl`), then CSP (`'wasm-unsafe-eval'`), then
> the bundler's `node:module` externalisation, then MIME (`application/wasm`).
