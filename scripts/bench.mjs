#!/usr/bin/env node
// bench.mjs — render throughput and fidelity numbers you can reproduce.
//
//   node scripts/bench.mjs [--json]
//
// Renders each committed fixture at 100 dpi (the resolution the Alfresco
// engine study used), reports milliseconds per page, and prints the MuPDF
// fidelity gate next to it. Two things this deliberately is not:
//
//   * not a cross-engine benchmark — pdf.js and MuPDF are not run here. The
//     engine-comparison studies that exist (Alfresco 2018, 3071 documents /
//     17,226 pages) measured server-side native rasterizers on a corpus we do
//     not have. Use their ordering only to know what to expect, never as our
//     result.
//   * not a browser measurement — this is Node, one thread, no worker, no
//     canvas paint. `test/e2e.mjs` is what proves the browser path.
//
// What it is: our own regression number. A doubling here means a renderer bug,
// not a slow machine, if the same fixture moved.

import { spawnSync } from 'node:child_process';
import { readFile } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { PdfDocument } from '../dist/index.js';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const SCALE = 100 / 72;              // 100 dpi
const WARM = 1;                      // renders discarded before timing
const RUNS = 3;

const FIXTURES = [
  ['reader-sample.pdf', 'text + vector + image, 3 pages'],
  ['patterns.pdf', 'tiling patterns'],
  ['mesh.pdf', 'mesh shadings (4/5, unverified)'],
  ['jb2globals.pdf', 'JBIG2 scan with a global symbol dictionary'],
  ['type3.pdf', 'Type3 glyph procedures'],
  ['cjk-cmaps.pdf', 'CID fonts through predefined CMaps'],
  ['annot-quads.pdf', 'markup annotations with /QuadPoints'],
];

const json = process.argv.includes('--json');
const rows = [];

for (const [name, label] of FIXTURES) {
  let bytes;
  try {
    bytes = await readFile(join(root, 'test', 'fixtures', name));
  } catch {
    rows.push({ fixture: name, note: 'missing fixture' });
    continue;
  }
  const doc = await PdfDocument.open(bytes, { filename: name });
  const pageCount = doc.pageCount;              // cached before close()
  const pages = Math.min(pageCount, 12);
  const size = doc.getPageSize(0);
  const pixelWidth = Math.round(size.width * SCALE);
  const pixelHeight = Math.round(size.height * SCALE);

  let openMs = 0;
  for (let i = 0; i < WARM; i++) {
    const t = performance.now();
    await doc.render(0, { scale: SCALE });
    openMs = performance.now() - t;
  }
  const runs = [];
  for (let r = 0; r < RUNS; r++) {
    const started = performance.now();
    for (let i = 0; i < pages; i++) await doc.render(i, { scale: SCALE });
    runs.push((performance.now() - started) / pages);
  }
  runs.sort((a, b) => a - b);
  doc.close();

  rows.push({
    fixture: name,
    label,
    pages: pageCount,
    pixels: `${pixelWidth}x${pixelHeight}`,
    msPerPage: Number(runs[Math.floor(runs.length / 2)].toFixed(1)),
    firstPageIncludingOpenMs: Math.round(openMs),
  });
}

if (json) {
  console.log(JSON.stringify({ scale: SCALE, dpi: 100, rows }, null, 2));
} else {
  console.log(`100 dpi, ${process.versions.node}, single thread, wasm — ms per page (median of ${RUNS})`);
  console.log('');
  console.log('fixture              pages   bitmap        ms/page   first page (incl. open)   what it is');
  for (const r of rows) {
    if (r.note) { console.log(`${r.fixture.padEnd(20)} ${r.note}`); continue; }
    console.log(
      `${r.fixture.padEnd(20)} ${String(r.pages).padStart(5)}   ${r.pixels.padEnd(12)}  `
      + `${String(r.msPerPage).padStart(7)}   ${String(r.firstPageIncludingOpenMs).padStart(8)} ms              ${r.label}`,
    );
  }
  console.log('');
  console.log('Fidelity is a separate gate: ~/pdfvenv/bin/python scripts/compare_render.py <fixture> 1');
  console.log('(text pages <= 1/255 mean abs diff vs MuPDF 1.28, scans <= 3/255, ink within 2%).');
  console.log('Cross-engine ordering: do not infer it from these numbers — see docs/BENCHMARK.md.');
}
