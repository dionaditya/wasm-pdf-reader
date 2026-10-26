// worker.js — the Web Worker entry point: the wasm engine, off the main thread.
//
// The main thread keeps no document and no wasm instance in worker mode. This
// file owns one module instantiation (the module carries a single render
// bitmap, so one worker serves one render at a time) and answers a small
// request/response protocol:
//
//   { id, op:'open',   payload:{ blob|bytes, filename, password, wasmUrl, wasmBinary } }
//   { id, op:'render', payload:{ docId, page, scale, stats } }   -> rgba (transferred) + text + links
//   { id, op:'text',   payload:{ docId, page } }                 -> glyph runs
//   { id, op:'links',  payload:{ docId, page } }                 -> link annotations
//   { id, op:'search', payload:{ docId, query, options } }       -> hits
//   { id, op:'pageText', payload:{ docId, page, options } }      -> plain text
//   { id, op:'field',  payload:{ docId, name, value } }          -> boolean
//   { id, op:'close',  payload:{ docId } }
//
// Every reply is `{ id, ok:true, result }` or `{ id, ok:false, error }`. The
// worker announces itself with `{ op:'ready' }` once the script has evaluated,
// which is what lets the client distinguish "worker is running" from
// "the worker script 404'd" and fall back to the inline path.

import { PdfDocument } from './document.js';

/** @type {Map<number, PdfDocument>} */
const docs = new Map();
let nextDocId = 0;

// One queue: the wasm module owns a single bitmap, and text extraction mutates
// the same document, so nothing may interleave.
let chain = Promise.resolve();
function enqueue(task) {
  const run = chain.then(task, task);
  chain = run.catch(() => {});
  return run;
}

function summarise(doc, filename) {
  const pageCount = doc.pageCount;
  const sizes = [];
  for (let i = 0; i < pageCount; i++) sizes.push(doc.getPageSize(i));
  return {
    pageCount,
    sizes,
    metadata: doc.metadata,
    outline: doc.outline,
    form: doc.form,
    warnings: doc.warnings,
    filename: filename || doc.filename || '',
    ranged: !!doc.ranged,
  };
}

function message(err) {
  return String((err && err.message) || err || 'unknown worker error');
}

const handlers = {
  async open(payload) {
    const common = {
      filename: payload.filename || '',
      password: payload.password || '',
      wasmUrl: payload.wasmUrl || undefined,
      wasmBinary: payload.wasmBinary || undefined,
      // No page cache here: every render reply transfers the RGBA buffer to the
      // main thread, which detaches it — a cached copy would be a detached
      // buffer by the next request. The main-thread document owns the cache.
      pageCache: false,
    };
    // `range` means: the file was never copied here, pull byte ranges on demand.
    const doc = payload.range
      ? await PdfDocument.openRanged(payload.range, common)
      : await PdfDocument.open(payload.blob || payload.bytes, common);
    const docId = ++nextDocId;
    docs.set(docId, doc);
    return { result: { docId, summary: summarise(doc, payload.filename) } };
  },

  async render(payload) {
    const doc = need(payload.docId);
    const out = await doc.render(payload.page, { scale: payload.scale, stats: !!payload.stats });
    // Text and links ride along: the viewer builds those layers right after the
    // bitmap lands, and a second round trip would leave the page briefly
    // selectable-looking but empty.
    const text = safe(() => doc.getText(payload.page), []);
    const links = safe(() => doc.getLinks(payload.page), []);
    return {
      result: {
        rgba: out.imageData.data.buffer,
        width: out.width,
        height: out.height,
        stats: out.stats,
        text,
        links,
      },
      transfer: [out.imageData.data.buffer],
    };
  },

  async text(payload) {
    return { result: need(payload.docId).getText(payload.page) };
  },

  async links(payload) {
    return { result: need(payload.docId).getLinks(payload.page) };
  },

  async search(payload) {
    return { result: need(payload.docId).search(payload.query, payload.options || {}) };
  },

  async pageText(payload) {
    return { result: need(payload.docId).getPageText(payload.page, payload.options) };
  },

  async field(payload) {
    return { result: need(payload.docId).setField(payload.name, payload.value) };
  },

  async stats(payload) {
    const doc = need(payload.docId);
    return { result: { source: doc.sourceStats || null, heapBytes: doc.Module ? doc.Module.HEAPU8.length : 0 } };
  },

  async close(payload) {
    const doc = docs.get(payload.docId);
    if (doc) {
      docs.delete(payload.docId);
      doc.close();
    }
    return { result: true };
  },
};

function need(docId) {
  const doc = docs.get(docId);
  if (!doc) throw new Error('wasm-pdf-reader: document is closed');
  return doc;
}

function safe(fn, fallback) {
  try { return fn(); } catch { return fallback; }
}

self.addEventListener('message', (ev) => {
  const msg = ev.data || {};
  const handler = handlers[msg.op];
  if (!handler) {
    self.postMessage({ id: msg.id, ok: false, error: `unknown op ${msg.op}` });
    return;
  }
  enqueue(async () => {
    try {
      const out = (await handler(msg.payload || {})) || {};
      self.postMessage({ id: msg.id, ok: true, result: out.result }, out.transfer || []);
    } catch (err) {
      self.postMessage({ id: msg.id, ok: false, error: message(err) });
    }
  });
});

self.postMessage({ op: 'ready' });
