#!/usr/bin/env node
// build-react.mjs — bundle the React example (development demo only).
import { build } from 'esbuild';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');

await build({
  entryPoints: [join(root, 'examples/react/main.jsx')],
  outfile: join(root, 'examples/react/bundle.js'),
  bundle: true,
  format: 'esm',
  jsx: 'automatic',
  target: ['es2020'],
  sourcemap: false,
  minify: true,
  external: ['node:module', 'node:fs', 'node:path', 'node:url'],
  logLevel: 'info',
});
