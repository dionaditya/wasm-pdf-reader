// memory.mjs — soak the engine and assert the wasm heap plateaus.
//
//   node --expose-gc test/memory.mjs
//
// The wasm heap only ever grows in Emscripten (it is a capacity, not a live
// figure), so a leak shows up as steady growth that never levels off. This
// exercises the paths that allocate the most: page rasterisation (one RGBA
// buffer per page plus the image decoders), repeated open/close of a document,
// and text extraction. A document leaking decoded images the way mupdf.js #200
// does would fail the plateau assertion after a few rounds.

import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadModule, PdfDocument } from '../dist/index.js';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const Module = await loadModule();
const heapMB = () => Module.HEAPU8.length / 1048576;

async function settle() {
  globalThis.gc?.();
  await new Promise((r) => setTimeout(r, 20));
  globalThis.gc?.();
  await new Promise((r) => setTimeout(r, 20));
}

async function soak(label, rounds, fn) {
  const sizes = [];
  await fn(0);                       // warm up: caches, wasm heap growth
  await settle();
  sizes.push(heapMB());
  for (let i = 1; i < rounds; i++) {
    await fn(i);
    await settle();
    sizes.push(heapMB());
  }
  const tail = sizes.slice(Math.floor(sizes.length / 2));
  const growth = Math.max(...tail) - Math.min(...tail);
  console.log(
    `  ${label.padEnd(34)} ${sizes.map((m) => m.toFixed(0)).join(' -> ')} MB` +
    `   (tail drift ${growth.toFixed(1)} MB)`,
  );
  return growth;
}

const scan = await readFile(join(root, 'test', 'fixtures', 'jb2globals.pdf'));     // 1 page, JBIG2
const cjk = await readFile(join(root, 'test', 'fixtures', 'cjk-cmaps.pdf'));       // 3 pages, CMaps
const sample = await readFile(join(root, 'test', 'fixtures', 'reader-sample.pdf'));

console.log('wasm heap after each round (MB):');

const openScan = await PdfDocument.open(scan);
const renderScan = await soak('render a JBIG2 scan at 2x', 8, async () => {
  const { imageData } = await openScan.render(0, { scale: 2 });
  assert.ok(imageData.width > 100);
});
openScan.close();

const openCjk = await PdfDocument.open(cjk);
const renderCjk = await soak('render all 3 CMap pages', 8, async () => {
  for (let i = 0; i < openCjk.pageCount; i++) await openCjk.render(i, { scale: 1 });
});
const textCjk = await soak('extract text of every page', 8, () => {
  for (let i = 0; i < openCjk.pageCount; i++) openCjk.getPageText(i);
});
openCjk.close();

const reopen = await soak('open + render + close', 10, async () => {
  const doc = await PdfDocument.open(sample);
  for (let i = 0; i < doc.pageCount; i++) await doc.render(i, { scale: 1 });
  doc.getPageText(0);
  doc.close();
});

// Deliberately hold the pixels the same way a viewer does (one per page) and
// drop them between rounds: the JS side must be collectable, not pinned by us.
let held = [];
const heldRounds = await soak('hold page pixels, then drop', 8, async (i) => {
  const doc = await PdfDocument.open(sample);
  held = [];
  for (let p = 0; p < doc.pageCount; p++) held.push((await doc.render(p, { scale: 1 })).imageData.data);
  doc.close();
  if (i > 0) held = [];
});

const worst = Math.max(renderScan, renderCjk, textCjk, reopen, heldRounds);
console.log(`\nheap: ${heapMB().toFixed(0)} MB, rss: ${(process.memoryUsage().rss / 1048576).toFixed(0)} MB`);

try {
  assert.ok(worst < 12, `wasm heap did not plateau (worst tail drift ${worst.toFixed(1)} MB)`);
  console.log('ok  wasm heap plateaus on every path (no leaked pages or decoded images)');
} catch (err) {
  console.error(`FAIL ${err.message}`);
  process.exitCode = 1;
}
