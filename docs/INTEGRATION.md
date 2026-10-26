# Integrating wasm-pdf-reader

Everything a host app needs: the two files that ship together (JS glue and
`.wasm`), how each bundler finds them, and the React layer. The recipes marked
**verified** were run in a headless Chrome against the published `dist/`; the
rest are the standard configuration for that tool, and the pitfalls section says
what breaks when they are wrong.

## TL;DR

```bash
npm install @dionaditya/wasm-pdf-reader
```

1. Ship **three** things from the package: the JS modules (`dist/`), the stylesheet
   (`dist/styles.css`) and `dist/wasm/pdf-reader.wasm`.
2. Make sure `pdf-reader.wasm` is served from a real URL — most bundlers do *not*
   copy it for you. Copy it into your static dir and pass `wasmUrl` (below).
3. Serve it as `application/wasm` and, if you send a CSP, allow
   `script-src 'wasm-unsafe-eval'`.

```js
const doc = await PdfDocument.open(bytes, { wasmUrl: '/pdf-reader.wasm' });
```

## What the package exports

| import | contents |
|---|---|
| `@dionaditya/wasm-pdf-reader` | `PdfDocument`, `mountPdfReader`, `loadModule`, `resetModule`, `applyTheme`, `defaultLabels`, `groupLines`, `itemsToText`, `searchItems`, `boxToCss`, `version` |
| `@dionaditya/wasm-pdf-reader/react` | `PdfReader`, `PdfPage`, `PdfToolbar`, `toolbarLabels`, `usePdfDocument`, `usePdfPage`, `useInView` + the core re-exports |
| `@dionaditya/wasm-pdf-reader/styles.css` | the viewer's stylesheet (required for the mounted viewer, optional if you only call `PdfDocument`) |
| `@dionaditya/wasm-pdf-reader/wasm/pdf-reader.wasm` | the binary, for hosts that want to copy it from `node_modules` at build time |

ESM only — `require()` does not work. Types ship for both entry points
(`dist/index.d.ts`, `dist/react/index.d.ts`). React is an **optional** peer
dependency (`>=17`); nothing outside `…/react` imports it.

## How the wasm is found (and why it sometimes isn't)

`dist/loader.js` resolves the binary at first document open, in this order:

1. `options.module` — a pre-instantiated emscripten Module (advanced/host-managed).
2. `options.wasmBinary` — your own `ArrayBuffer`/`Uint8Array` (fetched however you like).
3. `options.wasmUrl` — a URL; the loader fetches it (`cache: 'no-cache'`) and adds
   `?v=<version>` to defeat a stale-glue/fresh-wasm mismatch.
4. Otherwise a sibling of the module: `new URL('./wasm/pdf-reader.wasm', import.meta.url)`.

Step 4 is the one that surprises people. It is resolved **against the URL of the
JS file after your bundler has moved it**, not against `node_modules`. If your
build outputs `assets/index-abc123.js`, the browser asks for
`assets/wasm/pdf-reader.wasm` — a file nobody produced, so you get a 404 or, on a
single-page-app host, your `index.html` served with `text/html`, and the module
fails with a MIME/`CompileError`. Bundlers do not copy an asset that is fetched at
runtime; they only copy statically imported files.

The escape hatch is always the same: **copy the wasm to your public directory once
and pass `wasmUrl`.**

```bash
# any one of these; it is a plain copy, not a build step
cp node_modules/@dionaditya/wasm-pdf-reader/dist/wasm/pdf-reader.wasm public/
```

```js
PdfDocument.open(bytes, { wasmUrl: '/pdf-reader.wasm' });
// Vite/Next with a base path, and Pages-style sub-path deploys:
PdfDocument.open(bytes, { wasmUrl: `${import.meta.env.BASE_URL}pdf-reader.wasm` });
```

`mountPdfReader(target, { file, wasmUrl })` forwards the option too (it spreads its
options into `PdfDocument.open`), and the React component takes it as a prop.

## Plain HTML, no build step — verified

Copy `dist/` next to your page and import it as a module. The glue's sibling
resolution works here because nothing has moved the files.

```html
<link rel="stylesheet" href="/vendor/wasm-pdf-reader/styles.css">
<div id="viewer" style="height:100vh"></div>
<script type="module">
  import { mountPdfReader } from '/vendor/wasm-pdf-reader/index.js';
  mountPdfReader('#viewer', { file: '/docs/lease.pdf', fit: 'width' });
</script>
```

