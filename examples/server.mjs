#!/usr/bin/env node
// server.mjs — the demo host: static files from the repository root.
//
//   node examples/server.mjs [port]
//
// The examples use absolute paths (/dist/..., /test/...) so they must be served
// from the root, which is what this does. No caching, so a rebuild shows up on
// reload.

import { createServer } from 'node:http';
import { readFile, stat } from 'node:fs/promises';
import { extname, join, normalize, sep } from 'node:path';
import { dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const argRootIdx = process.argv.indexOf('--root');
const root = argRootIdx > 0
  ? process.argv[argRootIdx + 1]
  : join(dirname(fileURLToPath(import.meta.url)), '..');
const port = Number(process.argv[2] || process.env.PORT || 5177);

const types = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.wasm': 'application/wasm',
  '.pdf': 'application/pdf',
  '.png': 'image/png',
  '.svg': 'image/svg+xml',
  '.map': 'application/json',
};

const server = createServer(async (req, res) => {
  try {
    const url = new URL(req.url, 'http://localhost');
    let path = normalize(decodeURIComponent(url.pathname)).replace(/^(\.\.[/\\])+/, '');
    if (path.endsWith('/')) path += 'index.html';
    let file = join(root, path);
    if (!file.startsWith(root + sep) && file !== root) {
      res.writeHead(403).end('forbidden');
      return;
    }
    let info = await stat(file).catch(() => null);
    if (info?.isDirectory()) {
      file = join(file, 'index.html');
      info = await stat(file).catch(() => null);
    }
    if (!info) {
      res.writeHead(404, { 'content-type': 'text/plain' }).end('not found');
      return;
    }
    const body = await readFile(file);
    res.writeHead(200, {
      'content-type': types[extname(file)] || 'application/octet-stream',
      'cache-control': 'no-store',
      'content-length': body.length,
    });
    res.end(body);
  } catch (err) {
    res.writeHead(500, { 'content-type': 'text/plain' }).end(String(err));
  }
});

server.listen(port, () => {
  console.log(`wasm-pdf-reader demos on http://localhost:${port}/`);
  console.log(`  vanilla : http://localhost:${port}/examples/vanilla/`);
  console.log(`  react   : http://localhost:${port}/examples/react/`);
});
