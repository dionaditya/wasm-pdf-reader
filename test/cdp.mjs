// cdp.mjs — the smallest Chrome DevTools Protocol client that does the job.
//
// Node has a global WebSocket, so no dependency is needed: open a page target,
// send Runtime.evaluate / Page.captureScreenshot, read the results.

export class CDP {
  constructor(wsUrl) {
    this.ws = new WebSocket(wsUrl);
    this.id = 0;
    this.pending = new Map();
    this.events = [];
  }

  async open() {
    await new Promise((resolve, reject) => {
      this.ws.addEventListener('open', resolve, { once: true });
      this.ws.addEventListener('error', (e) => reject(new Error(`cdp socket: ${e.message || 'error'}`)), { once: true });
    });
    this.ws.addEventListener('message', (ev) => {
      const msg = JSON.parse(ev.data);
      if (msg.id && this.pending.has(msg.id)) {
        const { resolve, reject } = this.pending.get(msg.id);
        this.pending.delete(msg.id);
        if (msg.error) reject(new Error(`${msg.error.message} (${msg.error.code})`));
        else resolve(msg.result);
      } else if (msg.method) {
        this.events.push(msg);
      }
    });
    return this;
  }

  send(method, params = {}) {
    const id = ++this.id;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      this.ws.send(JSON.stringify({ id, method, params }));
      setTimeout(() => {
        if (this.pending.delete(id)) reject(new Error(`cdp timeout: ${method}`));
      }, 60000);
    });
  }

  /** Evaluate an expression in the page and return its JSON value. */
  async eval(expression) {
    const res = await this.send('Runtime.evaluate', {
      expression,
      awaitPromise: true,
      returnByValue: true,
      userGesture: true,
    });
    if (res.exceptionDetails) {
      const d = res.exceptionDetails;
      throw new Error(`page error: ${d.exception?.description || d.text}`);
    }
    return res.result?.value;
  }

  /** Poll an expression until it is truthy. */
  async waitFor(expression, { timeout = 20000, interval = 100, label = expression } = {}) {
    const start = Date.now();
    for (;;) {
      let value;
      try { value = await this.eval(expression); } catch { value = false; }
      if (value) return value;
      if (Date.now() - start > timeout) throw new Error(`timed out waiting for ${label}`);
      await new Promise((r) => setTimeout(r, interval));
    }
  }

  async screenshot(path) {
    const { data } = await this.send('Page.captureScreenshot', { format: 'png', captureBeyondViewport: false });
    const { writeFile } = await import('node:fs/promises');
    await writeFile(path, Buffer.from(data, 'base64'));
    return path;
  }

  async setViewport(width, height, dpr = 1) {
    await this.send('Emulation.setDeviceMetricsOverride', {
      width, height, deviceScaleFactor: dpr, mobile: false,
    });
  }

  async goto(url) {
    await this.send('Page.navigate', { url });
    await this.waitFor('document.readyState === "complete"', { label: `load ${url}` });
  }

  close() {
    try { this.ws.close(); } catch { /* already gone */ }
  }
}

export async function launchChrome({ port = 9333, userDataDir, url = 'about:blank' } = {}) {
  const { spawn } = await import('node:child_process');
  const { mkdtemp } = await import('node:fs/promises');
  const { tmpdir } = await import('node:os');
  const { join } = await import('node:path');

  const candidates = [
    process.env.CHROME,
    join(process.env.HOME || '', '.cache/ms-playwright/chromium-1248/chrome-linux64/chrome'),
    '/usr/bin/google-chrome',
    '/usr/bin/chromium',
    '/usr/bin/chromium-browser',
  ].filter(Boolean);

  const { access } = await import('node:fs/promises');
  let binary = null;
  for (const c of candidates) {
    try { await access(c); binary = c; break; } catch { /* next */ }
  }
  if (!binary) throw new Error('no Chrome/Chromium found; set CHROME=/path/to/chrome');

  const dir = userDataDir || await mkdtemp(join(tmpdir(), 'wpdf-chrome-'));
  // Headless Chromium from the Playwright cache needs a few system libraries
  // that may only exist as an extracted bundle on this machine. If a local
  // bundle is present, prefer it over the (possibly incomplete) system paths.
  const home = process.env.HOME || '';
  const libCandidates = [
    join(home, '.local/chromedeps/root/usr/lib/x86_64-linux-gnu'),
    join(home, 'lolibs/root/usr/lib/x86_64-linux-gnu'),
    '/usr/lib/x86_64-linux-gnu',
  ];
  const libDirs = [];
  for (const d of libCandidates) {
    try { await access(join(d, 'libnspr4.so')); libDirs.push(d); } catch { /* not here */ }
  }
  const env = { ...process.env };
  if (libDirs.length) {
    env.LD_LIBRARY_PATH = [...libDirs, env.LD_LIBRARY_PATH || ''].filter(Boolean).join(':');
  }

  const proc = spawn(binary, [
    '--headless=new',
    '--no-sandbox',
    '--disable-gpu',
    '--hide-scrollbars',
    '--mute-audio',
    '--no-first-run',
    '--no-default-browser-check',
    '--disable-dev-shm-usage',
    `--user-data-dir=${dir}`,
    `--remote-debugging-port=${port}`,
    url,
  ], { stdio: ['ignore', 'ignore', 'pipe'], env });

  let stderr = '';
  proc.stderr.on('data', (d) => { stderr += String(d); });

  // wait for the debugging endpoint
  const deadline = Date.now() + 20000;
  let version = null;
  while (Date.now() < deadline) {
    try {
      const res = await fetch(`http://127.0.0.1:${port}/json/version`);
      if (res.ok) { version = await res.json(); break; }
    } catch { /* not up yet */ }
    await new Promise((r) => setTimeout(r, 150));
  }
  if (!version) {
    proc.kill('SIGKILL');
    throw new Error(`chrome did not start\n${stderr.slice(-2000)}`);
  }
  return { proc, version, dir, kill: () => proc.kill('SIGKILL') };
}

/** Open a fresh page target and attach to it. */
export async function newPage(port, url) {
  const res = await fetch(`http://127.0.0.1:${port}/json/new?${encodeURIComponent(url)}`, { method: 'PUT' });
  if (!res.ok) throw new Error(`cannot open tab: ${res.status}`);
  const target = await res.json();
  const page = await new CDP(target.webSocketDebuggerUrl).open();
  await page.send('Page.enable');
  await page.send('Runtime.enable');
  return page;
}