If you want the whole engine with **zero** local files, any CDN that serves npm
works, but you must point at the wasm explicitly because the CDN rewrites paths:

```html
<link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/@dionaditya/wasm-pdf-reader@0.3.0/dist/styles.css">
<script type="module">
  import { mountPdfReader } from 'https://cdn.jsdelivr.net/npm/@dionaditya/wasm-pdf-reader@0.3.0/dist/index.js';
  mountPdfReader('#viewer', {
    file: '/docs/lease.pdf',
    wasmUrl: 'https://cdn.jsdelivr.net/npm/@dionaditya/wasm-pdf-reader@0.3.0/dist/wasm/pdf-reader.wasm',
  });
</script>
```

### Classic script (CMS, ownCloud-style apps, no bundler)

Mount from a **deferred classic** script that dynamically imports the bundle.
`document.currentScript` is `null` inside `type="module"`, and a head-loaded
script runs before the mount node exists — `defer` + `import()` fixes both, and
dynamic import is legal in a classic script.

```html
<div id="viewer"></div>
<script defer src="/apps/files_pdfviewer/js/mount-wasm.js"></script>
```

```js
// mount-wasm.js — classic script, so `this`/currentScript behave
(function () {
  const node = document.getElementById('viewer');
  const file = new URLSearchParams(location.search).get('file');
  import('/apps/files_pdfviewer/js/vendor/wasm-pdf-reader/index.js').then(({ mountPdfReader }) => {
    mountPdfReader(node, { file, wasmUrl: '/apps/files_pdfviewer/js/vendor/wasm-pdf-reader/wasm/pdf-reader.wasm' });
  });
})();
```

Keep a `VERSION.json` beside a vendored bundle (commit + per-file sha256). That is
what makes "pinned, traceable, no install-time build" reviewable — see the
ownCloud fork linked from the README.

## Vite

Vite serves `node_modules` files as-is in dev and rewrites `new URL(...,
import.meta.url)` in build, so the sibling path *usually* resolves. Do not rely on
it: the asset lands under `/assets/` with a hash and the loader's `?v=` query
stays attached. Pin it instead.

```js
// vite.config.js — copy the binary into the build, no plugin needed
import { copyFileSync, mkdirSync } from 'node:fs';
export default {
  plugins: [{
    name: 'copy-pdf-wasm',
    buildStart() { /* dev: file lives in node_modules, see below */ },
    writeBundle() {
      mkdirSync('dist', { recursive: true });
      copyFileSync('node_modules/@dionaditya/wasm-pdf-reader/dist/wasm/pdf-reader.wasm',
                   'dist/pdf-reader.wasm');
    },
  }],
};
```

```js
const wasmUrl = import.meta.env.BASE_URL + 'pdf-reader.wasm';
```

`vite dev` serves it from `public/`, so put the same copy there (or use
`vite-plugin-static-copy`). **Known bundler issue:** the emscripten glue contains
`import("node:module")` inside a Node-only branch. Rollup usually externalises it
with a warning; esbuild (below) stops with `Could not resolve "node:module"`
unless you mark it external.

## esbuild — verified

```bash
esbuild src/main.js --bundle --format=esm --outfile=dist/app.js \
  --platform=browser --external:node:module
cp node_modules/@dionaditya/wasm-pdf-reader/dist/wasm/pdf-reader.wasm dist/
```

Without `--external:node:module` the build fails on the emscripten Node branch
(a code path that never runs in a browser). With the wasm copied beside `app.js`
the engine loads, parses and paints (checked in headless Chrome: 3 pages, ink on
page 1). `dist/wasm/pdf-reader.wasm` is *not* emitted by esbuild — the copy line
is mandatory.

## webpack 5 / Create React App

No `experiments.asyncWebAssembly` needed (the binary is fetched, not imported), but
webpack 5 does not know `node:module` from a dependency either:

```js
// webpack.config.js
module.exports = {
  resolve: { fallback: { 'node:module': false } },
  plugins: [
    new (require('copy-webpack-plugin'))({
      patterns: [{ from: 'node_modules/@dionaditya/wasm-pdf-reader/dist/wasm/pdf-reader.wasm',
                   to: 'pdf-reader.wasm' }],
    }),
  ],
};
```

