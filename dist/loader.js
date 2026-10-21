// loader.js — instantiate the WebAssembly module once per page.
//
// The emscripten factory is imported statically so that bundlers (Vite,
// webpack, esbuild, Rollup) can resolve both the glue and `pdf-reader.wasm`
// next to it. Nothing is fetched until the first document is opened.

import createPdfReaderModule from './wasm/pdf-reader.mjs';

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
    const Module = await createPdfReaderModule(moduleOptions);
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
