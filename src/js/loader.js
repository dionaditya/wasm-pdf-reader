// loader.js — instantiate the WebAssembly module once per page.
//
// The emscripten factory is imported statically so that bundlers (Vite,
// webpack, esbuild, Rollup) can resolve both the glue and `pdf-reader.wasm`
// next to it. Nothing is fetched until the first document is opened.

import createPdfReaderModule from './wasm/pdf-reader.mjs';

const VERSION = '0.1.0';   // kept in sync with package.json by build-js.mjs

let instancePromise = null;

/**
 * Options that reach the emscripten factory.
 * @typedef {Object} ModuleOptions
 * @property {string} [wasmUrl]      URL of pdf-reader.wasm (overrides the default co-located path).
 * @property {ArrayBuffer|Uint8Array} [wasmBinary]  Pre-fetched wasm bytes.
 * @property {Object} [moduleOptions] Extra emscripten Module fields.
 * @property {Function} [factory]    A different emscripten factory (advanced).
 */

/**
 * Load (or reuse) the wasm module. Concurrent callers share one instance: the
 * module owns a single render bitmap, so one instance per page is what you want.
 * @param {ModuleOptions} [options]
 * @returns {Promise<Object>} the emscripten Module
 */
export async function loadModule(options = {}) {
  if (options.module) return options.module;               // pre-instantiated
  if (options.factory) return options.factory(options.moduleOptions || {});
  if (instancePromise) return instancePromise;

  instancePromise = (async () => {
    const moduleOptions = { ...(options.moduleOptions || {}) };
    if (options.wasmBinary) moduleOptions.wasmBinary = options.wasmBinary;
    if (options.wasmUrl) {
      const base = options.wasmUrl;
      moduleOptions.locateFile = (path) =>
        path.endsWith('.wasm') ? base : path;
    }
    // Fetch the binary ourselves with a cache-busting query: a stale glue
    // paired with a fresh .wasm (or the reverse) fails at instantiation with
    // "import function a:h must be callable", which is indistinguishable from
    // a code bug and only shows up where caches are aggressive (iOS).
    if (!moduleOptions.wasmBinary) {
      try {
        const base = moduleOptions.wasmUrl;
        const url = new URL(base || new URL('./wasm/pdf-reader.wasm', import.meta.url).href,
                            import.meta.url);
        if (base && !url.search) url.search = `?v=${VERSION}`;
        if (url.protocol === 'file:') {
          const specifier = 'node:' + 'fs/promises';
          const { readFile } = await import(/* @vite-ignore */ specifier);
          moduleOptions.wasmBinary = await readFile(url);
        } else {
          const res = await fetch(url, { cache: 'no-cache' });
          if (res.ok) moduleOptions.wasmBinary = new Uint8Array(await res.arrayBuffer());
        }
      } catch {
        // fall back to the glue's own resolution
      }
    }
    let Module;
    try {
      Module = await createPdfReaderModule(moduleOptions);
    } catch (err) {
      // "LinkError: import function a:h must be callable" means the glue and
      // the .wasm came from different builds — almost always a cached glue.
      // Retry once with a cache-busted module URL before giving up.
      if (!/LinkError|must be callable|Import #0/.test(String(err && err.message))) throw err;
      const fresh = await import(/* @vite-ignore */
        new URL('./wasm/pdf-reader.mjs', import.meta.url).href + `?v=${Date.now()}`);
      Module = await fresh.default(moduleOptions);
    }
    if (!Module._wpdf_abi_version) {
      throw new Error('wasm-pdf-reader: module loaded without the wpdf exports');
    }
    return Module;
  })();

  try {
    return await instancePromise;
  } catch (err) {
    instancePromise = null;   // let the next call retry
    throw err;
  }
}

/** Forget the shared instance (tests, hot reload). */
export function resetModule() {
  instancePromise = null;
}