**CRA** users cannot edit that config without ejecting or `craco`: either add
`craco` with the same `resolve.fallback`, or bypass bundler resolution entirely
and load the engine from a `<script type="module">` in `public/index.html` that
sets `window.PdfReaderLib` — then pass `wasmUrl: process.env.PUBLIC_URL +
'/pdf-reader.wasm'` after copying the binary to `public/`. Serving the wasm from
`public/` is what makes CRA work at all: everything under `public/` is copied
verbatim.

## Next.js

The wasm must live in `public/` — Next compiles the modules into
`/_next/static/chunks/…`, so the sibling path resolves into a directory that does
not contain your binary.

```bash
cp node_modules/@dionaditya/wasm-pdf-reader/dist/wasm/pdf-reader.wasm public/
```

```jsx
// app/viewer/PdfViewer.jsx
'use client';
import { PdfReader } from '@dionaditya/wasm-pdf-reader/react';
import '@dionaditya/wasm-pdf-reader/styles.css';

export default function PdfViewer({ url }) {
  return <PdfReader file={url} wasmUrl="/pdf-reader.wasm" fit="width" />;
}
```

```jsx
// app/viewer/page.jsx — keep the engine out of the server bundle
'use client';
import dynamic from 'next/dynamic';
const PdfViewer = dynamic(() => import('./PdfViewer'), {
  ssr: false,
  loading: () => <p>loading viewer…</p>,
});
export default function Page() { return <PdfViewer url="/docs/lease.pdf" />; }
```

Why `ssr: false`: the engine is wasm and `fetch`-based; a server render would try
to instantiate it in Node and ship a 3 MB dependency into the RSC payload. Pages
router: same idea with `dynamic(..., { ssr: false })`, `next.config` untouched.
Turbopack does not yet resolve the Emscripten glue reliably — if you hit
`Cannot find module 'node:module'`, that is the same branch as the esbuild note;
use webpack (`next dev --webpack`) or load the engine from a client-only script.

## Vue 3 / Svelte / Angular

All three just need a DOM node at mount time and teardown on unmount.

```vue
<!-- Vue 3 -->
<template><div ref="el" style="height:100vh"></div></template>
<script setup>
import { ref, onMounted, onBeforeUnmount } from 'vue';
import { mountPdfReader } from '@dionaditya/wasm-pdf-reader';
import '@dionaditya/wasm-pdf-reader/styles.css';
const el = ref(null); let viewer;
onMounted(() => { viewer = mountPdfReader(el.value, { file: props.url, wasmUrl: '/pdf-reader.wasm' }); });
onBeforeUnmount(() => viewer?.destroy());
</script>
```

```svelte
<!-- Svelte 5 -->
<script>
  import { onMount } from 'svelte';
  import { mountPdfReader } from '@dionaditya/wasm-pdf-reader';
  import '@dionaditya/wasm-pdf-reader/styles.css';
  export let url;
  let el, viewer;
  onMount(() => { viewer = mountPdfReader(el, { file: url, wasmUrl: '/pdf-reader.wasm' });
                  return () => viewer.destroy(); });
</script>
<div bind:this={el} style="height:100vh"></div>
```

Angular: `ngAfterViewInit` → `mountPdfReader`, `ngOnDestroy` → `viewer.destroy()`;
add `"allowedCommonJsDependencies": []` is not needed (the package is pure ESM), but
`angular.json` needs the stylesheet in `styles` or an `@import` in your SCSS.

## React in depth

```jsx
import { PdfReader } from '@dionaditya/wasm-pdf-reader/react';
import '@dionaditya/wasm-pdf-reader/styles.css';

<PdfReader
  file={file}                     // ArrayBuffer | Uint8Array | Blob/File | URL | Response
  wasmUrl="/pdf-reader.wasm"
  fit="width"                     // 'width' | 'page' | 'none'
  continuous
  onPageChange={(p) => setPage(p)}   // zero-based index
  pageOverlay={(page, { document, scale, pageCount }) => <Watermark page={page} />}
  toolbar={(api) => <MyToolbar api={api} />}
  theme={{ bg: '#0b0f19', accent: '#7dd3fc' }}
/>
```

