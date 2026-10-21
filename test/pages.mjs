// pages.mjs — check the GitHub Pages build the way Pages serves it.
//
//   node test/pages.mjs
//
// Pages hosts a project site under /<repo>/, so a demo that only works at the
// domain root would pass locally and break in production. This builds _site,
// serves it under a /wasm-pdf-reader/ prefix and loads both demos with no
// root-absolute paths to fall back on.

import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { cp, mkdir, rm } from 'node:fs/promises';
import { createServer } from 'node:net';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { tmpdir } from 'node:os';
import { launchChrome, newPage } from './cdp.mjs';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const site = join(tmpdir(), 'wpdf-pages-site');
const base = join(site, 'wasm-pdf-reader');

function run(cmd, args) {
  return new Promise((resolve, reject) => {
    const p = spawn(cmd, args, { cwd: root, stdio: 'ignore' });
    p.on('exit', (code) => (code === 0 ? resolve() : reject(new Error(`${cmd} exited ${code}`))));
  });
}

console.log('building the demo site…');
await run(process.execPath, [join(root, 'scripts', 'build-demo.mjs')]);

await rm(site, { recursive: true, force: true });
await mkdir(site, { recursive: true });
await cp(join(root, '_site'), base, { recursive: true });

const PORT = await new Promise((resolve) => {
  const probe = createServer();
  probe.listen(0, '127.0.0.1', () => { const { port } = probe.address(); probe.close(() => resolve(port)); });
});

const server = spawn(process.execPath, [join(root, 'examples', 'server.mjs'), String(PORT), '--root', site], {
  stdio: ['ignore', 'pipe', 'pipe'],
});
await new Promise((r) => setTimeout(r, 600));
process.on('exit', () => server.kill('SIGKILL'));

const chrome = await launchChrome({ port: 9334 });
let failed = 0;

async function visit(path, check) {
  const page = await newPage(9334, 'about:blank');
  await page.setViewport(1200, 860, 1);
  const errors = [];
  page.ws.addEventListener('message', (ev) => {
    const msg = JSON.parse(ev.data);
    if (msg.method === 'Runtime.exceptionThrown') errors.push(msg.params.exceptionDetails?.text || 'exception');
    if (msg.method === 'Runtime.consoleAPICalled' && msg.params.type === 'error') {
      errors.push(msg.params.args.map((a) => a.value ?? a.description).join(' '));
    }
  });
  try {
    await page.goto(`http://127.0.0.1:${PORT}/wasm-pdf-reader${path}`);
    const info = await page.waitFor(`(() => {
      const c = document.querySelector('.wpdf-page canvas');
      if (!c || !c.width) return false;
      const ctx = c.getContext('2d');
      const { data } = ctx.getImageData(0, 0, Math.min(c.width, 120), Math.min(c.height, 120));
      let ink = 0;
      for (let i = 0; i < data.length; i += 4) if (data[i] < 240) ink++;
      return { width: c.width, ink, title: document.title };
    })()`, { timeout: 25000, label: `first paint on ${path}` });
    check(info);
    assert.deepEqual(errors, [], `console errors on ${path}:\n${errors.join('\n')}`);
    console.log(`  ok  ${path} under /wasm-pdf-reader/ (${info.width}px wide, ${info.ink} ink px in the corner)`);
  } catch (err) {
    failed++;
    console.error(`  FAIL ${path}: ${err.message}`);
  } finally {
    page.close();
  }
}

try {
  await visit('/vanilla/', (info) => {
    assert.ok(info.width > 300, `canvas too small: ${info.width}`);
    assert.ok(info.ink > 0, 'corner of the page has no ink — the page probably failed to render');
  });
  await visit('/react/', (info) => {
    assert.ok(info.width > 300, `canvas too small: ${info.width}`);
    assert.ok(info.ink > 0, 'corner of the page has no ink');
  });
} finally {
  chrome.kill();
  server.kill('SIGKILL');
}

console.log(failed ? `\n${failed} page(s) failed` : '\nboth Pages demos work under a base path');
process.exit(failed ? 1 : 0);
