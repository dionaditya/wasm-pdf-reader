// document.js — one open PDF: pages, rendering, text, search.
//
// This is the framework-agnostic half of the library. It has no DOM
// dependency beyond a canvas it is handed (or that it creates), so it can be
// used from a worker, a test, a vanilla page or React.

import { loadModule } from './loader.js';
import { itemsToText, searchItems, boxToCss } from './text.js';
import { isRangeSpec } from './range-source.js';
import { PageCache } from './page-cache.js';

// The wasm module owns a single page bitmap, so renders are serialised across
// every document that shares the instance.
let renderChain = Promise.resolve();

function serialise(task) {
  const run = renderChain.then(task, task);
  renderChain = run.catch(() => {});
  return run;
}

/** Accept ArrayBuffer / TypedArray / Blob / File / URL / Response / fetch(). */
// Streaming copy: a File/Blob is read in slices straight into the wasm heap, so
// the document is never duplicated in JS memory. At 1 GB that is the difference
// between ~2x the file size and ~1x.
const CHUNK_BYTES = 16 * 1024 * 1024;

// Mirrors MAXIMUM_MEMORY in build.sh. The module cannot address more than this
// however much RAM the device has.
const WASM_CEILING_BYTES = 1024 * 1024 * 1024;
// A whole-file copy has to fit the file *and* everything a render needs on top
// of it (a page bitmap, decoded streams, the parser's own structures). Above
// half the ceiling that headroom no longer exists on any device, so the copy is
// not worth attempting — it just grows the heap to its limit and fails, which is
// what used to surface as a confusing "out of memory" on a big file.
const COPY_BUDGET_BYTES = WASM_CEILING_BYTES / 2;

const mb = (bytes) => `${(bytes / (1024 * 1024)).toFixed(1)} MB`;

/** The runtime's real ceiling: resizable memories report it, older ones do not. */
function heapCeilingBytes(Module) {
  const buffer = Module?.HEAPU8?.buffer;
  const max = buffer && Number.isFinite(buffer.maxByteLength) ? buffer.maxByteLength : 0;
  return max > 0 ? max : WASM_CEILING_BYTES;
}

/** Size in bytes of a source we can measure without reading it. */
function sourceSize(source) {
  if (typeof Blob !== 'undefined' && source instanceof Blob) return source.size;
  // A range spec ({url, size} / {blob, size}) declares its size; without this
  // the budget check sees 0 and a failed ranged open falls through to the inline
  // path, which cannot open a spec at all ("unsupported document source") and
  // buries the engine's actual error.
  if (isRangeSpec(source)) return Number(source.size) || 0;
  if (typeof ArrayBuffer !== 'undefined' && (source instanceof ArrayBuffer || ArrayBuffer.isView(source))) {
    return source.byteLength;
  }
  return 0;
}

const RANGED_HINT =
  'Open it with ranged loading instead, so the parser reads only the byte ranges it needs: ' +
  "PdfDocument.open({ url, size, worker: true }) — or { blob, size, worker: true } for a local file. " +
  'The mounted viewer does this automatically when the file is a Blob and { worker: true } is set.';

/** The file alone cannot fit the heap, with or without growth. */
function tooLargeToCopy(size) {
  return new Error(
    `wasm-pdf-reader: this PDF is ${mb(size)} — too large to copy into the WebAssembly heap ` +
    `(${mb(WASM_CEILING_BYTES)} ceiling). ${RANGED_HINT}`,
  );
}

/** The copy would fit a 1 GiB heap, but this runtime refused to grow. */
function heapStalled(size, Module) {
  const heap = Module?.HEAPU8?.length || 0;
  return new Error(
    `wasm-pdf-reader: this PDF is ${mb(size)} and the WebAssembly heap could not grow past ` +
    `${mb(heap)} (its ceiling is ${mb(heapCeilingBytes(Module))}). Mobile browsers — iOS in ` +
    `particular — cap wasm memory well below that. ${RANGED_HINT}`,
  );
}

