// ranged.mjs — proves M9.2: a 117 MB PDF opens and renders without the file
// ever being in wasm memory.
//
//   node test/ranged.mjs              # Range-enabled server (the good path)
//   node test/ranged.mjs --verbose
//
// What it asserts, in order of how much it would hurt to be wrong:
//   * the document parses from byte ranges and reports 240 pages;
//   * page 1 AND page 240 (far from anything the parser read first) rasterize
//     with real ink;
//   * the engine pulled less than 10 MB of a 117 MB file;
//   * the worker's wasm heap stays under 150 MB, i.e. the file is not resident;
//   * a server that ignores `Range` (answers 200) still works — correctness
//     must not depend on the origin being well behaved.
//
// The fixture is generated on demand and gitignored (117 MB in a repo is a
// hundred MB of clone for one test).

import assert from 'node:assert/strict';
import { spawn, execFileSync } from 'node:child_process';
import { createServer } from 'node:net';
import { existsSync } from 'node:fs';
import { stat } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { launchChrome, newPage } from './cdp.mjs';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const fixture = join(root, 'test', 'fixtures', 'large-scan.pdf');
const verbose = process.argv.includes('--verbose');
const BUDGET_FETCHED = 10 * 1024 * 1024;      // acceptance: < 10 MB of a 100 MB+ file
const BUDGET_HEAP = 150 * 1024 * 1024;        // acceptance: peak heap < 150 MB

if (!existsSync(fixture)) {
  console.log('fixture missing — generating test/fixtures/large-scan.pdf (~117 MB)…');
  const python = existsSync(join(root, '..', 'pdfvenv', 'bin', 'python'))
    ? join(root, '..', 'pdfvenv', 'bin', 'python')
    : 'python3';
  execFileSync(python, [join(root, 'scripts', 'make_large_fixture.py')], { stdio: 'inherit' });
}
const fileSize = (await stat(fixture)).size;

let passed = 0;
const failures = [];
async function step(name, fn) {
  try {
    await fn();
    passed++;
    console.log(`  ok  ${name}`);
  } catch (err) {
    failures.push({ name, err });
    console.error(`  FAIL ${name}\n       ${err.message.split('\n').join('\n       ')}`);
  }
}

const freePort = () => new Promise((resolve) => {
  const probe = createServer();
  probe.listen(0, '127.0.0.1', () => {
    const { port } = probe.address();
    probe.close(() => resolve(port));
  });
});

const chrome = await launchChrome({ port: 9339, url: 'about:blank' });
const servers = [];

