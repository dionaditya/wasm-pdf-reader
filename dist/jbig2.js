// jbig2.js — JBIG2 decoding, delegated to pdf.js.jbig2.
//
// The wasm module core carries no JBIG2 decoder: PDFium's decoder is BSD/Apache
// licensed and Mozilla already ships it compiled to wasm (external/jbig2 in
// pdf.js), so it lives here as a separate module the JS layer drives. A page
// render that meets a JBIG2 image reports it through wpdf_pending_images_json;
// we decode it once, upload the bitmap, and render again.
//
// Licensing: jbig2.js / jbig2.wasm come from mozilla/pdf.js.jbig2 (Apache-2.0,
// PDFium derived). See wasm/jbig2/LICENSE_* in the published package.

let modulePromise = null;

// Candidate locations for jbig2.wasm. The first that loads wins: the module's
// own directory covers the plain-ESM layout, the absolute paths cover bundles
// (where import.meta.url is the app bundle, not this file).
async function loadWasmBytes() {
  const candidates = [
    new URL('./jbig2/jbig2.wasm', import.meta.url),
    new URL('./wasm/jbig2/jbig2.wasm', import.meta.url),
    new URL('/dist/wasm/jbig2/jbig2.wasm', 'http://x/'),
  ].filter((u, i, a) => a.findIndex((v) => v.href === u.href) === i);
  let lastErr = null;
  for (const url of candidates) {
    try {
      return await readBytes(url);
    } catch (err) {
      lastErr = err;
    }
  }
  throw lastErr || new Error('wasm-pdf-reader: jbig2.wasm not found');
}

async function readBytes(url) {
  if (url.protocol === 'file:') {
    // Node / test runs. Kept as a variable specifier so browser bundlers do not
    // try to resolve node:fs at build time.
    const specifier = 'node:' + 'fs/promises';
    const { readFile } = await import(/* @vite-ignore */ specifier);
    return readFile(url);
  }
  const res = await fetch(url);
  if (!res.ok) throw new Error(`wasm-pdf-reader: cannot load jbig2.wasm (${res.status})`);
  return new Uint8Array(await res.arrayBuffer());
}

async function getModule() {
  if (!modulePromise) {
    modulePromise = (async () => {
      const [{ default: JBig2 }, wasm] = await Promise.all([
        import('./jbig2/jbig2.js'),
        loadWasmBytes(),
      ]);
      return JBig2({
        // The vendored glue is built for the web and only fetches its wasm from
        // a URL; handing it the bytes works in a worker, a page and under node.
        //
        // Emscripten is not consistent about the shape it passes here: some
        // builds hand over `{a: wasmImports}`, others the bare wasmImports.
        // Passing the wrong one through produces
        //   LinkError: import function a:h must be callable
        // on engines that validate imports eagerly (WebKit / Chrome on iOS),
        // so normalise and retry rather than guessing.
        instantiateWasm(imports, success, failure) {
          const shapes = [imports, { a: imports }];
          let i = 0;
          const attempt = () => {
            WebAssembly.instantiate(wasm, shapes[i]).then(({ instance }) => {
              success(instance);
            }).catch((err) => {
              if (++i < shapes.length) attempt();
              else if (failure) failure(err);
            });
          };
          attempt();
          return {};
        },
        print: () => {},
        printErr: () => {},
      });
    })();
    modulePromise.catch(() => { modulePromise = null; });
  }
  return modulePromise;
}

/**
 * Decode one JBIG2 image.
 * @param {Uint8Array} data      the /JBIG2Decode stream (still encoded)
 * @param {number} width
 * @param {number} height
 * @param {Uint8Array|null} globals  the /JBIG2Globals stream, if any
 * @returns {Promise<{ data: Uint8Array, stride: number }>} 1 bpc, 1 = white
 */
export async function decodeJbig2(data, width, height, globals = null) {
  const module = await getModule();
  const w = width | 0;
  const h = height | 0;
  if (!w || !h) throw new Error('wasm-pdf-reader: JBIG2 image without dimensions');

  const dataPtr = module._malloc(data.length);
  const globalsPtr = globals && globals.length ? module._malloc(globals.length) : 0;
  try {
    module.writeArrayToMemory(data, dataPtr);
    if (globalsPtr) module.writeArrayToMemory(globals, globalsPtr);
    module.imageData = null;
    module._jbig2_decode(dataPtr, data.length, w, h, globalsPtr, globals ? globals.length : 0);
    const out = module.imageData;
    module.imageData = null;
    if (!out) throw new Error('wasm-pdf-reader: JBIG2 decode failed');
    const stride = (w + 7) >> 3;
    // The decoder may hand back 32-bit pitched rows; normalise to byte packed.
    if (out.length === stride * h) return { data: out, stride };
    const pitch32 = (((w + 31) >> 5) << 2);
    const packed = new Uint8Array(stride * h);
    for (let y = 0; y < h; y++) {
      packed.set(out.subarray(y * pitch32, y * pitch32 + stride), y * stride);
    }
    return { data: packed, stride };
  } finally {
    module._free(dataPtr);
    if (globalsPtr) module._free(globalsPtr);
  }
}