- **Controlled vs uncontrolled.** `page`/`scale` are controlled when you pass
  them, `defaultPage`/`defaultScale` are the uncontrolled seeds. Mixing them is
  the usual "my zoom resets itself" bug.
- **Escape hatches, in increasing order of commitment:** `theme` and `components`
  (restyle), `toolbar={false}` + your own buttons over the `api` object, `hooks`
  (`usePdfDocument`, `usePdfPage`, `useInView`) to build your own layout,
  `PdfDocument` + `renderToCanvas` to skip the React layer entirely.
- **`renderer`** swaps the backend: anything matching
  `{ render(doc, page, opts) -> { imageData } }` works, which is the seam for a
  server-rendered or cached pipeline.
- **`pixelTransform`** post-processes the RGBA of every page (invert, tint,
  watermark) inside the render call, so scrolling back does not recompute it.
- **Strict mode:** React 18 mounts twice in dev. The component tolerates it (the
  document is closed on cleanup), but a `<PdfReader key={…}>` remount re-parses;
  keep the `file` identity stable (`useMemo` on the ArrayBuffer) rather than
  recreating it per render.
- **SSR:** render nothing on the server (`dynamic(..., { ssr: false })`, or a
  `typeof window === 'undefined'` guard around `<PdfReader>`). Nothing in the core
  touches `window` at import time, so the *module* can be imported during SSR; only
  mounting must be client-side.
- **Styling:** the CSS is scoped to `.wpdf-root`, so it does not leak into your
  page. Tokens are CSS variables (`--wpdf-bg`, `--wpdf-accent`, `--wpdf-radius`,
  …) which `theme` sets on the root node. Ship the stylesheet once, globally.
- **Teardown:** `viewer.destroy()` (React does it for you) removes the DOM, closes
  the document and detaches observers. For a long-lived SPA that opens hundreds of
  files, call `resetModule()` when you want the 1 GiB heap high-water mark back.

## React without the components

```jsx
const { document: doc, status, error, pageCount, metadata, warnings, reload } =
  usePdfDocument(file, { wasmUrl: '/pdf-reader.wasm' });

const { canvasRef, width, height, cssWidth, cssHeight, rendering, error: renderError } =
  usePdfPage(doc, pageNumber, { scale: 1.5 });

return <canvas ref={canvasRef} style={{ width: cssWidth, height: cssHeight }} />;
```

`usePdfPage` owns a canvas: it renders into it on every page/scale change and
reports device pixels (`width`/`height`) separately from CSS pixels
(`cssWidth`/`cssHeight`, already divided by `dpr`), so a retina render is not a
stretched one. Pass `renderer` to swap the backend, `pixelTransform` to post-process
the pixels, `enabled: false` to skip off-screen pages. `useInView` is the
IntersectionObserver used by continuous mode — the building block for a
virtualised page list.

## Server-side / Node

The same modules run in Node 18+, including a `file:` URL source:

```js
import { PdfDocument } from '@dionaditya/wasm-pdf-reader';
const doc = await PdfDocument.open('file:///srv/docs/lease.pdf');
const { imageData, width, height } = await doc.render(0, { scale: 2 });
const text = doc.getPageText(0);
```

Useful for thumbnails, text indexing and pre-flight checks. Two caveats: the heap
still only grows (one process, many documents → recycle the worker/process), and
`file:` handling goes through a dynamic `node:fs` import, so bundling for a
browser needs the `node:module` external noted above.

## Web worker mode

Built in since 0.3.0 (M9.1): pass `worker: true` and the parse and every render
happen in a worker. The main thread only does `putImageData` on pixels that come
back transferred.

```js
const doc = await PdfDocument.open(file, {
  worker: true,
  wasmUrl: '/pkg/pdf-reader.wasm',   // still the main-thread copy: the worker fetches it too
  workerUrl: '/pkg/worker.js',       // REQUIRED when the library is bundled
});
await doc.renderToCanvas(canvas, 0, { scale: 1.5 });
const hits = await doc.search('total');        // Promise in worker mode
doc.close();                                   // terminates the worker when it is the last doc
```

Three things a host must know:

