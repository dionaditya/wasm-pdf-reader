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

// ── object streams whose data starts at /First, not at byte 0 ───────────────
// calibre, Acrobat and Ghostscript all write a non-zero /First. Reading the
// header as object data yields a one-page document of garbage.
const objStmPath = join(root, 'test', 'fixtures', 'objstm-sample.pdf');
const objStmExpect = JSON.parse(await readFile(join(root, 'test', 'fixtures', 'objstm-sample.json'), 'utf8'));
const objStmBytes = await readFile(objStmPath);
const objStmDoc = await PdfDocument.open(objStmBytes, { filename: 'objstm-sample.pdf' });

await check('object stream: catalog and page tree resolve through /First', () => {
  assert.equal(objStmDoc.pageCount, objStmExpect.pages);
  assert.equal(objStmDoc.warnings.length, 0);
});

await check('object stream: geometry and text come from compressed objects', () => {
  const size = objStmDoc.getPageSize(0);
  assert.equal(Math.round(size.width), 612);
  assert.equal(Math.round(size.height), 792);
  objStmExpect.pageText[1].forEach((needle) => {
    assert.ok(objStmDoc.getPageText(1).includes(needle), `page 1 should contain ${needle}`);
  });
});

await check('object stream: pages rasterize with real ink', async () => {
  for (const page of [0, 1]) {
    const { imageData } = await objStmDoc.render(page, { scale: 0.5 });
    assert.ok(inkRatio(imageData) > 0.005, `page ${page} should have ink`);
  }
});
objStmDoc.close();


// ── link annotations ───────────────────────────────────────────────────────
const linksDoc = await PdfDocument.open(await readFile(join(root, 'test', 'fixtures', 'links.pdf')));

await check('links: /GoTo and /URI annotations are reported', () => {
  const links = linksDoc.getLinks(0);
  assert.equal(links.length, 2);
  const goto = links.find((l) => l.page !== undefined);
  const uri = links.find((l) => l.uri);
  assert.equal(goto.page, 2, 'internal link targets page 3');
  assert.equal(uri.uri, 'https://example.com/');
  assert.ok(goto.x1 > goto.x0 && goto.y1 > goto.y0, 'rect has area');
});

await check('links: named destination resolves through /Names /Dests', () => {
  const links = linksDoc.getLinks(1);
  assert.equal(links.length, 1);
  assert.equal(links[0].page, 2);
});

await check('links: pages without annotations report none', () => {
  assert.deepEqual(linksDoc.getLinks(2), []);
});
linksDoc.close();

// ── annotation fallbacks: /QuadPoints marks and note icons without /AP ─────
const annotDoc = await PdfDocument.open(await readFile(join(root, 'test', 'fixtures', 'annot-quads.pdf')));

function countPixels(imageData, predicate) {
  const { data, width, height } = imageData;
  let n = 0;
  for (let i = 0; i < width * height; i++) {
    const r = data[i * 4], g = data[i * 4 + 1], b = data[i * 4 + 2];
    if (predicate(r, g, b)) n++;
  }
  return n;
}

await check('annotations: /QuadPoints paint highlight, underline, strikeout, squiggly', async () => {
  const { imageData } = await annotDoc.render(0, { scale: 1 });
  const yellow = countPixels(imageData, (r, g, b) => r > 200 && g > 200 && b < 215 && b > 90);
  const red = countPixels(imageData, (r, g, b) => r > 140 && g < 110 && b < 110);
  const blue = countPixels(imageData, (r, g, b) => b > 140 && r < 110 && g < 110);
  const green = countPixels(imageData, (r, g, b) => g > 90 && r < 110 && b < 110);
  assert.ok(yellow > 4000, `highlight should fill two quads, got ${yellow} px`);
  assert.ok(red > 250, `underline should be a red bar, got ${red} px`);
  assert.ok(blue > 250, `strikeout should be a blue bar, got ${blue} px`);
  assert.ok(green > 250, `squiggly should be a green zigzag, got ${green} px`);

  // the red underline belongs under the third line: device y = pageHeight - baseline
  const { data, width, height } = imageData;
  let minY = height, maxY = 0;
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const i = (y * width + x) * 4;
      if (data[i] > 140 && data[i + 1] < 110 && data[i + 2] < 110) {
        if (y < minY) minY = y;
        if (y > maxY) maxY = y;
      }
    }
  }
  assert.ok(Math.abs(minY - (792 - 656)) <= 4, `underline sits at the text baseline, got y=${minY}`);
});