async function copyIntoWasm(Module, source) {
  if (typeof Blob !== 'undefined' && source instanceof Blob) {
    const size = source.size;
    if (!size) throw new Error('wasm-pdf-reader: empty file');
    if (size > COPY_BUDGET_BYTES) throw tooLargeToCopy(size);
    const ptr = Module._wpdf_alloc(size);
    if (!ptr) throw heapStalled(size, Module);
    try {
      for (let off = 0; off < size; off += CHUNK_BYTES) {
        const end = Math.min(size, off + CHUNK_BYTES);
        const chunk = new Uint8Array(await source.slice(off, end).arrayBuffer());
        Module.HEAPU8.set(chunk, ptr + off);      // re-read after any growth
      }
      return { ptr, size };
    } catch (err) {
      Module._wpdf_free(ptr);
      throw err;
    }
  }
  const bytes = await toBytes(source);
  if (!bytes.length) throw new Error('wasm-pdf-reader: empty file');
  if (bytes.length > COPY_BUDGET_BYTES) throw tooLargeToCopy(bytes.length);
  const ptr = Module._wpdf_alloc(bytes.length);
  if (!ptr) throw heapStalled(bytes.length, Module);
  Module.HEAPU8.set(bytes, ptr);
  return { ptr, size: bytes.length };
}

export async function toBytes(source) {
  if (source == null) throw new TypeError('wasm-pdf-reader: no document given');
  if (source instanceof Uint8Array) return source;
  if (source instanceof ArrayBuffer) return new Uint8Array(source);
  if (typeof source === 'string' || source instanceof URL) {
    const res = await fetch(source);
    if (!res.ok) throw new Error(`wasm-pdf-reader: HTTP ${res.status} for ${source}`);
    return new Uint8Array(await res.arrayBuffer());
  }
  if (typeof Response !== 'undefined' && source instanceof Response) {
    if (!source.ok) throw new Error(`wasm-pdf-reader: HTTP ${source.status}`);
    return new Uint8Array(await source.arrayBuffer());
  }
  if (typeof Blob !== 'undefined' && source instanceof Blob) {
    return new Uint8Array(await source.arrayBuffer());
  }
  if (typeof source.arrayBuffer === 'function') {
    return new Uint8Array(await source.arrayBuffer());
  }
  throw new TypeError('wasm-pdf-reader: unsupported document source');
}

/**
 * @typedef {Object} RenderOptions
 * @property {number} [scale=1.5]        CSS pixels per PDF point (72pt = 1in).
 * @property {number} [dpr=1]            device pixel ratio, multiplies `scale` for the bitmap.
 * @property {number} [maxPixels]        refuse pages larger than this (default 40M, the wasm cap).
 * @property {number} [page]             page index, filled in for `pixelTransform`.
 * @property {(rgba: Uint8ClampedArray, info: {width:number,height:number,page:number,scale:number}) => (ImageData|void)} [pixelTransform]
 *   Hook to post-process the RGBA bytes — invert for dark mode, tint, watermark.
 * @property {boolean} [withText=false]  also return the page text layer.
 */

/**
 * A parsed PDF. Construct with `PdfDocument.open(source, options)`.
 */
export class PdfDocument {
  /** @type {PdfDocument|null} */
  static _noop = null;

  /**
   * @param {Object} module emscripten Module
   * @param {number} handle wasm doc handle
   * @param {Uint8Array} bytes kept alive so wasm can copy from it on demand
   */
  constructor(module, handle, bytes, options = {}) {
    this.Module = module;
    this._handle = handle;
    this._bytes = bytes;
    this._closed = false;
    this._cache = new PageCache({
      enabled: options.pageCache !== false,
      budgetBytes: options.pageCacheBytes,
    });
    this._visiblePage = 0;
    this._sizeCache = new Map();
    this._textCache = new Map();
    this._linkCache = new Map();
    this._workerError = null;
  }