- **`workerUrl` in a bundle.** The default is `new URL('./worker.js',
  import.meta.url)`, which in an unbundled page resolves next to `dist/document.js`
  and is correct. In a bundle (Vite, esbuild, webpack) `import.meta.url` is the
  *page*, so set `workerUrl` explicitly, or copy `dist/worker.js`, `dist/remote.js`
  and `dist/wasm/` into your public directory and point at them. A 404 here falls
  back to the inline path — check `doc.mode === 'worker'` in CI, not by eye.
- **CSP:** `worker-src 'self'` (plus `blob:` only if you build the worker URL from
  a blob), and the worker itself needs `script-src 'self' 'wasm-unsafe-eval'` for
  `WebAssembly.compile`.
- **One worker per pool, reused across documents.** Opening a second file while
  the first is open shares the module; closing the last document terminates the
  worker, so a serial open/close cycle recompiles the wasm each time (~50 ms).
  Keep documents open if you switch between them.

The synchronous part of the API survives because geometry, metadata, the outline,
form fields and warnings are captured at open time. `getText()`/`getLinks()` are
warm for a page that has been rendered; use `await loadText(i)` /
`getPageText(i)` for a page that has not. `{ worker: 'strict' }` throws instead
of falling back.

### Rolling your own worker

If you need OffscreenCanvas, a different scheduling policy, or your own message
protocol, the module is still a normal library inside a worker — the module owns
one render bitmap and serialises renders, so use one `PdfDocument` per worker and
transfer the `ArrayBuffer`s in:

```js
// pdf.worker.js
import { PdfDocument } from '@dionaditya/wasm-pdf-reader';
let doc;
self.onmessage = async ({ data }) => {
  if (data.open) {
    doc = await PdfDocument.open(data.bytes, { wasmUrl: self.location.origin + '/pdf-reader.wasm' });
    self.postMessage({ pageCount: doc.pageCount });
  } else {
    const p = await doc.render(data.page, { scale: data.scale });
    self.postMessage({ page: data.page, width: p.width, height: p.height, image: p.imageData.data.buffer },
                     [p.imageData.data.buffer]);
  }
};
```

Then paint into an `OffscreenCanvas` in the worker (or transfer the buffer back
and `putImageData` on the main thread).

## Ranged loading (big files, no copy)

By default the file is copied into wasm memory once. For a large document —
several hundred pages, or a scan big enough to matter on a phone — the engine can
instead read byte ranges on demand, so the wasm heap holds a window instead of
the file (M9.2):

```js
const doc = await PdfDocument.open(
  { url: '/files/annual-report.pdf', size: 117_000_000 },
  { worker: true, wasmUrl: '/pkg/pdf-reader.wasm', workerUrl: '/pkg/worker.js' },
);
```

Or with a `File`/`Blob`, which slices locally and never touches the network:

```js
const doc = await PdfDocument.open({ blob: file, size: file.size }, { worker: true });
```

Requirements and behaviour:

- **Worker mode is mandatory.** The parser's reads are synchronous, so a ranged
  source can only run off the main thread. `open()` throws a clear error if you
  ask for one inline.
- **Your server must answer `Range`** (`206 Partial Content`). If it ignores the
  header and returns `200` with the whole body, that response is kept and sliced
  locally — correct, but you paid the full download. `examples/server.mjs` shows
  a 60-line implementation; check with
  `curl -sI -H 'Range: bytes=0-99' https://your.host/doc.pdf` for a `206`.
- **One ranged document per worker.** The fetch hook is per worker, so opening a
  second ranged file while the first is alive throws rather than silently reading
  the wrong bytes. Close the first, or give each its own pool.
- **`size` is required** for a URL. Range requests need to know where the file
  ends; read it from `Content-Length` (`HEAD`) or your own metadata.
- **Cost to know about:** the worker reads every page dict at open to fill the
  viewer's geometry snapshot. That is one range request per page — fine on a fast
  origin (117 MB / 240 pages: ~3 s), painful on a slow one. If that matters,
  open the document, then measure before deciding.

What this is not: streaming *rendering*. The page you draw is fully fetched
before it is rasterized; ranged loading narrows *which* bytes that is.

## Serving the documents themselves

`PdfDocument.open(source)` accepts:

| source | note |
|---|---|
| `ArrayBuffer` / `Uint8Array` | copied into wasm memory; release the JS copy afterwards |
| `Blob` / `File` | one `arrayBuffer()` await, then as above |
| `URL` string | fetched by the library (same-origin or CORS-permitted) |
| `Response` | read as bytes — use this when you need an `Authorization` header |

