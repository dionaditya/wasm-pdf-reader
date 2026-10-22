#!/usr/bin/env node
// build-js.mjs — assemble dist/ from src/.
//
// No bundler: the package ships plain ES modules (one file per concern) so
// bundlers can tree-shake, and the wasm glue stays a sibling of the .wasm it
// loads. The only transform is JSX -> JS for the React layer.

import { build } from 'esbuild';
import { cp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const src = join(root, 'src');
const dist = join(root, 'dist');

const coreFiles = ['index.js', 'document.js', 'jbig2.js', 'loader.js', 'mount.js', 'text.js'];
const reactFiles = ['index.js', 'PdfReader.jsx', 'PdfPage.jsx', 'PdfToolbar.jsx', 'hooks.js'];

// 1. clean the JS output only: dist/wasm comes from emcc
for (const file of coreFiles) await rm(join(dist, file), { force: true });
await rm(join(dist, 'react'), { recursive: true, force: true });
await mkdir(join(dist, 'react'), { recursive: true });

// 2. copy the core modules verbatim
for (const file of coreFiles) {
  await cp(join(src, 'js', file), join(dist, file));
}

// 3. JSX -> JS, imports left intact
await build({
  entryPoints: reactFiles.map((f) => join(src, 'js', 'react', f)),
  outdir: join(dist, 'react'),
  outbase: join(src, 'js', 'react'),
  format: 'esm',
  platform: 'neutral',
  jsx: 'automatic',
  target: ['es2020'],
  logLevel: 'warning',
});

// 4. the emitted modules import each other by their source names; point them at
//    the compiled files instead
for (const file of ['index.js', 'PdfReader.js', 'PdfPage.js', 'PdfToolbar.js', 'hooks.js']) {
  const path = join(dist, 'react', file);
  const code = await readFile(path, 'utf8');
  if (code.includes('.jsx"')) {
    await writeFile(path, code.replace(/(from\s+["'][^"']+)\.jsx(["'])/g, '$1.js$2'));
  }
}

// 5. stylesheet + type declarations
await cp(join(src, 'styles.css'), join(dist, 'styles.css'));
for (const [from, to] of [
  ['types/index.d.ts', 'index.d.ts'],
  ['types/react.d.ts', 'react/index.d.ts'],
]) {
  if (existsSync(join(src, from))) await cp(join(src, from), join(dist, to));
}

// 5b. JBIG2 decoder assets (pdf.js.jbig2, Apache-2.0; see jbig2/LICENSE_*).
//     Copied verbatim: the glue resolves jbig2.wasm next to itself.
await cp(join(src, 'js', 'jbig2'), join(dist, 'jbig2'), { recursive: true });
// also under wasm/ for pages that copy only that directory
await cp(join(src, 'js', 'jbig2'), join(dist, 'wasm', 'jbig2'), { recursive: true });

// 6. fail loudly if the wasm module was never built
if (!existsSync(join(dist, 'wasm', 'pdf-reader.wasm'))) {
  console.warn('warning: dist/wasm/pdf-reader.wasm is missing — run ./build.sh');
}

const pkg = JSON.parse(await readFile(join(root, 'package.json'), 'utf8'));
const entry = await readFile(join(dist, 'index.js'), 'utf8');
if (!entry.includes(`version = '${pkg.version}'`)) {
  await writeFile(join(dist, 'index.js'), entry.replace(/version = '[^']+'/, `version = '${pkg.version}'`));
  console.warn(`note: version in dist/index.js updated to ${pkg.version}`);
}

console.log('dist ready:', ['index.js', 'react/', 'styles.css', 'wasm/'].join(', '));