  /** Which path this document runs on: 'worker' or 'inline'. */
  get mode() { return 'inline'; }

  /** True for a worker-backed document (see remote.js). */
  get isWorker() { return false; }

  /** Why `{worker:true}` fell back to the inline path, or null. */
  get workerError() { return this._workerError; }

  set workerError(err) { this._workerError = err || null; }

  /**
   * Parse a PDF. The bytes are copied into wasm memory; the source can be released.
   *
   * With `{ worker: true }` the file is parsed and rendered inside a Web Worker
   * and the result is a `PdfRemoteDocument` (same surface; text/link accessors
   * are pre-cached by the render, search returns a Promise). When a worker
   * cannot start — no `Worker`, a 404 on `workerUrl`, a CSP that blocks it — the
   * call falls back to the inline path and records why on `workerError`, unless
   * `worker: 'strict'` was asked for.
   *
   * @param {ArrayBuffer|Uint8Array|Blob|File|string|URL|Response} source
   * @param {import('./loader.js').ModuleOptions & {filename?: string, password?: string,
   *          worker?: boolean|'strict', workerUrl?: string, workerFactory?: Function}} [options]
   * @returns {Promise<PdfDocument>}
   */
  static async open(source, options = {}) {
    if (isRangeSpec(source) && !options.worker) {
      throw new Error('wasm-pdf-reader: a ranged source needs { worker: true } — its '
        + 'reads are synchronous, so it can only run off the main thread');
    }
    let workerError = null;
    if (options.worker) {
      if (typeof Worker === 'undefined') {
        const err = new Error('wasm-pdf-reader: no Web Worker support in this environment');
        if (options.worker === 'strict') throw err;
        workerError = err;
      } else {
        try {
          const remote = await import('./remote.js');
          return await remote.openRemote(source, options);
        } catch (err) {
          // A range spec has no inline path — falling back would replace the
          // engine's reason (too large, bad xref, wrong password) with
          // "unsupported document source".
          if (options.worker === 'strict' || isRangeSpec(source)) throw err;
          workerError = err;           // fall through to the inline path, loudly
        }
      }
    }
    // A file above the copy budget cannot be opened inline on any device, so
    // ranged loading is the only path rather than an optimisation. Take it even
    // when the caller did not ask for a worker — but if it fails, throw that
    // error instead of attempting a copy that is certain to run out of memory.
    const size = sourceSize(source);
    if (size > COPY_BUDGET_BYTES) {
      if (typeof Worker === 'undefined') throw tooLargeToCopy(size);
      try {
        const remote = await import('./remote.js');
        return await remote.openRemote(source, { ...options, worker: true, ranged: true });
      } catch (err) {
        err.message = `${tooLargeToCopy(size).message} (ranged loading failed: ${err.message})`;
        throw err;
      }
    }
    const doc = await PdfDocument._openInline(source, options);
    if (workerError) {
      doc.workerError = workerError;
      if (typeof globalThis.dispatchEvent === 'function') {
        try {
          globalThis.dispatchEvent(new CustomEvent('wpdf-worker-fallback', { detail: { error: workerError } }));
        } catch { /* CustomEvent is not universal */ }
      }
    }
    return doc;
  }