await check('annotations: text notes draw an icon from /Rect alone', async () => {
  const { imageData } = await annotDoc.render(1, { scale: 1 });
  const { width, data } = imageData;
  for (let i = 0; i < 3; i++) {
    // note rect is [460, 690-40i, 480, 710-40i] in page space, 20x20
    const px0 = 458, px1 = 482;
    const py0 = 792 - 712 - i * -40, py1 = 792 - 688 - i * -40;
    let ink = 0;
    for (let y = Math.min(py0, py1); y < Math.max(py0, py1); y++) {
      for (let x = px0; x < px1; x++) {
        const j = (y * width + x) * 4;
        if (data[j] < 245 || data[j + 1] < 245 || data[j + 2] < 245) ink++;
      }
    }
    assert.ok(ink > 80, `note ${i} should have an icon, got ${ink} px`);
  }
});

await check('annotations: an /AP appearance wins over the /QuadPoints fallback', async () => {
  const { imageData } = await annotDoc.render(2, { scale: 1 });
  // the quad covers 72..480 x 620..700; a fallback fill would be a huge band
  const yellow = countPixels(imageData, (r, g, b) => r > 200 && g > 200 && b < 200);
  assert.ok(yellow < 200, `the quad must not be filled when /AP exists, got ${yellow} px`);
  assert.equal(imageData.width, 612);
});

await check('annotations: marks follow the page CTM on a rotated page', async () => {
  const { imageData } = await annotDoc.render(3, { scale: 1 });
  assert.equal(imageData.width, 792, 'rotated page swaps its dimensions');
  assert.equal(imageData.height, 612);
  const cyan = countPixels(imageData, (r, g, b) => b > 200 && r < 220 && g > 180);
  assert.ok(cyan > 4000, `highlight should cover the rotated line, got ${cyan} px`);
});
annotDoc.close();

// ── CMaps: predefined tables and embedded CMap streams (CJK) ───────────────
const cjkDoc = await PdfDocument.open(await readFile(join(root, 'test', 'fixtures', 'cjk-cmaps.pdf')));

function inkIn(imageData, x0, y0, x1, y1) {
  const { data, width, height } = imageData;
  let n = 0;
  for (let y = Math.max(0, y0); y < Math.min(height, y1); y++) {
    for (let x = Math.max(0, x0); x < Math.min(width, x1); x++) {
      const i = (y * width + x) * 4;
      if (data[i] < 245 || data[i + 1] < 245 || data[i + 2] < 245) n++;
    }
  }
  return n;
}

await check('CMaps: /UniJIS-UCS2-H resolves codes to CIDs and glyphs', async () => {
  const text = cjkDoc.getPageText(0);
  assert.ok(text.includes('日本語のテキスト'), `page text should decode, got ${JSON.stringify(text.slice(0, 40))}`);
  const { imageData } = await cjkDoc.render(0, { scale: 1 });
  const ink = inkIn(imageData, 70, 60, 300, 110);
  assert.ok(ink > 400, `the CJK glyph run should have ink, got ${ink} px`);
});

await check('CMaps: an embedded CMap stream inherits from usecmap', async () => {
  const { imageData } = await cjkDoc.render(1, { scale: 1 });
  // あ (inherited U+3042) + 漢 and 語 (overridden codes) sit at 24 pt
  const ink = inkIn(imageData, 70, 60, 200, 110);
  assert.ok(ink > 400, `three CJK glyphs should have ink, got ${ink} px`);
  assert.ok(!cjkDoc.warnings.some((w) => w.includes('CMap')), `unexpected warnings: ${cjkDoc.warnings}`);
});

await check('CMaps: mixed 1-byte and 2-byte codespaces decode in order', async () => {
  const { imageData } = await cjkDoc.render(2, { scale: 1 });
  // (A) is a one-byte code for 日, <8141> a two-byte code for は
  const first = inkIn(imageData, 70, 60, 96, 110);
  const second = inkIn(imageData, 96, 60, 130, 110);
  assert.ok(first > 100, `日 should paint in the first cell, got ${first} px`);
  assert.ok(second > 100, `は should paint in the second cell, got ${second} px`);
});
cjkDoc.close();

