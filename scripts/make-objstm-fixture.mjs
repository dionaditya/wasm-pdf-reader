#!/usr/bin/env node
// make-objstm-fixture.mjs — a second fixture, shaped like a calibre export.
//
// Tiny, but it carries the two structures the ROS 2 book hit:
//   * the catalog, page tree, page dicts and font live in an object stream
//     whose /First is not zero (offsets in the header are relative to /First)
//   * the cross-reference table is an /XRef stream with a PNG predictor
//
// Run `node scripts/make-objstm-fixture.mjs` to regenerate.
// Output: test/fixtures/objstm-sample.pdf

import { deflateSync } from 'node:zlib';
import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const outFile = join(here, '..', 'test', 'fixtures', 'objstm-sample.pdf');

const W = 612;
const H = 792;
const OBJSTM_NUM = 8;
const XREF_NUM = 9;

const content = (str) => `BT /F1 24 Tf 72 700 Td (${str}) Tj ET\n0.20 0.40 0.90 rg 72 620 240 40 re f\n`;

// Objects 1..5 live inside the object stream, in this order.
const compressed = [
  [1, '<< /Type /Catalog /Pages 2 0 R >>'],
  [2, '<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>'],
  [3, `<< /Type /Page /Parent 2 0 R /MediaBox [0 0 ${W} ${H}] /Resources << /Font << /F1 5 0 R >> >> /Contents 6 0 R >>`],
  [4, `<< /Type /Page /Parent 2 0 R /MediaBox [0 0 ${W} ${H}] /Resources << /Font << /F1 5 0 R >> >> /Contents 7 0 R >>`],
  [5, '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>'],
];

// header is "num offset num offset ...", where offset counts from /First
let bodies = '';
const pairs = [];
for (const [num, body] of compressed) {
  pairs.push([num, Buffer.byteLength(bodies, 'latin1')]);
  bodies += `${body}\n`;
}
const header = `${pairs.map(([n, o]) => `${n} ${o}`).join(' ')}\n`;
const first = Buffer.byteLength(header, 'latin1');
const objStmRaw = Buffer.from(header + bodies, 'latin1');

const streams = [
  [6, content('Object streams')],
  [7, content('Second page, also from the ObjStm')],
];

// ── serialise ───────────────────────────────────────────────────────────────
let pdf = '%PDF-1.7\n%\xE2\xE3\xCF\xD3\n';
const offsets = new Map();

const push = (num, dict, raw) => {
  offsets.set(num, Buffer.byteLength(pdf, 'latin1'));
  pdf += `${num} 0 obj\n${dict}\n`;
  if (raw) {
    pdf += 'stream\n';
    pdf += raw.toString('latin1');
    pdf += '\nendstream\n';
  }
  pdf += 'endobj\n';
};

for (const [num, text] of streams) {
  const z = deflateSync(Buffer.from(text, 'latin1'));
  push(num, `<< /Length ${z.length} /Filter /FlateDecode >>`, z);
}

const objStmZ = deflateSync(objStmRaw);
push(OBJSTM_NUM, `<< /Type /ObjStm /N ${compressed.length} /First ${first} /Length ${objStmZ.length} /Filter /FlateDecode >>`, objStmZ);

// ── xref stream: /W [1 4 2], PNG Up predictor ───────────────────────────────
const size = XREF_NUM + 1;
const rows = [];
rows.push(Buffer.from([0, 0, 0, 0, 0, 0xff, 0xff]));          // object 0: free

compressed.forEach(([num], index) => {
  const row = Buffer.alloc(7);
  row[0] = 2;                        // in an object stream
  row.writeUInt32BE(OBJSTM_NUM, 1);
  row.writeUInt16BE(index, 5);       // index inside that stream
  void num;
  rows.push(row);
});

for (let num = 6; num <= XREF_NUM; num++) {
  const row = Buffer.alloc(7);
  row[0] = 1;                        // byte offset in the file
  row.writeUInt32BE(offsets.get(num), 1);
  row.writeUInt16BE(0, 5);
  rows.push(row);
}

const encoded = [];
let prev = Buffer.alloc(7);
for (const row of rows) {
  const out = Buffer.alloc(8);
  out[0] = 2;                        // PNG filter type 2 (Up)
  for (let i = 0; i < 7; i++) out[i + 1] = (row[i] - prev[i]) & 0xff;
  encoded.push(out);
  prev = row;
}
const xrefZ = deflateSync(Buffer.concat(encoded));

push(XREF_NUM,
  `<< /Type /XRef /Size ${size} /Index [0 ${size}] /W [1 4 2] /Root 1 0 R ` +
  `/Filter /FlateDecode /DecodeParms << /Predictor 12 /Columns 7 >> /Length ${xrefZ.length} >>`,
  xrefZ);

pdf += `startxref\n${offsets.get(XREF_NUM)}\n%%EOF\n`;

mkdirSync(dirname(outFile), { recursive: true });
writeFileSync(outFile, Buffer.from(pdf, 'latin1'));

const expectations = {
  pages: 2,
  pageText: {
    0: ['Object streams'],
    1: ['Second page, also from the ObjStm'],
  },
};
writeFileSync(join(dirname(outFile), 'objstm-sample.json'), `${JSON.stringify(expectations, null, 2)}\n`);
console.log(`wrote ${outFile}`);