  /**
   * Open a document that is **not** loaded into wasm: the parser pulls byte
   * ranges on demand (M9.2). Requires worker mode — the reads are synchronous.
   *
   * @param {{url?: string, blob?: Blob, size?: number}} spec
   * @param {Object} [options] ModuleOptions + password
   */
  static async openRanged(spec, options = {}) {
    const Module = await loadModule(options);
    if (typeof Module._wpdf_open_stream !== 'function') {
      throw new Error('wasm-pdf-reader: this build has no ranged-source support (rebuild the wasm)');
    }
    const { installRangeFetcher, clearRangeFetcher } = await import('./range-source.js');
    const { size } = installRangeFetcher(spec);

    let handle = 0;
    let pwPtr = 0;
    const password = options.password ? String(options.password) : '';
    try {
      if (password) {
        const pw = new TextEncoder().encode(password);
        pwPtr = Module._wpdf_alloc(pw.length);
        if (!pwPtr) throw new Error('out of memory');
        Module.HEAPU8.set(pw, pwPtr);
        handle = Module._wpdf_open_stream(size, pwPtr, pw.length);
      } else {
        handle = Module._wpdf_open_stream(size, 0, 0);
      }
    } finally {
      if (pwPtr) Module._wpdf_free(pwPtr);
    }
    if (!handle) {
      const msg = Module.UTF8ToString(Module._wpdf_last_error()) || 'cannot read PDF';
      // A refused document must not leave the per-worker hook installed, or the
      // next ranged open fails with "one ranged document per worker" and hides
      // the real reason (which is the message we are about to throw).
      clearRangeFetcher();
      throw new Error(`wasm-pdf-reader: ${msg}`);
    }
    const doc = new PdfDocument(Module, handle, null, options);
    doc.filename = options.filename || spec.url || (spec.blob && spec.blob.name) || '';
    doc.ranged = true;
    // The fetch hook is per worker (one ranged document at a time), so it has to
    // be released with the document. Leaking it here made the *next* big file
    // throw "one ranged document per worker", fall back to inline and copy the
    // whole file into wasm — the out-of-memory this path exists to avoid.
    doc._clearRange = clearRangeFetcher;
    return doc;
  }

  static async _openInline(source, options = {}) {
    const Module = await loadModule(options);
    const { ptr, size } = await copyIntoWasm(Module, source);
    let handle = 0;
    const password = options.password ? String(options.password) : '';
    let pwPtr = 0;
    try {
      if (password && typeof Module._wpdf_open_pw !== 'function') {
        throw new Error('wasm-pdf-reader: this build cannot open password-protected PDFs (reload to update the wasm module)');
      }
      // Test hook (M9.2): force the parser's resident window down so every
      // slide path runs against a normal in-memory file.
      const winBytes = Number(options.windowBytes);
      if (!password && Number.isFinite(winBytes) && winBytes > 0
          && typeof Module._wpdf_open_windowed === 'function') {
        handle = Module._wpdf_open_windowed(ptr, size, winBytes | 0);
      } else if (password) {
        const pw = new TextEncoder().encode(password);
        pwPtr = Module._wpdf_alloc(pw.length);
        if (!pwPtr) throw new Error('out of memory');
        Module.HEAPU8.set(pw, pwPtr);        // re-read the view: alloc may grow it
        handle = Module._wpdf_open_pw(ptr, size, pwPtr, pw.length);
      } else {
        handle = Module._wpdf_open(ptr, size);
      }
    } finally {
      if (pwPtr) Module._wpdf_free(pwPtr);
      Module._wpdf_free(ptr);
    }
    if (!handle) {
      const msg = Module.UTF8ToString(Module._wpdf_last_error()) || 'cannot read PDF';
      throw new Error(`wasm-pdf-reader: ${msg}`);
    }
    // the bytes now live in wasm memory only; the JS copy is not kept
    const doc = new PdfDocument(Module, handle, null, options);
    doc.filename = options.filename || '';
    return doc;
  }

  get pageCount() {
    this._assertOpen();
    return this.Module._wpdf_page_count(this._handle);
  }

  /** Document information dictionary, or {} when the file has none. */
  get metadata() {
    this._assertOpen();
    try {
      return JSON.parse(this.Module.UTF8ToString(this.Module._wpdf_info_json(this._handle)));
    } catch {
      return {};
    }
  }