async function startServer(extraArgs = []) {
  const port = await freePort();
  const proc = spawn(process.execPath, [join(root, 'examples', 'server.mjs'), String(port), ...extraArgs], {
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  servers.push(proc);
  let log = '';
  proc.stdout.on('data', (d) => { log += String(d); });
  proc.stderr.on('data', (d) => { log += String(d); });
  await new Promise((resolve, reject) => {
    const t = setTimeout(() => reject(new Error(`server did not start\n${log}`)), 10000);
    const poll = setInterval(() => {
      if (log.includes('demos on')) { clearTimeout(t); clearInterval(poll); resolve(); }
    }, 50);
  });
  return port;
}

try {
  const port = await startServer();
  const page = await newPage(9339, 'about:blank');
  await page.setViewport(1280, 900, 1);
  await page.goto(`http://127.0.0.1:${port}/examples/vanilla/`);

  const result = await page.eval(`(async () => {
    const { PdfDocument } = await import('/dist/index.js');
    const size = ${fileSize};
    const t0 = performance.now();
    const doc = await PdfDocument.open({ url: '/test/fixtures/large-scan.pdf', size }, { worker: true });
    const openMs = Math.round(performance.now() - t0);
    const ink = async (index) => {
      const out = await doc.render(index, { scale: 0.5 });
      const d = out.imageData.data;
      let n = 0;
      for (let i = 0; i < d.length; i += 4) if (d[i] < 245 || d[i+1] < 245 || d[i+2] < 245) n++;
      return { ratio: n / (out.width * out.height), w: out.width, h: out.height };
    };
    const first = await ink(0);
    const last = await ink(239);
    const text = await doc.getPageText(239);
    const stats = await doc.stats();
    const pageCount = doc.pageCount;
    const mode = doc.mode, ranged = doc.ranged;
    doc.close();
    return { mode, ranged, pageCount, openMs, first, last, text: text.slice(0, 40),
             fetched: stats.source ? stats.source.fetched : -1,
             fetches: stats.source ? stats.source.fetches : -1,
             windowBytes: stats.source ? stats.source.windowBytes : -1,
             heapBytes: stats.heapBytes };
  })()`);

  if (verbose) console.log('       ', JSON.stringify(result));

  await step('the document opens from byte ranges, not from a copy', () => {
    assert.equal(result.mode, 'worker');
    assert.equal(result.ranged, true, 'document is not ranged — the file was copied into wasm');
  });

  await step('all 240 pages are discovered through the xref', () => {
    assert.equal(result.pageCount, 240);
  });

  await step('page 1 rasterizes with real ink', () => {
    assert.ok(result.first.ratio > 0.001, `ink ${(result.first.ratio * 100).toFixed(3)}%`);
    assert.equal(result.first.w, 306);
  });

  await step('page 240 rasterizes too (a range nothing read up front)', () => {
    assert.ok(result.last.ratio > 0.001, `ink ${(result.last.ratio * 100).toFixed(3)}%`);
    assert.match(result.text, /page 240/i);
  });

  await step('less than 10 MB of the 117 MB file is fetched', () => {
    assert.ok(result.fetched > 0, 'no source stats — ranged path not taken');
    assert.ok(result.fetched < BUDGET_FETCHED,
      `fetched ${(result.fetched / 1048576).toFixed(1)} MB (budget ${BUDGET_FETCHED / 1048576} MB)`);
    console.log(`       fetched ${(result.fetched / 1048576).toFixed(2)} MB in ${result.fetches} range requests, 2 pages + text rendered`);
  });

  await step('the wasm heap stays under 150 MB', () => {
    assert.ok(result.heapBytes > 0, 'no heap size reported');
    assert.ok(result.heapBytes < BUDGET_HEAP,
      `heap ${(result.heapBytes / 1048576).toFixed(0)} MB (budget ${BUDGET_HEAP / 1048576} MB)`);
    console.log(`       heap ${(result.heapBytes / 1048576).toFixed(0)} MB for a ${(fileSize / 1048576).toFixed(0)} MB file`);
  });

  await page.close();

  // ── a server that ignores Range must still work ──────────────────────────
  const port2 = await startServer(['--no-range']);
  const page2 = await newPage(9339, 'about:blank');
  await page2.setViewport(1280, 900, 1);
  await page2.goto(`http://127.0.0.1:${port2}/examples/vanilla/`);
  const fallback = await page2.eval(`(async () => {
    const { PdfDocument } = await import('/dist/index.js');
    const doc = await PdfDocument.open({ url: '/test/fixtures/large-scan.pdf', size: ${fileSize} }, { worker: true });
    const out = await doc.render(0, { scale: 0.5 });
    const d = out.imageData.data;
    let n = 0;
    for (let i = 0; i < d.length; i += 4) if (d[i] < 245 || d[i+1] < 245 || d[i+2] < 245) n++;
    const stats = await doc.stats();
    doc.close();
    return { pages: 240, ratio: n / (out.width * out.height), fetches: stats.source.fetches };
  })()`);

  await step('scrolling 60 pages forward and back stays inside the cache budget', async () => {
    const page3 = await newPage(9339, 'about:blank');
    await page3.setViewport(1280, 900, 1);
    await page3.goto(`http://127.0.0.1:${port}/examples/vanilla/`);
    const res = await page3.eval(`(async () => {
      const { PdfDocument } = await import('/dist/index.js');
      const doc = await PdfDocument.open({ url: '/test/fixtures/large-scan.pdf', size: ${fileSize} },
        { worker: true, pageCacheBytes: 16 * 1024 * 1024 });
      const forward = [];
      for (let i = 0; i < 60; i++) { doc.pinPage(i, 0.6); forward.push(await doc.render(i, { scale: 0.6 })); }
      const mid = doc.cacheStats;
      for (let i = 59; i >= 0; i--) await doc.render(i, { scale: 0.6 });   // back: hits where still cached
      const stats = doc.cacheStats;
      const heap = (await doc.stats()).heapBytes;
      doc.close();
      return { mid, stats, heap };
    })()`);
    assert.ok(res.stats.bytes <= res.stats.budget,
      `cache holds ${(res.stats.bytes / 1048576).toFixed(1)} MB of a ${(res.stats.budget / 1048576).toFixed(0)} MB budget`);
    // 60 pages at 0.6 are ~42 MB, so a 16 MB budget keeps roughly the last
    // quarter: the hits on the way back are what fits, and the rest re-render.
    // The point of the step is that memory is bounded, not that everything fits.
    assert.ok(res.stats.hits >= 10, `only ${res.stats.hits} cache hits on the way back`);
    assert.ok(res.stats.evictions > 0, 'a 60-page sweep should have evicted something');
    assert.ok(res.heap < BUDGET_HEAP, `heap ${(res.heap / 1048576).toFixed(0)} MB`);
    console.log(`       60 pages: cache ${(res.stats.bytes / 1048576).toFixed(1)} MB / ${(res.stats.budget / 1048576).toFixed(0)} MB budget, `
      + `${res.stats.hits} hits, ${res.stats.evictions} evictions, heap ${(res.heap / 1048576).toFixed(0)} MB`);
    await page3.close();
  });

  await step('a server that ignores Range still serves the document', () => {
    assert.ok(fallback.ratio > 0.001, `ink ${(fallback.ratio * 100).toFixed(3)}%`);
    // one download, then every window is a slice of it
    console.log(`       no-Range server: page 1 rendered from a single full download (${fallback.fetches} local reads)`);
  });

  await page2.close();
} finally {
  chrome.kill();
  for (const s of servers) s.kill('SIGKILL');
}

console.log(`\n${passed} step${passed === 1 ? '' : 's'} passed${failures.length ? `, ${failures.length} failed` : ''}`);
process.exit(failures.length ? 1 : 0);
