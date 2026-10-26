// e2e.mjs — drive the demos in headless Chrome and look at the pixels.
//
//   node test/e2e.mjs                     # assert + screenshots
//   node test/e2e.mjs --update-baseline   # rewrite the visual baseline
//
// What it proves, end to end:
//   * the wasm module loads over HTTP from the packaged dist/ (no build steps,
//     no CDN) and parses the fixture to 3 pages;
//   * each page rasterizes to a canvas with real ink, and the three pages are
//     different from each other;
//   * navigation, zoom and search work through the vanilla controller;
//   * the React component mounts, renders and exposes the text layer;
//   * the output is stable: per-page pixel hashes are compared to a baseline,
//     so a regression in the rasterizer fails the build instead of shipping.
//
// Screenshots land in docs/screenshots/ and are what the README shows.

import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createServer } from 'node:net';
import { createHash } from 'node:crypto';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { launchChrome, newPage } from './cdp.mjs';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const shots = join(root, 'docs', 'screenshots');
const baselinePath = join(root, 'test', 'baseline', 'visual.json');
// a free port, so a crashed run cannot poison the next one
const PORT = await new Promise((resolve) => {
  const probe = createServer();
  probe.listen(0, '127.0.0.1', () => {
    const { port } = probe.address();
    probe.close(() => resolve(port));
  });
});
const updateBaseline = process.argv.includes('--update-baseline');

const expectations = JSON.parse(await readFile(join(root, 'test', 'fixtures', 'reader-sample.json'), 'utf8'));

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

// ── boot ────────────────────────────────────────────────────────────────────
await mkdir(shots, { recursive: true });
await mkdir(dirname(baselinePath), { recursive: true });

if (!existsSync(join(root, 'dist', 'wasm', 'pdf-reader.wasm'))) {
  console.error('dist/wasm/pdf-reader.wasm is missing — run ./build.sh first');
  process.exit(2);
}

const server = spawn(process.execPath, [join(root, 'examples', 'server.mjs'), String(PORT)], {
  stdio: ['ignore', 'pipe', 'pipe'],
});
process.on('exit', () => server.kill('SIGKILL'));
let serverLog = '';
server.stdout.on('data', (d) => { serverLog += String(d); });
server.stderr.on('data', (d) => { serverLog += String(d); });
await new Promise((resolve, reject) => {
  const t = setTimeout(() => reject(new Error(`demo server did not start\n${serverLog}`)), 10000);
  const poll = setInterval(() => {
    if (serverLog.includes('demos on')) { clearTimeout(t); clearInterval(poll); resolve(); }
  }, 100);
});

const base = `http://127.0.0.1:${PORT}`;
const chrome = await launchChrome({ port: 9333, url: 'about:blank' });
console.log(`chrome ${chrome.version.Browser}`);