// ── API contract: nothing that promises a value may return undefined ───────
// The mupdf.js #199 shape — a binding that calls into wasm but forgets to
// return the result — is invisible until a caller branches on it, so pin every
// query on a real document to its declared shape.
await check('api: every query returns a value of its declared type', async () => {
  const sample = await readFile(join(root, 'test', 'fixtures', 'reader-sample.pdf'));
  const doc = await PdfDocument.open(sample);
  const checks = [
    ['pageCount', doc.pageCount, 'number', 3],
    ['getPageSize', doc.getPageSize(0), 'object', null],
    ['getText', doc.getText(0), 'object', null],
    ['getPageText', doc.getPageText(0), 'string', null],
    ['getAllText', doc.getAllText(), 'object', null],
    ['search', doc.search('the'), 'object', null],
    ['getLinks', doc.getLinks(0), 'object', null],
    ['metadata', doc.metadata, 'object', null],
    ['warnings', doc.warnings, 'object', null],
    ['stats', doc._stats(0), 'object', null],
    ['boxToCss', doc.boxToCss({ x0: 0, y0: 0, x1: 10, y1: 10 }, 0, 1), 'object', null],
  ];
  for (const [name, value, type, exact] of checks) {
    assert.notEqual(value, undefined, `${name}() returned undefined`);
    assert.equal(typeof value, type, `${name}() should be a ${type}, got ${typeof value}`);
    if (exact !== null) assert.equal(value, exact);
  }
  const size = doc.getPageSize(0);
  for (const k of ['width', 'height', 'rotation']) {
    assert.equal(typeof size[k], 'number', `getPageSize().${k} should be a number`);
  }
  assert.ok(typeof doc.pageCount === 'number' && doc.pageCount > 0);
  assert.equal(typeof (await import('../dist/index.js')).version, 'string', 'version should be a string');
  doc.close();
});

// ── shared indirect objects (the reading side of a stream must survive reuse) ─
const sharedDoc = await PdfDocument.open(await readFile(join(root, 'test', 'fixtures', 'shared-xobject.pdf')));

await check('shared image and form XObjects resolve on every page', async () => {
  assert.equal(sharedDoc.pageCount, 2);
  const shots = [];
  for (let i = 0; i < 2; i++) {
    const { imageData } = await sharedDoc.render(i, { scale: 1 });
    shots.push(imageData);
    assert.ok(inkRatio(imageData) > 0.05, `page ${i} should draw the shared image, got ink ${inkRatio(imageData)}`);
  }
  // Same streams, same pixels: a stream that lost its bytes on the second visit
  // would show up here as a blank half-page.
  const a = shots[0].data, b = shots[1].data;
  assert.equal(a.length, b.length);
  for (let i = 0; i < a.length; i++) {
    if (a[i] !== b[i]) assert.fail(`page 1 differs from page 0 at byte ${i}`);
  }
});
sharedDoc.close();

// ── encryption: every revision, user and owner ─────────────────────────────
// Fixtures from scripts/make_encrypted_fixtures.py; the derived keys are
// compared with `qpdf --show-encryption-key` by scripts/verify_encryption.sh.
const USER_PW = 'reader-fixture';
const OWNER_PW = 'owner-fixture';
const encRevs = [
  ['enc-r2-rc4.pdf', 'R2 RC4-40'],
  ['enc-r4-rc4.pdf', 'R4 RC4-128'],
  ['enc-r4-aes128.pdf', 'R4 AES-128'],
  ['enc-r5-aes256.pdf', 'R5 AES-256'],
  ['enc-r6-aes256.pdf', 'R6 AES-256'],
];
for (const [file, label] of encRevs) {
  const bytes = await readFile(join(root, 'test', 'fixtures', file));
  await check(`${label}: user password decrypts text and pixels`, async () => {
    const doc = await PdfDocument.open(bytes, { password: USER_PW });
    assert.ok(doc.getPageText(0).includes('Encryption fixture'), 'text should decrypt');
    const { imageData } = await doc.render(0, { scale: 1 });
    assert.ok(inkRatio(imageData) > 0.002, `page should have ink, got ${inkRatio(imageData)}`);
    assert.equal(doc.warnings.length, 0, `unexpected warnings: ${doc.warnings}`);
    doc.close();
  });
  await check(`${label}: owner password opens the same bytes`, async () => {
    const doc = await PdfDocument.open(bytes, { password: OWNER_PW });
    assert.ok(doc.getPageText(0).includes('Encryption fixture'));
    doc.close();
  });
  await check(`${label}: a wrong password is refused by name`, async () => {
    await assert.rejects(() => PdfDocument.open(bytes, { password: 'nope' }), /wrong password/);
  });
}

