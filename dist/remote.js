// remote.js — the main-thread half of worker mode.
//
// `PdfDocument.open(source, { worker: true })` returns a PdfRemoteDocument: the
// same shape as PdfDocument, except that the file is parsed and every page is
// rasterized inside a Web Worker. Only the RGBA bytes (transferred, not copied)
// and small JSON payloads cross the thread boundary.
//
// The synchronous part of the API still works because everything a layout needs
// is captured once, at open time: page count, every page's geometry, metadata,
// the outline, form fields and warnings. What cannot stay synchronous is
// anything that has to touch the document for the first time:
//
//   render(), renderToCanvas()      async (unchanged in signature)
//   getText(), getLinks()           cached; warmed by the render that drew the page
//   loadText(), loadLinks()         async — use these before a render
//   getPageText(), getAllText()     async
//   search()                        returns a Promise
//   setField()                      returns a Promise<boolean>
//
// A host that needs the sync API keeps using the default inline path.

import { boxToCss, itemsToText } from './text.js';

const READY_TIMEOUT = 15000;

function defaultWorkerUrl() {
  try {
    return new URL('./worker.js', import.meta.url).href;
  } catch {
    return './worker.js';
  }
}

function spawn(options) {
  if (typeof options.workerFactory === 'function') {
    const worker = options.workerFactory(options);
    if (!worker) throw new Error('wasm-pdf-reader: workerFactory returned no worker');
    return worker;
  }
  if (typeof Worker === 'undefined') {
    throw new Error('wasm-pdf-reader: no Web Worker support in this environment');
  }
  return new Worker(options.workerUrl || defaultWorkerUrl(), { type: 'module' });
}

/** One worker, request/response by message id. */
class WorkerClient {
  constructor(worker) {
    this.worker = worker;
    this.nextId = 1;
    this.pending = new Map();
    this.dead = null;
    this.ready = new Promise((resolve, reject) => {
      this._resolveReady = resolve;
      this._rejectReady = reject;
    });
    this.ready.catch(() => {});          // an unawaited rejection must not surface
    this._timer = setTimeout(() => {
      this._fail(new Error('wasm-pdf-reader: the worker script did not start (bad workerUrl, CSP, or a bundler that rewrote the URL)'));
    }, READY_TIMEOUT);
    if (typeof this._timer?.unref === 'function') this._timer.unref();
    worker.addEventListener('message', (ev) => this._onMessage(ev));
    worker.addEventListener('error', (ev) => {
      this._fail(new Error(`wasm-pdf-reader: worker error — ${ev?.message || 'the script failed to load'}`));
    });
    worker.addEventListener('messageerror', () => {
      this._fail(new Error('wasm-pdf-reader: could not deserialize a worker message'));
    });
  }

  _onMessage(ev) {
    const data = ev.data || {};
    if (data.op === 'ready') {
      clearTimeout(this._timer);
      this._resolveReady();
      return;
    }
    const waiter = this.pending.get(data.id);
    if (!waiter) return;
    this.pending.delete(data.id);
    if (data.ok) waiter.resolve(data.result);
    else waiter.reject(new Error(data.error || 'worker error'));
  }

  _fail(err) {
    if (this.dead) return;
    this.dead = err;
    clearTimeout(this._timer);
    this._rejectReady(err);
    for (const [, waiter] of this.pending) waiter.reject(err);
    this.pending.clear();
    try { this.worker.terminate(); } catch { /* already gone */ }
  }

  call(op, payload, transfer = []) {
    if (this.dead) return Promise.reject(this.dead);
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      try {
        this.worker.postMessage({ id, op, payload }, transfer);
      } catch (err) {
        this.pending.delete(id);
        reject(err);
      }
    });
  }
}

/**
 * One worker per pool. Documents opened through the same pool share it, so a
 * host that opens three files pays for one wasm instance; the pool terminates
 * the worker when its last document closes.
 */
class WorkerPool {
  constructor(options) {
    this.options = options;
    this.docs = 0;
    this.client = null;
    this.opening = null;
  }

