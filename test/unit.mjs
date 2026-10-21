// unit.mjs — core library checks that do not need a browser.
//
//   node test/unit.mjs
//
// Exercises the wasm ABI through the public PdfDocument API: parsing, page
// geometry, rasterizing, the text layer and search, plus the guarded failures
// JS callers rely on.

import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { PdfDocument, resetModule, version } from '../dist/index.js';
import { groupLines, itemsToText, searchItems } from '../dist/text.js';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const fixturePath = join(root, 'test', 'fixtures', 'reader-sample.pdf');
const expectations = JSON.parse(await readFile(join(root, 'test', 'fixtures', 'reader-sample.json'), 'utf8'));

let passed = 0;
function check(name, fn) {
  return Promise.resolve()
    .then(fn)
    .then(() => { passed++; console.log(`  ok  ${name}`); })
    .catch((err) => { console.error(`  FAIL ${name}\n       ${err.message}`); process.exitCode = 1; });
}

// coverage of non-white pixels, ignoring a 1px border where AA lives
function inkRatio(imageData) {
  const { data, width, height } = imageData;
  let ink = 0;
  for (let y = 1; y < height - 1; y++) {
    for (let x = 1; x < width - 1; x++) {
      const i = (y * width + x) * 4;
      if (data[i] < 245 || data[i + 1] < 245 || data[i + 2] < 245) ink++;
    }
  }
  return ink / (width * height);
}

console.log('wasm-pdf-reader unit tests');

resetModule();
const bytes = await readFile(fixturePath);
const doc = await PdfDocument.open(bytes, { filename: 'reader-sample.pdf' });

await check('version matches package.json', async () => {
  const pkg = JSON.parse(await readFile(join(root, 'package.json'), 'utf8'));
  assert.equal(version, pkg.version);
});

await check('page count', () => assert.equal(doc.pageCount, expectations.pages));

await check('page geometry including a /Rotate page', () => {
  expectations.pageSize.forEach(([w, h, rot], i) => {
    const size = doc.getPageSize(i);
    assert.equal(Math.round(size.width), w, `page ${i} width`);
    assert.equal(Math.round(size.height), h, `page ${i} height`);
    assert.equal(size.rotation, rot, `page ${i} rotation`);
  });
});

await check('document info dictionary', () => {
  const meta = doc.metadata;
  for (const [k, v] of Object.entries(expectations.metadata)) assert.equal(meta[k], v);
});

await check('no parse warnings for a clean file', () => assert.deepEqual(doc.warnings, []));

await check('rasterizes page 1 at 1.5x with real ink', async () => {
  const out = await doc.render(0, { scale: 1.5 });
  assert.equal(out.width, 918);
  assert.equal(out.height, 1188);
  assert.equal(out.imageData.data.length, 918 * 1188 * 4);
  const ratio = inkRatio(out.imageData);
  assert.ok(ratio > 0.02, `expected >2% ink, got ${(ratio * 100).toFixed(2)}%`);
  assert.ok(ratio < 0.9, `expected <90% ink, got ${(ratio * 100).toFixed(2)}%`);
});

await check('renders all three pages, each with its own content', async () => {
  const ratios = [];
  for (let i = 0; i < doc.pageCount; i++) {
    const out = await doc.render(i, { scale: 0.6 });
    ratios.push(inkRatio(out.imageData));
  }
  for (const [i, r] of ratios.entries()) assert.ok(r > 0.01, `page ${i} looks blank (${(r * 100).toFixed(2)}%)`);
  // different pages must not produce identical bitmaps
  const a = await doc.render(0, { scale: 0.3 });
  const b = await doc.render(2, { scale: 0.3 });
  assert.notDeepEqual(Array.from(a.imageData.data.slice(0, 4096)), Array.from(b.imageData.data.slice(0, 4096)));
});

