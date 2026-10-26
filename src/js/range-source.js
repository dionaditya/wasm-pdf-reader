// range-source.js — the synchronous byte fetcher a ranged document needs.
//
// The parser is a synchronous lexer, so "get me bytes at offset N" has to block.
// That is only acceptable off the main thread, which is why ranged loading is a
// worker-mode feature (M9.1): the worker blocks, the tab does not.
//
// Two backing stores, both synchronous inside a worker:
//   * Blob/File  — FileReaderSync on a slice. No network at all.
//   * http(s) URL — XMLHttpRequest with a `Range:` header, `false` for async.
//     A server that ignores Range (200 instead of 206) still works: the body is
//     the whole file, and we slice it. That is correct but costs the bandwidth
//     the request was meant to save, so `stats.rangeIgnored` records it.
//
// The fetcher installs itself as `globalThis.__wpdfRangeFetch`, which is the
// symbol the wasm side calls (see wpdf_js_range_fetch in wpdf_api.cpp).

/** Counters, so a test can prove the engine read a fraction of the file. */
export const stats = {
  bytes: 0,
  requests: 0,
  rangeIgnored: 0,
  fullFetch: false,
};

export function resetStats() {
  stats.bytes = 0;
  stats.requests = 0;
  stats.rangeIgnored = 0;
  stats.fullFetch = false;
}

/** Is this source a range spec rather than bytes? */
export function isRangeSpec(source) {
  if (!source || typeof source !== 'object') return false;
  if (typeof Blob !== 'undefined' && source instanceof Blob) return false;
  if (typeof ArrayBuffer !== 'undefined' && (source instanceof ArrayBuffer || ArrayBuffer.isView(source))) return false;
  return typeof source.url === 'string' || (typeof Blob !== 'undefined' && source.blob instanceof Blob);
}

/**
 * Install the synchronous fetcher for `spec` on globalThis.
 * @param {{url?: string, blob?: Blob, size?: number}} spec
 * @returns {{size: number}}
 */
let activeSpec = null;

export function installRangeFetcher(spec) {
  if (activeSpec && activeSpec !== spec) {
    throw new Error('wasm-pdf-reader: one ranged document per worker — the fetch hook is per-worker (open them in sequence, or use separate pools)');
  }
  activeSpec = spec;
  const blob = spec.blob instanceof Blob ? spec.blob : null;
  const url = typeof spec.url === 'string' ? spec.url : '';
  const size = Number(spec.size) || (blob ? blob.size : 0);
  if (!blob && !url) throw new Error('wasm-pdf-reader: a ranged source needs a url or a blob');
  if (!size) throw new Error('wasm-pdf-reader: a ranged source needs the file size');
  if (typeof FileReaderSync === 'undefined' && !url) {
    throw new Error('wasm-pdf-reader: ranged loading needs a worker (FileReaderSync is worker-only)');
  }

  // An origin that ignores `Range:` answers 200 with the whole body; the first
  // such response is kept and every later read is a slice of it. Re-requesting
  // the file per window would download it dozens of times.
  let whole = null;

  const read = (off, len) => {
    const start = Math.max(0, off | 0);
    const end = Math.min(size, start + Math.max(0, len | 0));
    if (end <= start) return new Uint8Array(0);
    if (whole) return whole.subarray(start, end);
    if (blob) {
      const slice = blob.slice(start, end);
      const buf = new FileReaderSync().readAsArrayBuffer(slice);
      stats.bytes += buf.byteLength;
      stats.requests++;
      return new Uint8Array(buf);
    }
    const xhr = new XMLHttpRequest();
    xhr.open('GET', url, false);                    // synchronous, worker only
    xhr.setRequestHeader('Range', `bytes=${start}-${end - 1}`);
    try { xhr.responseType = 'arraybuffer'; } catch { /* older engines */ }
    xhr.send(null);
    const body = xhr.response;
    if (!body) throw new Error(`wasm-pdf-reader: range request failed (${xhr.status})`);
    stats.requests++;
    if (xhr.status === 200) {
      // Range not honoured: keep the body and serve everything from it.
      stats.rangeIgnored++;
      stats.fullFetch = true;
      whole = new Uint8Array(body);
      stats.bytes += whole.length;
      return whole.subarray(start, end);
    }
    if (xhr.status !== 206) {
      throw new Error(`wasm-pdf-reader: range request failed (${xhr.status})`);
    }
    const bytes = new Uint8Array(body);
    stats.bytes += bytes.length;
    return bytes;
  };

  globalThis.__wpdfRangeFetch = read;
  return { size };
}

export function clearRangeFetcher() {
  delete globalThis.__wpdfRangeFetch;
  activeSpec = null;
}
