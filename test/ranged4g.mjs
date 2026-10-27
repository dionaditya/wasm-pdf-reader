// ranged4g.mjs — the 4 GiB offset test: a file whose objects live past 2^32.
//
//   node test/ranged4g.mjs
//
// A 120 MB fixture cannot reach the top of the offset range, so every truncation
// bug (32-bit `size_t` in ByteSource, `long` xref offsets, `(int)off` in the JS
// bridge, `atol` on the startxref value) stays invisible. This opens a *sparse*
// PDF sized 4 GiB - 1 MiB whose header is at 0 and whose every object sits in the
// last few hundred bytes, so the trailer, xref and page object are all addressed
// above 2^32 - 1 MiB.
//
// It asserts the ranged path end to end: the document parses, the page renders
// with real ink, fetched bytes stay in single-digit megabytes (the window is
// still 16 KB), the heap stays flat — and a file *above* 4 GiB is refused with a
// message that says so instead of silently becoming size 0.

import assert from 'node:assert/strict';
import { spawn, execFileSync } from 'node:child_process';
import { createServer } from 'node:net';
import { existsSync } from 'node:fs';
import { stat } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { launchChrome, newPage } from './cdp.mjs';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const fixture = join(root, 'test', 'fixtures', 'huge-sparse.pdf');
const verbose = process.argv.includes('--verbose');
const BUDGET_FETCHED = 2 * 1024 * 1024;       // acceptance: < 2 MB of a 4 GiB file
const BUDGET_HEAP = 150 * 1024 * 1024;
const LIMIT = 4294967296;                     // 2^32: the reader's stated ceiling

if (!existsSync(fixture)) {
  console.log('fixture missing — generating a sparse ~4 GiB PDF (no real disk cost)…');
  const python = existsSync(join(root, '..', 'pdfvenv', 'bin', 'python'))
    ? join(root, '..', 'pdfvenv', 'bin', 'python')
    : 'python3';
  execFileSync(python, [join(root, 'scripts', 'make_sparse_pdf.py')], { stdio: 'inherit' });
}
const fileSize = (await stat(fixture)).size;
// Sparse files report their logical size; if this host or filesystem materialised
// the hole we do not want to serve 4 GiB of zeros either.
const { blocks } = await stat(fixture, { bigint: true });
const allocated = Number(blocks) * 512;

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

const chrome = await launchChrome({ port: 9341, url: 'about:blank' });
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
  console.log(`fixture ${fileSize} bytes (${(fileSize / 2 ** 30).toFixed(3)} GiB), ` +
              `${(allocated / 2 ** 20).toFixed(1)} MiB allocated on disk`);
  assert.ok(fileSize > 2 ** 32 - 2 ** 21, 'fixture is not near 4 GiB');
  assert.ok(allocated < 64 * 1024 * 1024, 'fixture is not sparse — it would cost 4 GiB of disk');

  const port = await startServer();
  const page = await newPage(9341, 'about:blank');
  await page.setViewport(1280, 900, 1);
  await page.goto(`http://127.0.0.1:${port}/examples/vanilla/`);

  const result = await page.eval(`(async () => {
    const { PdfDocument } = await import('/dist/index.js');
    const size = ${fileSize};
    const t0 = performance.now();
    const doc = await PdfDocument.open({ url: '/test/fixtures/huge-sparse.pdf', size }, { worker: true });
    const openMs = Math.round(performance.now() - t0);
    const out = await doc.render(0, { scale: 0.5 });
    const d = out.imageData.data;
    let n = 0;
    for (let i = 0; i < d.length; i += 4) if (d[i] < 245 || d[i+1] < 245 || d[i+2] < 245) n++;
    const stats = await doc.stats();
    const res = {
      mode: doc.mode, ranged: doc.ranged, pageCount: doc.pageCount, openMs,
      ink: n / (out.width * out.height), w: out.width, h: out.height,
      fetched: stats.source ? stats.source.fetched : -1,
      fetches: stats.source ? stats.source.fetches : -1,
      windowBytes: stats.source ? stats.source.windowBytes : -1,
      heapBytes: stats.heapBytes,
      text: (await doc.getPageText(0)).slice(0, 60),
    };
    doc.close();
    return res;
  })()`);

  if (verbose) console.log('       ', JSON.stringify(result));

  await step('opens from byte ranges, not from a copy', () => {
    assert.equal(result.mode, 'worker');
    assert.equal(result.ranged, true, 'document is not ranged — the file was copied into wasm');
  });

  await step('the xref past 2^32 - 1 MiB resolves to one page', () => {
    assert.equal(result.pageCount, 1, `pageCount ${result.pageCount}`);
    assert.match(result.text, /Sparse 4 GiB fixture/);
  });

  await step('the page rasterizes with real ink', () => {
    assert.ok(result.ink > 0.001, `ink ${(result.ink * 100).toFixed(3)}%`);
    assert.equal(result.w, 306);
  });

  await step('less than 2 MB of the 4 GiB file is fetched', () => {
    assert.ok(result.fetched > 0, 'no source stats — ranged path not taken');
    assert.ok(result.fetched < BUDGET_FETCHED,
      `fetched ${(result.fetched / 1048576).toFixed(2)} MB (budget 2 MB)`);
    console.log(`       fetched ${(result.fetched / 1024).toFixed(0)} KB in ${result.fetches} range requests, ` +
                `window ${(result.windowBytes / 1024).toFixed(0)} KB, open ${result.openMs} ms`);
  });

  await step('the wasm heap stays flat', () => {
    assert.ok(result.heapBytes > 0, 'no heap size reported');
    assert.ok(result.heapBytes < BUDGET_HEAP,
      `heap ${(result.heapBytes / 1048576).toFixed(0)} MB (budget ${BUDGET_HEAP / 1048576} MB)`);
    console.log(`       heap ${(result.heapBytes / 1048576).toFixed(0)} MB for a ${(fileSize / 2 ** 30).toFixed(2)} GiB file`);
  });

  const refused = await page.eval(`(async () => {
    const { PdfDocument } = await import('/dist/index.js');
    try {
      await PdfDocument.open({ url: '/test/fixtures/huge-sparse.pdf', size: ${LIMIT + 1048576} }, { worker: true });
      return { threw: false };
    } catch (e) { return { threw: true, message: String(e && e.message || e) }; }
  })()`);

  await step('a file above 4 GiB is refused, not silently truncated to size 0', () => {
    assert.equal(refused.threw, true, 'a >4 GiB size was accepted');
    assert.match(refused.message, /4 GiB/, `message: ${refused.message}`);
    assert.doesNotMatch(refused.message, /empty document/i, `message: ${refused.message}`);
    console.log(`       refused: ${refused.message.split('\n')[0].slice(0, 110)}`);
  });

  await page.close();
} finally {
  chrome.kill();
  for (const s of servers) s.kill('SIGKILL');
}

console.log(`\n${passed} step${passed === 1 ? '' : 's'} passed${failures.length ? `, ${failures.length} failed` : ''}`);
process.exit(failures.length ? 1 : 0);
