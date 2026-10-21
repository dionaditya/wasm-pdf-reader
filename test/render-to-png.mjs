// render-to-png.mjs <pdf> <page> <scale> <out.png> — dev helper: wasm pixels -> PNG.
import { readFileSync, writeFileSync } from 'node:fs';
import { deflateSync } from 'node:zlib';
import createModule from '../dist/wasm/pdf-reader.mjs';

const [, , pdfPath, pageArg = '0', scaleArg = '1.5', outPath = 'page.png'] = process.argv;
const bytes = readFileSync(pdfPath);
const M = await createModule({});
const ptr = M._wpdf_alloc(bytes.length);
M.HEAPU8.set(bytes, ptr);
const h = M._wpdf_open(ptr, bytes.length);
M._wpdf_free(ptr);
if (!h) throw new Error('open failed');
if (!M._wpdf_render_page(h, Number(pageArg), Number(scaleArg))) throw new Error('render failed');
const w = M._wpdf_bitmap_width();
const hgt = M._wpdf_bitmap_height();
const rgba = Buffer.from(new Uint8Array(M.HEAPU8.buffer, M._wpdf_bitmap_ptr(), w * hgt * 4));

// PNG encoder (RGBA8, filter 0)
const raw = Buffer.alloc((w * 4 + 1) * hgt);
for (let y = 0; y < hgt; y++) {
  raw[y * (w * 4 + 1)] = 0;
  rgba.copy(raw, y * (w * 4 + 1) + 1, y * w * 4, (y + 1) * w * 4);
}
const crcTable = Array.from({ length: 256 }, (_, n) => {
  let c = n;
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  return c >>> 0;
});
const crc32 = (buf) => {
  let c = 0xffffffff;
  for (const b of buf) c = crcTable[(c ^ b) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
};
const chunk = (type, data) => {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length);
  const body = Buffer.concat([Buffer.from(type, 'latin1'), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(body));
  return Buffer.concat([len, body, crc]);
};
const ihdr = Buffer.alloc(13);
ihdr.writeUInt32BE(w, 0);
ihdr.writeUInt32BE(hgt, 4);
ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
const png = Buffer.concat([
  Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
  chunk('IHDR', ihdr),
  chunk('IDAT', deflateSync(raw, { level: 9 })),
  chunk('IEND', Buffer.alloc(0)),
]);
writeFileSync(outPath, png);
console.log(`${outPath} ${w}x${hgt}`);