  /** AcroForm fields as [{name, type, value, page, rect}]. */
  get form() {
    if (!this._handle) return [];
    try {
      return JSON.parse(this.Module.UTF8ToString(this.Module._wpdf_form_json(this._handle)));
    } catch {
      return [];
    }
  }

  /**
   * Fill a field by name. The value lands in /V (and /AS for buttons) inside the
   * in-memory document, so the next render shows it; returns false when no field
   * matches.
   */
  setField(name, value) {
    if (!this._handle) return false;
    const M = this.Module;
    const enc = (str) => {
      const bytes = new TextEncoder().encode(String(str));
      const ptr = M._wpdf_alloc(bytes.length + 1);
      if (!ptr) throw new Error('out of memory');
      M.HEAPU8.set(bytes, ptr);            // re-read the view: alloc may grow it
      M.HEAPU8[ptr + bytes.length] = 0;
      return ptr;
    };
    const a = enc(name);
    const b = enc(value);
    try {
      const ok = M._wpdf_set_field(this._handle, a, b) === 1;
      if (ok) this._cache?.clear();   // the next render must repaint, not replay
      return ok;
    } finally {
      M._wpdf_free(a);
      M._wpdf_free(b);
    }
  }

  /** Outline / bookmarks as [{title, page}], page -1 when the dest is unresolved. */
  get outline() {
    if (!this._handle) return [];
    try {
      return JSON.parse(this.Module.UTF8ToString(this.Module._wpdf_outline_json(this._handle)));
    } catch {
      return [];
    }
  }

  /** Non-fatal parse warnings ("rebuilt xref", "unsupported filter", ...). */
  get warnings() {
    this._assertOpen();
    try {
      return JSON.parse(this.Module.UTF8ToString(this.Module._wpdf_warnings_json(this._handle)));
    } catch {
      return [];
    }
  }

  _assertOpen() {
    if (this._closed) throw new Error('wasm-pdf-reader: document is closed');
  }

  /**
   * Page geometry in points, with rotation applied to width/height.
   * @param {number} index
   * @returns {{width:number, height:number, rotation:number, boxX0:number, boxY0:number}}
   */
  getPageSize(index) {
    this._assertOpen();
    if (this._sizeCache.has(index)) return this._sizeCache.get(index);
    const n = this.pageCount;
    if (index < 0 || index >= n) throw new RangeError(`wasm-pdf-reader: page ${index} of ${n}`);

    const out = this.Module._wpdf_alloc(5 * 8);
    try {
      const ok = this.Module._wpdf_page_size(this._handle, index, out);
      const view = new Float64Array(this.Module.HEAPU8.buffer, out, 5);
      if (!ok) throw new Error('wasm-pdf-reader: cannot read page size');
      const [w, h, rotation, boxX0, boxY0] = Array.from(view);
      // The rasterizer swaps the axes for /Rotate 90 and 270, so report the
      // size the page is actually drawn at; keep the raw box for overlays.
      const swap = rotation === 90 || rotation === 270;
      const info = {
        width: swap ? h : w,
        height: swap ? w : h,
        rotation,
        boxX0,
        boxY0,
        mediaWidth: w,
        mediaHeight: h,
      };
      this._sizeCache.set(index, info);
      return info;
    } finally {
      this._wpdfFree(out);
    }
  }

  _wpdfFree(ptr) {
    this.Module._wpdf_free(ptr);
  }