```js
const res = await fetch('/api/doc/42', { headers: { Authorization: `Bearer ${token}` } });
const doc = await PdfDocument.open(res);
```

The whole file is resident in wasm memory while open (see README *Limits*), so for
very large scans prefer one document at a time, or `resetModule()` between files.

## CSP and headers

```
Content-Security-Policy: script-src 'self' 'wasm-unsafe-eval'; worker-src 'self' blob:;
```

`'wasm-unsafe-eval'` is required: WebAssembly compilation is counted as dynamic
execution. `'unsafe-eval'` also works but is a strictly worse trade — this engine
needs no `eval` at all (the build sets `-s DYNAMIC_EXECUTION=0`), so anything else
in your bundle asking for `'unsafe-eval'` should be the thing you remove. Not
needed: `'unsafe-inline'`, blob URLs, or third-party origins — the binary is
same-origin by default.

Serve the binary as `application/wasm`:

```
# nginx
types { application/wasm wasm; }
# Apache
AddType application/wasm .wasm
# S3/CloudFront: set the Content-Type metadata on the object
```

Some hosts (notably single-page-app rewrites) answer a missing `.wasm` with
`index.html`; the symptom is `CompileError: expected magic word 00 61 73 6d`
— that is a routing bug, not a corrupt build.

## Pitfalls, by symptom

| symptom | cause | fix |
|---|---|---|
| `CompileError: expected magic word 00 61 73 6d` | the server returned HTML for the `.wasm` | fix the rewrite rule or the `wasmUrl` path |
| `Incorrect response MIME type. Expected 'application/wasm'` | wrong `Content-Type` | set it (see above) |
| 404 on `/assets/wasm/pdf-reader.wasm` | the bundler moved the JS, nobody copied the binary | copy to `public/` + `wasmUrl` |
| `LinkError: import function … must be callable` | stale glue paired with a fresh `.wasm` (aggressive cache or mismatched versions) | don't hand-mix versions; let `wasmUrl` add `?v=` or set `wasmBinary` |
| `EvalError`/`CompileError` naming `wasm-unsafe-eval` | CSP | add the directive |
| `Cannot read properties of undefined (reading 'open')` | wrong entry point: `PdfReader` vs `PdfDocument`, or a default-import of a module that has no default | import `{ PdfDocument }` from the root, `{ PdfReader }` from `/react` |
| `require is not defined` / `ERR_REQUIRE_ESM` | CommonJS consumer | use `import`, or dynamic `import()` from CJS |
| blank page, `pageCount` fine | the canvas was resized after `renderToCanvas` | render after layout; the viewer does this for you |
| viewer shrinks/grows on load in a flex parent | fit-width measuring a stretched viewport | give the viewport a fixed height and `min-width: 0` |
| React dev shows the viewer twice | Strict-mode double mount | keep `file` identity stable; the second mount replaces the first |
| iOS `out of memory` | asked for a huge `scale` (the heap is capped at 1 GiB) | render smaller, check `stats`/warnings |

## Verifying an integration

```bash
# 1. the binary you ship is the binary you built
sha256sum dist/pdf-reader.wasm node_modules/@dionaditya/wasm-pdf-reader/dist/wasm/pdf-reader.wasm

# 2. no dynamic execution crept in (this is the security-review question)
grep -rnE "\beval\(|new Function\(" dist/ | grep -v '\.wasm'

# 3. pixels actually reach the DOM — do it in a browser, not in jsdom
node your-e2e.mjs   # assert ink coverage + pageCount, not "no error"
```

`test/e2e.mjs` in this repo is a working example of step 3 with no dependencies
beyond Chrome: it mounts the viewer over HTTP, reads a canvas back, and hashes it.

## Version pinning

- **npm:** the registry lags the repository — see the README *Install* section
  before telling a reviewer the published build is current.
- **commit pin:** `"@dionaditya/wasm-pdf-reader": "github:dionaditya/wasm-pdf-reader"`,
  `dist/` is committed so no build runs on install.
- **vendored copy:** copy `dist/` into your tree with a `VERSION.json` (package
  version, commit, per-file sha256) and a `sync` script; that is the ownCloud
  pattern and the only shape that survives an air-gapped review.