let code = 0;
try {
  // ── vanilla viewer ───────────────────────────────────────────────────────
  const page = await newPage(9333, 'about:blank');
  await page.setViewport(1280, 900, 1);
  await page.goto(`${base}/examples/vanilla/`);

  const ready = await page.waitFor('window.__demoReady && true', { label: 'viewer boot' })
    .then(() => page.eval('window.__demoReady'));

  await page.waitFor('document.querySelector(".wpdf-page canvas")?.width > 100', { label: 'first paint' });

  await page.eval(`window.__ink = async () => {
    const c = document.querySelector('.wpdf-page canvas');
    const ctx = c.getContext('2d');
    const { data } = ctx.getImageData(0, 0, c.width, c.height);
    let ink = 0;
    for (let i = 0; i < data.length; i += 4) {
      if (data[i] < 245 || data[i + 1] < 245 || data[i + 2] < 245) ink++;
    }
    return ink / (c.width * c.height);
  }`);

  await step('wasm module reports the expected page count', () => {
    assert.equal(ready.pageCount, expectations.pages);
  });

  await step('rendering runs in a worker, with no wasm instance on the main thread', async () => {
    const info = await page.eval(`(() => {
      const doc = window.viewer.document;
      return {
        mode: doc.mode,
        isWorker: doc.isWorker,
        hasModule: typeof doc.Module !== 'undefined',
        workerError: doc.workerError ? String(doc.workerError.message) : null,
      };
    })()`);
    assert.equal(info.mode, 'worker', `the demo fell back to the inline path: ${info.workerError}`);
    assert.equal(info.isWorker, true);
    assert.equal(info.hasModule, false, 'a worker-backed document must not own a wasm instance on the main thread');
  });

  await step('text and links cross the worker boundary', async () => {
    const res = await page.eval(`(async () => {
      const doc = window.viewer.document;
      // page 3 is not on screen, so this is a real round trip, not a cache hit
      const items = await doc.loadText(2);
      const text = await doc.getPageText(2);
      const links = await doc.loadLinks(2);
      return { runs: items.length, chars: text.length, links: Array.isArray(links) };
    })()`);
    assert.ok(res.runs > 5, `worker returned ${res.runs} text runs`);
    assert.ok(res.chars > 20, `worker returned ${res.chars} characters`);
    assert.equal(res.links, true, 'links must come back as an array');
  });

  await step('the main thread keeps ticking while a page renders', async () => {
    // The point of the worker: rasterizing a big bitmap must not freeze the tab.
    // A timer that fires while the render is in flight proves the main thread was
    // free; on the inline path it would fire once, after the render.
    const res = await page.eval(`(async () => {
      const doc = window.viewer.document;
      let ticks = 0;
      const timer = setInterval(() => { ticks++; }, 2);
      const started = performance.now();
      const out = await doc.render(0, { scale: 3, dpr: 2 });
      const ms = performance.now() - started;
      clearInterval(timer);
      return { ticks, ms, w: out.width, h: out.height };
    })()`);
    assert.ok(res.w > 1000 && res.h > 1000, `render produced ${res.w}x${res.h}`);
    assert.ok(res.ticks >= 2, `main thread blocked for ${Math.round(res.ms)} ms (${res.ticks} timer ticks)`);
  });

  await page.screenshot(join(shots, 'vanilla.png'));

  await step('page 1 paints real ink into the canvas', async () => {
    const ratio = await page.eval('window.__ink()');
    assert.ok(ratio > 0.02, `ink ratio ${(ratio * 100).toFixed(2)}% is too low`);
    assert.ok(ratio < 0.9, `ink ratio ${(ratio * 100).toFixed(2)}% suggests a solid block`);
  });

  await step('canvas is sized for the fit-width scale', async () => {
    const box = await page.eval(`(() => {
      const c = document.querySelector('.wpdf-page canvas');
      const r = c.getBoundingClientRect();
      return { w: r.width, h: r.height, ratio: r.width / r.height };
    })()`);
    assert.ok(box.w > 400, `canvas too narrow: ${box.w}`);
    assert.ok(Math.abs(box.ratio - 612 / 792) < 0.05, `wrong aspect ratio ${box.ratio.toFixed(3)}`);
  });

  await step('navigation renders a different page', async () => {
    const before = await page.eval(`(() => {
      const c = document.querySelector('.wpdf-page canvas');
      const d = c.getContext('2d').getImageData(0, 0, 60, 60).data;
      return Array.from(d.slice(0, 400)).join(',');
    })()`);
    await page.eval('window.viewer.goToPage(1)');
    await page.waitFor('window.__demoPage === 1', { label: 'page change' });
    await page.waitFor('document.querySelector(".wpdf-status")?.textContent.includes("Page 2")', { label: 'status update' });
    const after = await page.eval(`(() => {
      const c = document.querySelector('.wpdf-page canvas');
      const d = c.getContext('2d').getImageData(0, 0, 60, 60).data;
      return Array.from(d.slice(0, 400)).join(',');
    })()`);
    assert.notEqual(before, after, 'page 2 rendered identically to page 1');
  });

  await step('zoom changes the bitmap, fit-width restores it', async () => {
    const width = () => page.eval('document.querySelector(".wpdf-page canvas").width');
    const start = await width();
    await page.eval('window.viewer.zoomIn()');
    await page.waitFor(`document.querySelector('.wpdf-page canvas').width > ${start}`, { label: 'zoom in' });
    // the demo opens in fit="page", so fit-width lands on a different scale than
    // the starting one: assert it matches the viewport, not the previous width
    await page.eval('window.viewer.fitWidth()');
    await page.waitFor(
      `Math.abs(document.querySelector('.wpdf-page canvas').width - (document.querySelector('.wpdf-viewport').clientWidth - 32)) <= 2`,
      { label: 'fit width' },
    );
  });

  await step('search highlights matches on the page', async () => {
    await page.eval(`(() => {
      const input = document.getElementById('search');
      input.value = 'clipping';
      input.dispatchEvent(new Event('input'));
    })()`);
    await page.waitFor('window.__demoHits > 0', { label: 'search hits' });
    const hits = await page.eval('window.__demoHits');
    assert.ok(hits >= 2, `expected several hits, got ${hits}`);
    await page.waitFor('document.querySelector(".wpdf-highlight")', { label: 'highlight layer' });
    const box = await page.eval(`(() => {
      const h = document.querySelector('.wpdf-highlight').getBoundingClientRect();
      return { w: h.width, h: h.height };
    })()`);
    assert.ok(box.w > 20 && box.h > 3, `highlight looks empty: ${JSON.stringify(box)}`);
  });

  await page.screenshot(join(shots, 'search.png'));

  await step('clearing the search removes the highlights', async () => {
    await page.eval(`(() => {
      const input = document.getElementById('search');
      input.value = '';
      input.dispatchEvent(new Event('input'));
    })()`);
    await page.waitFor('!document.querySelector(".wpdf-highlight")', { label: 'highlights cleared' });
  });

  await step('viewer capability probes return booleans, not undefined', async () => {
    // The mupdf.js #199 shape: a probe that silently returns undefined makes
    // every `if (probe())` branch dead. Assert the type, not truthiness.
    assert.equal(await page.eval('typeof window.viewer.isContinuous()'), 'boolean');
    assert.equal(await page.eval('typeof window.viewer.state.page'), 'number');
    assert.equal(await page.eval('typeof window.viewer.document.pageCount'), 'number');
  });

  await step('continuous mode stacks every page', async () => {
    await page.eval('document.getElementById("continuous").click()');
    await page.waitFor('document.querySelectorAll(".wpdf-page").length === 3', { label: 'continuous layout' });
    await page.waitFor(`(() => {
      const cs = [...document.querySelectorAll('.wpdf-page canvas')];
      return cs.length >= 2 && cs[0].width > 100 && cs[1].width > 100;
    })()`, { label: 'two pages rendered' });
    await page.screenshot(join(shots, 'continuous.png'));
  });

  // ── visual baseline + page sheet, straight from the library ─────────────
  const hashes = await page.eval(`(async () => {
    const doc = window.viewer.document;
    const out = [];
    for (let i = 0; i < doc.pageCount; i++) {
      const { imageData } = await doc.render(i, { scale: 1, dpr: 1 });
      const digest = await crypto.subtle.digest('SHA-256', imageData.data.buffer.slice(0));
      out.push(Array.from(new Uint8Array(digest)).map(b => b.toString(16).padStart(2, '0')).join(''));
    }
    return out;
  })()`);

  await step('per-page pixel hashes are stable', async () => {
    assert.equal(hashes.length, expectations.pages);
    if (updateBaseline || !existsSync(baselinePath)) {
      await writeFile(baselinePath, `${JSON.stringify({
        note: 'SHA-256 of the RGBA page bitmaps at scale 1.0, produced by test/e2e.mjs',
        pages: hashes,
      }, null, 2)}\n`);
      console.log(`       baseline written (${baselinePath.replace(root + '/', '')})`);
      return;
    }
    const baseline = JSON.parse(await readFile(baselinePath, 'utf8'));
    assert.deepEqual(hashes, baseline.pages,
      'rendered pixels changed — inspect with: node test/e2e.mjs --update-baseline');
  });

  await step('page sheet renders all three pages in one image', async () => {
    const dataUrl = await page.eval(`(async () => {
      const doc = window.viewer.document;
      const scale = 0.9;
      const pages = [];
      for (let i = 0; i < doc.pageCount; i++) pages.push((await doc.render(i, { scale })).imageData);
      const pad = 24;
      const W = pages.reduce((s, p) => s + p.width, 0) + pad * (pages.length + 1);
      const H = Math.max(...pages.map(p => p.height)) + pad * 2;
      const cv = document.createElement('canvas');
      cv.width = W; cv.height = H;
      const ctx = cv.getContext('2d');
      ctx.fillStyle = '#111827'; ctx.fillRect(0, 0, W, H);
      let x = pad;
      for (const p of pages) {
        const tmp = document.createElement('canvas');
        tmp.width = p.width; tmp.height = p.height;
        tmp.getContext('2d').putImageData(p, 0, 0);
        ctx.drawImage(tmp, x, pad);
        x += p.width + pad;
      }
      return cv.toDataURL('image/png');
    })()`);
    const png = Buffer.from(dataUrl.split(',')[1], 'base64');
    await writeFile(join(shots, 'pages.png'), png);
    assert.ok(png.length > 20000, 'page sheet looks empty');
  });

  await page.close();

  // ── React demo ──────────────────────────────────────────────────────────
  const reactPage = await newPage(9333, 'about:blank');
  await reactPage.setViewport(1280, 900, 1);
  await reactPage.goto(`${base}/examples/react/`);

  await step('the React demo also runs the engine in a worker', async () => {
    // The bundle resolves workerUrl against the page, not the library, so this
    // is the step that catches a worker 404 falling back to the main thread.
    await reactPage.waitFor('window.__reactDoc', { label: 'react document', timeout: 25000 });
    const info = await reactPage.eval(`(() => {
      const d = window.__reactDoc;
      return d ? { mode: d.mode, err: d.workerError ? String(d.workerError.message) : null } : null;
    })()`);
    assert.ok(info, 'the demo never reported its document through onLoad');
    assert.equal(info.mode, 'worker', `React demo fell back to inline: ${info.err}`);
  });

  await step('React component mounts and renders a page', async () => {
    // Wait for the page bitmap, not for the canvas element: a bare <canvas> is
    // 300x150 before the first putImageData, which is enough to pass a
    // width-only check while the render is still in flight (worker mode).
    await reactPage.waitFor(
      `(() => {
        const c = document.querySelector('.wpdf-root[data-status="ready"] canvas');
        return !!c && c.width > 300 && c.height > 400;
      })()`,
      { label: 'react first paint', timeout: 25000 },
    );
    const info = await reactPage.eval(`(() => {
      const c = document.querySelector('.wpdf-page canvas');
      return { w: c.width, h: c.height, pages: document.querySelector('.wpdf-toolbar')?.textContent };
    })()`);
    assert.ok(info.w > 300 && info.h > 400, `canvas wrong size: ${JSON.stringify(info)}`);
    assert.match(info.pages, /1 \/ 3/, `toolbar shows ${info.pages}`);
  });

  await step('React text extraction returns the page text', async () => {
    await reactPage.eval(`[...document.querySelectorAll('button')].find(b => b.textContent === 'Extract text').click()`);
    await reactPage.waitFor('document.querySelector(".side pre").textContent.includes("WASM PDF READER")', {
      label: 'text panel',
    });
    const text = await reactPage.eval('document.querySelector(".side pre").textContent');
    assert.ok(text.length > 200, `text panel too short (${text.length})`);
  });

  await step('React metadata lands in the sidebar', async () => {
    const meta = await reactPage.eval('document.querySelector(".side dl").textContent');
    assert.match(meta, /WASM PDF Reader sample/);
  });

  await reactPage.screenshot(join(shots, 'react.png'));
  await reactPage.close();

  code = failures.length ? 1 : 0;
} finally {
  chrome.kill();
  server.kill('SIGKILL');
}

console.log(`\n${passed} steps passed${failures.length ? `, ${failures.length} failed` : ''}`);
console.log(`screenshots: ${shots.replace(root + '/', '')}/`);
process.exit(code);