await check('rotated page rasterizes at the display size the reader reports', async () => {
  const out = await doc.render(2, { scale: 1 });
  const size = doc.getPageSize(2);
  assert.equal(out.width, size.width);
  assert.equal(out.height, size.height);
  assert.equal(size.rotation, 90);
  // a 612 x 792 media box with /Rotate 90 displays as 792 x 612
  assert.equal(out.width, 792);
  assert.equal(out.height, 612);
});

await check('pixelTransform can post-process a page', async () => {
  const plain = await doc.render(0, { scale: 0.3 });
  const tinted = await doc.render(0, {
    scale: 0.3,
    pixelTransform: (data) => {
      for (let i = 0; i < data.length; i += 4) { data[i] = 0; data[i + 1] = 0; data[i + 2] = 255; }
    },
  });
  assert.notEqual(plain.imageData.data[0], tinted.imageData.data[0], 'pixelTransform had no effect');
  assert.equal(tinted.imageData.data[0], 0);
  assert.equal(tinted.imageData.data[2], 255);
});

await check('text layer matches the page content', () => {
  const text = doc.getPageText(0);
  for (const needle of expectations.pageText[0]) {
    assert.ok(text.includes(needle), `page 1 text is missing "${needle}"`);
  }
  for (let i = 0; i < doc.pageCount; i++) {
    const pageText = doc.getPageText(i);
    for (const needle of expectations.pageText[i]) {
      assert.ok(pageText.includes(needle), `page ${i + 1} text is missing "${needle}"`);
    }
  }
});

await check('glyph runs carry page-space boxes and sane advances', () => {
  const items = doc.getText(0);
  assert.ok(items.length > 100, `expected many glyph runs, got ${items.length}`);
  for (const it of items.slice(0, 50)) {
    assert.ok(it.x1 >= it.x0 && it.y1 >= it.y0, 'box is inverted');
    assert.ok(it.adv >= 0 && it.adv < 60, `advance out of range: ${it.adv}`);
    assert.ok(it.fs > 0 && it.fs <= 40, `font size out of range: ${it.fs}`);
  }
  const lines = groupLines(items);
  assert.ok(lines.length >= 8, `expected several lines, got ${lines.length}`);
  // lines are ordered top to bottom (superscripts may nudge the average)
  for (let i = 1; i < lines.length; i++) {
    assert.ok(lines[i].oy <= lines[i - 1].oy + 8, `line ${i} out of order: ${lines[i].oy} after ${lines[i - 1].oy}`);
  }
});

await check('search finds text on the right page and returns highlight rects', () => {
  const hits = doc.search('clipping');
  assert.ok(hits.length > 0, 'no hits');
  assert.equal(hits[0].page, 1);
  assert.ok(hits[0].rects.length > 0 && hits[0].rects[0].x1 > hits[0].rects[0].x0);
  assert.deepEqual(doc.search(''), []);
  assert.deepEqual(doc.search('a phrase that does not exist'), []);
});

await check('searchItems is case-insensitive by default', () => {
  const items = doc.getText(1);
  assert.ok(searchItems(items, 'IMAGES').length > 0);
  assert.equal(searchItems(items, 'IMAGES', { caseSensitive: true }).length, 0);
});

await check('itemsToText separates lines', () => {
  const text = itemsToText(doc.getText(2));
  assert.ok(text.split('\n').length > 2);
  assert.ok(text.includes('ROTATED'));
});

await check('unreadable input fails with a useful error', async () => {
  await assert.rejects(
    () => PdfDocument.open(new TextEncoder().encode('not a pdf at all')),
    /wasm-pdf-reader:/,
  );
});

await check('used document throws after close', () => {
  const copy = doc;
  copy.close();
  assert.throws(() => copy.pageCount, /closed/);
  assert.throws(() => copy.getPageSize(0), /closed/);
});

await check('a second document can be opened after the first is closed', async () => {
  const again = await PdfDocument.open(await readFile(fixturePath));
  assert.equal(again.pageCount, expectations.pages);
  const out = await again.render(1, { scale: 0.4 });
  assert.ok(out.width > 0);
  again.close();
});

console.log(`\n${passed} checks passed${process.exitCode ? ', with failures' : ''}`);
