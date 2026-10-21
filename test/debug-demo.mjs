// debug-demo.mjs — print what the vanilla demo is doing (dev aid).
import { spawn } from 'node:child_process';
import { createServer } from 'node:net';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { launchChrome, newPage } from './cdp.mjs';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const PORT = await new Promise((r) => {
  const s = createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => r(p)); });
});
const server = spawn(process.execPath, [join(root, 'examples', 'server.mjs'), String(PORT)], { stdio: 'ignore' });
await new Promise((r) => setTimeout(r, 500));
const chrome = await launchChrome({ port: 9335, url: 'about:blank' });
const page = await newPage(9335, 'about:blank');

const logs = [];
page.ws.addEventListener('message', (ev) => {
  const m = JSON.parse(ev.data);
  if (m.method === 'Runtime.consoleAPICalled') logs.push(`${m.params.type}: ${m.params.args.map(a => a.value ?? a.description).join(' ')}`);
  if (m.method === 'Runtime.exceptionThrown') logs.push(`EXC: ${m.params.exceptionDetails?.exception?.description}`);
});

try {
  await page.setViewport(1280, 900, 1);
  await page.goto(`http://127.0.0.1:${PORT}/examples/vanilla/`);
  await new Promise((r) => setTimeout(r, 3000));
  const state = await page.eval(`(() => {
    const root = document.querySelector('.wpdf-root');
    const vp = document.querySelector('.wpdf-viewport');
    const c = document.querySelector('.wpdf-page canvas');
    return {
      viewer: typeof window.viewer,
      status: window.viewer?.state,
      bodyH: document.body.getBoundingClientRect().height,
      mainH: document.querySelector('main')?.getBoundingClientRect().height,
      viewerH: document.getElementById('viewer')?.getBoundingClientRect().height,
      bodyDisplay: getComputedStyle(document.body).display,
      viewerDisplay: getComputedStyle(document.getElementById('viewer')).display,
      rootH: root?.getBoundingClientRect().height,
      vpH: vp?.getBoundingClientRect().height,
      canvas: c ? { w: c.width, h: c.height, css: c.getBoundingClientRect().width } : null,
      pages: document.querySelectorAll('.wpdf-page').length,
      toolbar: document.querySelector('.wpdf-toolbar')?.textContent,
      overlay: document.querySelector('.wpdf-overlay')?.outerHTML.slice(0, 200),
    };
  })()`);
  console.log(JSON.stringify(state, null, 2));
  console.log('LOGS:\n' + logs.join('\n'));
} finally {
  page.close(); chrome.kill(); server.kill('SIGKILL');
}