  async acquire() {
    if (!this.client) {
      if (!this.opening) {
        this.opening = (async () => {
          const client = new WorkerClient(spawn(this.options));
          await client.ready;
          this.client = client;
          this.opening = null;
          return client;
        })().catch((err) => {
          this.opening = null;
          throw err;
        });
      }
      await this.opening;
    }
    this.docs++;
    return this.client;
  }

  release() {
    this.docs = Math.max(0, this.docs - 1);
    if (this.docs > 0 || !this.client) return;
    try { this.client.worker.terminate(); } catch { /* already gone */ }
    this.client = null;
    pools.delete(this.key);
  }
}

const pools = new Map();

function poolFor(options) {
  const key = typeof options.workerFactory === 'function'
    ? options.workerFactory
    : `url:${options.workerUrl || defaultWorkerUrl()}`;
  let pool = pools.get(key);
  if (!pool) {
    pool = new WorkerPool(options);
    pool.key = key;
    pools.set(key, pool);
  }
  return pool;
}

/** Turn any accepted source into something the worker can open without a copy. */
async function openPayload(source, options) {
  const payload = {
    filename: options.filename || '',
    password: options.password ? String(options.password) : '',
    wasmUrl: options.wasmUrl || '',
  };
  let transfer = [];
  if (typeof Blob !== 'undefined' && source instanceof Blob) {
    payload.blob = source;                       // structured clone shares the bytes
    payload.filename = payload.filename || source.name || '';
  } else if (typeof ArrayBuffer !== 'undefined'
    && (source instanceof ArrayBuffer || ArrayBuffer.isView(source))) {
    const view = source instanceof ArrayBuffer
      ? new Uint8Array(source)
      : new Uint8Array(source.buffer, source.byteOffset, source.byteLength);
    const copy = view.slice();                   // the caller keeps its buffer
    payload.bytes = copy.buffer;
    transfer = [copy.buffer];
  } else {
    payload.blob = await sourceAsBlob(source);
  }
  return { payload, transfer };
}

async function sourceAsBlob(source) {
  if (source == null) throw new TypeError('wasm-pdf-reader: no document given');
  if (typeof source === 'string' || source instanceof URL) {
    const res = await fetch(source);
    if (!res.ok) throw new Error(`wasm-pdf-reader: HTTP ${res.status} for ${source}`);
    return res.blob();
  }
  if (typeof Response !== 'undefined' && source instanceof Response) {
    if (!source.ok) throw new Error(`wasm-pdf-reader: HTTP ${source.status}`);
    return source.blob();
  }
  if (source && typeof source.arrayBuffer === 'function') {
    return new Blob([await source.arrayBuffer()]);
  }
  throw new TypeError('wasm-pdf-reader: unsupported document source');
}

/**
 * Open a document in a worker.
 * @param {*} source same shapes as `PdfDocument.open`
 * @param {Object} options ModuleOptions + `workerUrl` / `workerFactory`
 * @returns {Promise<PdfRemoteDocument>}
 */
export async function openRemote(source, options = {}) {
  const pool = poolFor(options);
  const client = await pool.acquire();
  try {
    const { payload, transfer } = await openPayload(source, options);
    const opened = await client.call('open', payload, transfer);
    return new PdfRemoteDocument(pool, client, opened.docId, opened.summary);
  } catch (err) {
    pool.release();
    throw err;
  }
}

/** A document whose wasm instance lives in a worker. */
export class PdfRemoteDocument {
  constructor(pool, client, docId, summary) {
    this._pool = pool;
    this._client = client;
    this._id = docId;
    this._summary = summary;
    this._closed = false;
    this._textCache = new Map();
    this._linkCache = new Map();
    this._sizeCache = new Map();
    this.filename = summary.filename || '';
  }

  /** Which path this document runs on: 'worker' or 'inline'. */
  get mode() { return 'worker'; }

  /** True for a worker-backed document (the viewer/e2e probes this). */
  get isWorker() { return true; }

  get pageCount() {
    this._assertOpen();
    return this._summary.pageCount;
  }

  get metadata() { return this._summary.metadata || {}; }

  get outline() { return this._summary.outline || []; }

  get form() { return this._summary.form || []; }

  get warnings() { return this._summary.warnings || []; }

