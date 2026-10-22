// document.js — one open PDF: pages, rendering, text, search.
//
// This is the framework-agnostic half of the library. It has no DOM
// dependency beyond a canvas it is handed (or that it creates), so it can be
// used from a worker, a test, a vanilla page or React.

import { loadModule } from './loader.js';
import { decodeJbig2 } from './jbig2.js';
import { itemsToText, searchItems, boxToCss } from './text.js';

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

async function copyIntoWasm(Module, source) {
  if (typeof Blob !== 'undefined' && source instanceof Blob) {
    const size = source.size;
    if (!size) throw new Error('wasm-pdf-reader: empty file');
    const ptr = Module._wpdf_alloc(size);
    if (!ptr) throw new Error('wasm-pdf-reader: out of memory');
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
  const ptr = Module._wpdf_alloc(bytes.length);
  if (!ptr) throw new Error('wasm-pdf-reader: out of memory');
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
  constructor(module, handle, bytes) {
    this.Module = module;
    this._handle = handle;
    this._bytes = bytes;
    this._closed = false;
    this._sizeCache = new Map();
    this._textCache = new Map();
    this._imageCache = new Map();     // objNum -> { data, stride } decoded JBIG2
    this._linkCache = new Map();
  }

  // ── JBIG2 ───────────────────────────────────────────────────────────────
  // Bi-level scans are decoded by the vendored pdf.js.jbig2 module: the render
  // paints a hole, reports the XObjects it could not decode, and we hand the
  // finished bitmap back before rendering again. Decoded images are cached for
  // the life of the document, so re-rendering a page costs nothing extra.
  _imageStream(objNum, globals) {
    const M = this.Module;
    const ptr = M._wpdf_image_bytes(this._handle, objNum, globals ? 1 : 0);
    if (!ptr) return null;
    const len = M._wpdf_image_bytes_len(this._handle);
    if (!len) return null;
    return new Uint8Array(M.HEAPU8.buffer, ptr, len).slice();
  }

  async _resolveImages() {
    const M = this.Module;
    const json = M._wpdf_pending_images_json(this._handle);
    const list = json ? JSON.parse(M.UTF8ToString(json)) : [];
    if (!list.length) return false;
    M._wpdf_clear_pending_images(this._handle);
    let injected = false;
    for (const item of list) {
      const objNum = item.obj | 0;
      let decoded = this._imageCache.get(objNum);
      if (!decoded) {
        const data = this._imageStream(objNum, false);
        if (!data) continue;
        const globals = this._imageStream(objNum, true);
        try {
          decoded = await decodeJbig2(data, item.width, item.height, globals);
        } catch (err) {
          this._warnings?.push?.(String(err));
          continue;
        }
        this._imageCache.set(objNum, decoded);
      }
      const ptr = M._wpdf_alloc(decoded.data.length);
      if (!ptr) continue;
      M.HEAPU8.set(decoded.data, ptr);       // re-read after any growth
      M._wpdf_set_image(this._handle, objNum, item.width, item.height, ptr, decoded.stride);
      M._wpdf_free(ptr);
      injected = true;
    }
    return injected;
  }

  /**
   * Parse a PDF. The bytes are copied into wasm memory; the source can be released.
   * @param {ArrayBuffer|Uint8Array|Blob|File|string|URL|Response} source
   * @param {import('./loader.js').ModuleOptions & {filename?: string, password?: string}} [options]
   * @returns {Promise<PdfDocument>}
   */
  static async open(source, options = {}) {
    const Module = await loadModule(options);
    const { ptr, size } = await copyIntoWasm(Module, source);
    let handle = 0;
    const password = options.password ? String(options.password) : '';
    let pwPtr = 0;
    try {
      if (password) {
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
    const doc = new PdfDocument(Module, handle, null);
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

    return serialise(async () => {
      this._assertOpen();
      this.Module._wpdf_clear_pending_images(this._handle);
      let ok = this.Module._wpdf_render_page(this._handle, index, deviceScale);
      if (ok && this.Module._wpdf_pending_images_json) {
        // A page with a JBIG2 scan: decode, upload, render once more.
        await this._resolveImages().then((again) => {
          if (again) ok = this.Module._wpdf_render_page(this._handle, index, deviceScale);
        });
      }
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

      return {
        imageData,
        width: w,
        height: h,
        cssWidth: size.width * scale,
        cssHeight: size.height * scale,
        scale,
        stats: options.stats ? this._stats(index) : undefined,
      };
    });
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
    this._imageCache.clear();
    this._linkCache.clear();
    this._sizeCache.clear();
    this.Module._wpdf_close(this._handle);
    this._handle = 0;
  }
}

export default PdfDocument;