// Owner-password-only file: the empty password is the user password. This is the
// case that exposed an Algorithm 2.B bug — the owner path could not match /O, so
// "owner password support" was only true for files that also had a user password.
const ownerOnly = await readFile(join(root, 'test', 'fixtures', 'enc-r6-aes256-user-only.pdf'));
await check('R6 owner-only: opens with no user password and with the owner password', async () => {
  for (const pw of ['', OWNER_PW]) {
    const doc = await PdfDocument.open(ownerOnly, { password: pw });
    assert.ok(doc.getPageText(0).includes('Encryption fixture'), `password ${pw || '(empty)'} should open`);
    doc.close();
  }
  // The empty password *is* the user password here, so opening without one works.
  await assert.rejects(() => PdfDocument.open(ownerOnly, { password: 'nope' }), /wrong password/);
});

await check('R6: a corrupt /Perms rejects the owner password, not the user one', async () => {
  const bytes = await readFile(join(root, 'test', 'fixtures', 'enc-r6-aes256-badperms.pdf'));
  // /Perms only proves the *owner* key; the user path is unaffected.
  const asUser = await PdfDocument.open(bytes, { password: USER_PW });
  assert.ok(asUser.getPageText(0).includes('Encryption fixture'));
  asUser.close();
  await assert.rejects(
    () => PdfDocument.open(bytes, { password: OWNER_PW }),
    /wrong password/,
    'the owner path must refuse when /Perms does not decrypt to the marker',
  );
});

// ─── tiling patterns and transparency groups ────────────────────────────────
// Both fixtures are hand-built by scripts/make_pattern_fixtures.py so the
// geometry is known exactly: a 45-degree checker over 260x260 points, a hatch
// under text, a Multiply group, a /ca 0.5 group and a transparent CMYK fill.
const patBytes = await readFile(join(root, 'test', 'fixtures', 'patterns.pdf'));
const patDoc = await PdfDocument.open(patBytes, { filename: 'patterns.pdf' });

await check('tiling pattern: 45-degree checker fills the painted rectangle', async () => {
  const out = await patDoc.render(0, { scale: 1.5 });
  const ink = inkRatio(out.imageData);
  assert.ok(ink > 0.38 && ink < 0.47, `ink ${ink.toFixed(3)} should be ~0.43`);
});

await check('tiling pattern: hatch fills under text and the text stays black', async () => {
  const out = await patDoc.render(1, { scale: 1.5 });
  const { data, width, height } = out.imageData;
  // the heading sits near the top; count dark pixels in that band
  let dark = 0;
  for (let y = 90; y < 170; y++) {
    for (let x = 0; x < width; x++) {
      const i = (y * width + x) * 4;
      if (data[i] < 90 && data[i + 1] < 90 && data[i + 2] < 90) dark++;
    }
  }
  assert.ok(dark > 100, `heading pixels ${dark}`);
  const ink = inkRatio(out.imageData);
  assert.ok(ink > 0.06 && ink < 0.15, `ink ${ink.toFixed(3)} should be ~0.10`);
});
patDoc.close();

const trBytes = await readFile(join(root, 'test', 'fixtures', 'transparency.pdf'));
const trDoc = await PdfDocument.open(trBytes, { filename: 'transparency.pdf' });

function pixel(imageData, x, y) {
  const i = (y * imageData.width + x) * 4;
  return [imageData.data[i], imageData.data[i + 1], imageData.data[i + 2]];
}

await check('transparency: a Multiply group darkens the backdrop', async () => {
  const out = await trDoc.render(0, { scale: 1.5 });
  // x=300,y=300 lands inside the logo over the blue half: multiply, not replace
  const [r, g, b] = pixel(out.imageData, 300, 300);
  assert.ok(r < 60 && g < 80 && b > 80, `multiply pixel ${r},${g},${b}`);
});

await check('transparency: /ca 0.5 group blends halfway, not opaque', async () => {
  const out = await trDoc.render(1, { scale: 1.5 });
  const [r, g, b] = pixel(out.imageData, 120, 300);   // red over blue half
  assert.ok(r > 100 && r < 200 && b > 60, `/ca pixel ${r},${g},${b}`);
});
trDoc.close();

console.log(`\n${passed} checks passed${process.exitCode ? ', with failures' : ''}`);