  _assertOpen() {
    if (this._closed) throw new Error('wasm-pdf-reader: document is closed');
  }

  getPageSize(index) {
    this._assertOpen();
    const cached = this._sizeCache.get(index);
    if (cached) return cached;
    const info = this._summary.sizes[index];
    if (!info) throw new RangeError(`wasm-pdf-reader: page ${index} of ${this._summary.pageCount}`);
    this._sizeCache.set(index, info);
    return info;
  }

  async render(index, options = {}) {
    this._assertOpen();
    const scale = options.scale ?? 1.5;
    const dpr = options.dpr ?? 1;
    const deviceScale = scale * dpr;
    const size = this.getPageSize(index);
    const res = await this._client.call('render', {
      docId: this._id, page: index, scale: deviceScale, stats: !!options.stats,
    });
    this._assertOpen();
    this._textCache.set(index, res.text || []);
    this._linkCache.set(index, res.links || []);

    let imageData;
    if (typeof ImageData !== 'undefined') {
      imageData = new ImageData(new Uint8ClampedArray(res.rgba), res.width, res.height);
    } else {
      imageData = { data: new Uint8ClampedArray(res.rgba), width: res.width, height: res.height };
    }
    if (typeof options.pixelTransform === 'function') {
      const replaced = options.pixelTransform(imageData.data, {
        width: res.width, height: res.height, page: index, scale: deviceScale,
      });
      if (replaced && replaced.data) imageData = replaced;
    }
    return {
      imageData,
      width: res.width,
      height: res.height,
      cssWidth: size.width * scale,
      cssHeight: size.height * scale,
      scale,
      stats: res.stats,
    };
  }

  async renderToCanvas(canvas, index, options = {}) {
    const out = await this.render(index, options);
    const dpr = options.dpr ?? 1;
    canvas.width = out.width;
    canvas.height = out.height;
    if (options.cssScale !== false) {
      canvas.style.width = `${out.width / dpr}px`;
      canvas.style.height = `${out.height / dpr}px`;
    }
    canvas.getContext('2d').putImageData(out.imageData, 0, 0);
    return out;
  }

  /** Cached text runs for a page — warmed by the render that drew it. */
  getText(index) {
    this._assertOpen();
    return this._textCache.get(index) || [];
  }

  getLinks(index) {
    this._assertOpen();
    return this._linkCache.get(index) || [];
  }

  async loadText(index) {
    this._assertOpen();
    if (this._textCache.has(index)) return this._textCache.get(index);
    const items = await this._client.call('text', { docId: this._id, page: index });
    this._textCache.set(index, items || []);
    return this._textCache.get(index);
  }

  async loadLinks(index) {
    this._assertOpen();
    if (this._linkCache.has(index)) return this._linkCache.get(index);
    const links = await this._client.call('links', { docId: this._id, page: index });
    this._linkCache.set(index, links || []);
    return this._linkCache.get(index);
  }

  async getPageText(index, options) {
    return itemsToText(await this.loadText(index), options);
  }

  async getAllText(options) {
    const pages = [];
    for (let i = 0; i < this.pageCount; i++) pages.push(await this.getPageText(i, options));
    return pages;
  }

  /** Resolves to `[{page, index, length, line, rects}]`. */
  search(query, options = {}) {
    this._assertOpen();
    return this._client.call('search', { docId: this._id, query, options });
  }

  /** Resolves to true when the field was found. */
  setField(name, value) {
    this._assertOpen();
    return this._client.call('field', { docId: this._id, name: String(name), value: String(value) });
  }

  boxToCss(box, index, scale) {
    const size = this.getPageSize(index);
    return boxToCss(box, { height: size.height, boxY0: size.boxY0, scale });
  }

  close() {
    if (this._closed) return;
    this._closed = true;
    this._textCache.clear();
    this._linkCache.clear();
    this._sizeCache.clear();
    // The last document closing takes the worker with it, so do not spend a
    // round trip on the close message that terminate() would race.
    const shared = this._pool.docs > 1;
    if (shared) this._client.call('close', { docId: this._id }).catch(() => {});
    this._pool.release();
  }
}

export default openRemote;