  /**
   * Rasterize a page in wasm and return its pixels.
   *
   * @param {number} index
   * @param {RenderOptions} [options]
   * @returns {Promise<{imageData: ImageData, width:number, height:number, cssWidth:number, cssHeight:number, scale:number, stats?: number[]}>}
   */
  async render(index, options = {}) {
    this._assertOpen();
    const scale = options.scale ?? 1.5;
    const dpr = options.dpr ?? 1;
    const deviceScale = scale * dpr;
    const size = this.getPageSize(index);

    // M9.3: a page already rasterized at this scale is a bitmap copy away. A
    // pixelTransform (dark mode, tinting) mutates the bytes, so it bypasses the
    // cache rather than poisoning it.
    const cacheable = this._cache && this._cache.enabled && typeof options.pixelTransform !== 'function';
    if (cacheable) {
      const hit = this._cache.get(index, deviceScale);
      if (hit) return { ...hit, stats: options.stats ? this._stats(index) : undefined };
    }

    return serialise(async () => {
      this._assertOpen();
      const ok = this.Module._wpdf_render_page(this._handle, index, deviceScale);
      if (!ok) {
        const msg = this.Module.UTF8ToString(this.Module._wpdf_last_error());
        throw new Error(`wasm-pdf-reader: ${msg || 'render failed'}`);
      }
      const w = this.Module._wpdf_bitmap_width();
      const h = this.Module._wpdf_bitmap_height();
      const ptr = this.Module._wpdf_bitmap_ptr();
      // Copy out before any other render can touch the shared bitmap.
      const rgba = new Uint8ClampedArray(this.Module.HEAPU8.buffer, ptr, w * h * 4).slice();

      let imageData;
      if (typeof ImageData !== 'undefined') {
        imageData = new ImageData(rgba, w, h);
      } else {
        imageData = { data: rgba, width: w, height: h };
      }

      if (typeof options.pixelTransform === 'function') {
        const replaced = options.pixelTransform(imageData.data, {
          width: w, height: h, page: index, scale: deviceScale,
        });
        if (replaced && replaced.data) imageData = replaced;
      }

      const result = {
        imageData,
        width: w,
        height: h,
        cssWidth: size.width * scale,
        cssHeight: size.height * scale,
        scale,
        stats: options.stats ? this._stats(index) : undefined,
      };
      if (cacheable) this._cache.set(index, deviceScale, result, PageCache.key(this._visiblePage ?? index, deviceScale));
      return result;
    });
  }

  /** Tell the cache which page must survive eviction (the one on screen). */
  pinPage(index, scale = 1.5, dpr = 1) {
    this._visiblePage = index;
    this._cache?.pin(index, scale * dpr);
  }

  /** Page-cache counters: {entries, bytes, budget, hits, misses, evictions}. */
  get cacheStats() {
    return this._cache ? this._cache.stats() : null;
  }

  /** Drop cached bitmaps (theme change, memory pressure). */
  clearPageCache() {
    this._cache?.clear();
  }

  /**
   * Render straight into a canvas, sizing its backing store for `dpr`.
   * @param {HTMLCanvasElement} canvas
   * @param {number} index
   * @param {RenderOptions & {cssScale?: number}} [options]
   */
  async renderToCanvas(canvas, index, options = {}) {
    const out = await this.render(index, options);
    const dpr = options.dpr ?? 1;
    canvas.width = out.width;
    canvas.height = out.height;
    if (options.cssScale !== false) {
      canvas.style.width = `${out.width / dpr}px`;
      canvas.style.height = `${out.height / dpr}px`;
    }
    const ctx = canvas.getContext('2d');
    ctx.putImageData(out.imageData, 0, 0);
    return out;
  }

  /**
   * Link annotations on a page: `{ x0, y0, x1, y1, uri?, page? }`, page-space
   * (y up, origin bottom-left). `page` is a zero-based internal target.
   */
  getLinks(index) {
    this._assertOpen();
    if (this._linkCache.has(index)) return this._linkCache.get(index);
    const json = this.Module._wpdf_page_links_json(this._handle, index);
    const links = json ? JSON.parse(this.Module.UTF8ToString(json)) : [];
    this._linkCache.set(index, links);
    return links;
  }

  /** The text layer of a page: glyph runs with page-space boxes. */
  getText(index) {
    this._assertOpen();
    if (this._textCache.has(index)) return this._textCache.get(index);
    const json = this.Module._wpdf_page_text_json(this._handle, index);
    if (!json) throw new Error('wasm-pdf-reader: text extraction failed');
    const items = JSON.parse(this.Module.UTF8ToString(json));
    this._textCache.set(index, items);
    return items;
  }

