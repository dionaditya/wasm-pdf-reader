// probe.mjs — quick manual check of the wasm ABI from node.
import { readFileSync } from 'node:fs';
import createModule from '../dist/wasm/pdf-reader.mjs';

const bytes = readFileSync(new URL('../test/fixtures/reader-sample.pdf', import.meta.url));
const M = await createModule({});

const ptr = M._wpdf_alloc(bytes.length);
M.HEAPU8.set(bytes, ptr);
const h = M._wpdf_open(ptr, bytes.length);
M._wpdf_free(ptr);
console.log('handle', h, 'pages', M._wpdf_page_count(h));
console.log('info', M.UTF8ToString(M._wpdf_info_json(h)));
console.log('warnings', M.UTF8ToString(M._wpdf_warnings_json(h)));

const sz = M._wpdf_alloc(5 * 8);
for (let i = 0; i < M._wpdf_page_count(h); i++) {
  M._wpdf_page_size(h, i, sz);
  console.log('size', i, Array.from(new Float64Array(M.HEAPU8.buffer, sz, 5)));
}
M._wpdf_free(sz);

const t0 = performance.now();
console.log('render', M._wpdf_render_page(h, 0, 1.5), M._wpdf_bitmap_width(), M._wpdf_bitmap_height(), Math.round(performance.now() - t0) + 'ms');
console.log('text', M.UTF8ToString(M._wpdf_page_text_json(h, 0)).slice(0, 300));
console.log('stats', (() => { const p = M._wpdf_alloc(7 * 4); const n = M._wpdf_page_stats(h, 0, p, 7); const v = Array.from(new Int32Array(M.HEAPU8.buffer, p, n)); M._wpdf_free(p); return v; })());
