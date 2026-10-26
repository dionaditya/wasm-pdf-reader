#!/usr/bin/env node
// build-demo.mjs — assemble the static demo published to GitHub Pages.
//
// Output: _site/
//   index.html            landing page
//   vanilla/              the framework-free viewer
//   react/                the React component demo
//   dist/                 the library as published (ESM + wasm)
//   reader-sample.pdf     the generated fixture
//
// The example pages use root-absolute paths (/dist/...), which is what the
// local demo server serves. Pages hosts the site under /<repo>/, so the paths
// are rewritten to ../ here — the same files then work from any base path.

import { cp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const out = join(root, '_site');

const rewrite = (html) => html
  .replaceAll('/dist/styles.css', '../dist/styles.css')
  .replaceAll('/dist/index.js', '../dist/index.js')
  .replaceAll('/dist/react/index.js', '../dist/react/index.js')
  .replaceAll('/dist/wasm/pdf-reader.wasm', '../dist/wasm/pdf-reader.wasm')
  .replaceAll('/dist/worker.js', '../dist/worker.js')
  .replaceAll('/test/fixtures/reader-sample.pdf', '../reader-sample.pdf');

if (!existsSync(join(root, 'dist', 'wasm', 'pdf-reader.wasm'))) {
  console.error('dist/wasm/pdf-reader.wasm is missing — run ./build.sh and npm run build:js');
  process.exit(1);
}
if (!existsSync(join(root, 'examples', 'react', 'bundle.js'))) {
  console.error('examples/react/bundle.js is missing — run node examples/build-react.mjs');
  process.exit(1);
}

await rm(out, { recursive: true, force: true });
await mkdir(join(out, 'vanilla'), { recursive: true });
await mkdir(join(out, 'react'), { recursive: true });

await cp(join(root, 'dist'), join(out, 'dist'), { recursive: true });
await cp(join(root, 'test', 'fixtures', 'reader-sample.pdf'), join(out, 'reader-sample.pdf'));

await writeFile(join(out, 'vanilla', 'index.html'), rewrite(await readFile(join(root, 'examples', 'vanilla', 'index.html'), 'utf8')));
await writeFile(join(out, 'react', 'index.html'), rewrite(await readFile(join(root, 'examples', 'react', 'index.html'), 'utf8')));
await writeFile(join(out, 'react', 'bundle.js'), rewrite(await readFile(join(root, 'examples', 'react', 'bundle.js'), 'utf8')));

await writeFile(join(out, 'index.html'), `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>wasm-pdf-reader — a PDF reader in WebAssembly</title>
<link rel="stylesheet" href="./dist/styles.css">
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body {
    margin: 0; min-height: 100vh; display: grid; place-items: center;
    background: radial-gradient(1200px 600px at 20% -10%, #1e293b, #0b0f19 60%);
    color: #e8ecf4; font: 15px/1.55 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
  }
  main { max-width: 720px; padding: 48px 24px; }
  h1 { font-size: 30px; margin: 0 0 8px; letter-spacing: -0.01em; }
  h1 span { color: #7dd3fc; }
  p.lead { color: #94a3b8; margin: 0 0 28px; }
  .cards { display: grid; gap: 14px; grid-template-columns: 1fr 1fr; }
  a.card {
    display: block; padding: 18px; border-radius: 12px; text-decoration: none;
    color: inherit; background: #111827; border: 1px solid #1f2937;
    transition: border-color .15s ease, transform .15s ease;
  }
  a.card:hover { border-color: #38bdf8; transform: translateY(-1px); }
  a.card strong { display: block; font-size: 16px; margin-bottom: 4px; }
  a.card small { color: #94a3b8; }
  pre { background: #0f172a; border: 1px solid #1f2937; border-radius: 10px; padding: 14px; overflow: auto; font-size: 12.5px; }
  footer { margin-top: 28px; color: #64748b; font-size: 12.5px; }
  @media (max-width: 560px) { .cards { grid-template-columns: 1fr; } }
</style>
</head>
<body>
<main>
  <h1>wasm-pdf-reader <span>· live demo</span></h1>
  <p class="lead">Parse, rasterize and extract text from a PDF entirely in the browser tab — no server, no CDN, no upload.</p>
  <div class="cards">
    <a class="card" href="./vanilla/"><strong>Vanilla JS →</strong><small>mountPdfReader(el, options)</small></a>
    <a class="card" href="./react/"><strong>React →</strong><small>&lt;PdfReader /&gt; with a custom toolbar</small></a>
  </div>
  <pre>npm install wasm-pdf-reader
import { PdfDocument } from 'wasm-pdf-reader';

const doc = await PdfDocument.open(file);
await doc.renderToCanvas(canvas, 0, { scale: 1.5 });</pre>
  <footer>Built from the generated 3-page sample · C++ compiled with Emscripten</footer>
</main>
</body>
</html>
`);

console.log('demo built:', out.replace(root + '/', '') + '/');