  /** Async form of `getText`, for code that must also work on a worker document. */
  async loadText(index) { return this.getText(index); }

  /** Async form of `getLinks`. */
  async loadLinks(index) { return this.getLinks(index); }

  /** Plain text for one page. */
  getPageText(index, options) {
    return itemsToText(this.getText(index), options);
  }

  /** Plain text for the whole document (lazy: one page at a time). */
  getAllText(options) {
    const pages = [];
    for (let i = 0; i < this.pageCount; i++) pages.push(this.getPageText(i, options));
    return pages;
  }

  /**
   * Search the document. Pages are scanned lazily and the wasm text layer is
   * cached, so searching twice is cheap.
   * @param {string} query
   * @param {{caseSensitive?: boolean, limit?: number, pages?: number[]}} [options]
   */
  search(query, options = {}) {
    const limit = options.limit ?? 500;
    const hits = [];
    const pages = options.pages ?? Array.from({ length: this.pageCount }, (_, i) => i);
    for (const page of pages) {
      for (const hit of searchItems(this.getText(page), query, { caseSensitive: options.caseSensitive })) {
        hits.push({ ...hit, page });
        if (hits.length >= limit) return hits;
      }
    }
    return hits;
  }

  /**
   * How the bytes were obtained: `{ fetched, fetches, windowBytes }`. For a
   * ranged document `fetched` is the point — it says how much of the file the
   * engine actually read.
   */
  get sourceStats() {
    const M = this.Module;
    if (typeof M._wpdf_source_stats !== 'function' || !this._handle) return null;
    const out = M._wpdf_alloc(3 * 8);
    try {
      const n = M._wpdf_source_stats(this._handle, out, 3);
      if (!n) return null;
      const view = new Float64Array(M.HEAPU8.buffer, out, 3);
      const [fetched, fetches, windowBytes] = Array.from(view);
      return { fetched, fetches, windowBytes, fileSize: this.ranged ? undefined : fetched };
    } finally {
      M._wpdf_free(out);
    }
  }

  /** Render statistics for a page (needs a render or text extraction first). */
  _stats(index) {
    const out = this.Module._wpdf_alloc(7 * 4);
    try {
      const n = this.Module._wpdf_page_stats(this._handle, index, out, 7);
      const view = new Int32Array(this.Module.HEAPU8.buffer, out, Math.max(0, n));
      const [ops = 0, texts = 0, glyphs = 0, paths = 0, fills = 0, strokes = 0, unsupported = 0] = Array.from(view);
      return { ops, texts, glyphs, paths, fills, strokes, unsupported };
    } finally {
      this._wpdfFree(out);
    }
  }

  /**
   * Page-space box -> CSS pixels inside the rendered page. Useful for
   * highlight overlays built on `getText()` / `search()`.
   * @param {{x0:number,y0:number,x1:number,y1:number}} box
   * @param {number} index
   * @param {number} scale scale used when rendering
   */
  boxToCss(box, index, scale) {
    const size = this.getPageSize(index);
    return boxToCss(box, {
      height: size.height,
      boxY0: size.boxY0,
      scale,
    });
  }

  /** Release the wasm document. Rendering after this throws. */
  close() {
    if (this._closed) return;
    this._closed = true;
    this._textCache.clear();
    this._linkCache.clear();
    this._sizeCache.clear();
    this._cache?.clear();
    this.Module._wpdf_close(this._handle);
    this._handle = 0;
    // Free the worker's range hook so the next ranged document can install its
    // own (a per-worker singleton — see range-source.js).
    if (this._clearRange) {
      try { this._clearRange(); } catch { /* already gone */ }
      this._clearRange = null;
    }
  }
}

export default PdfDocument;
