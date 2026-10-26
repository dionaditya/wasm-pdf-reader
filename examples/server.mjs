#!/usr/bin/env node
// server.mjs — the demo host: static files from the repository root.
//
//   node examples/server.mjs [port]
//
// The examples use absolute paths (/dist/..., /test/...) so they must be served
// from the root, which is what this does. No caching, so a rebuild shows up on
// reload.
//
// Byte ranges are supported (`Range: bytes=a-b` -> 206 with Content-Range),
// because that is what ranged loading asks for and a demo server that silently
// returned 200 would hide the win. `--no-range` disables it, which is how
// test/ranged.mjs proves the "server ignores Range" fallback still works.

import { createServer } from 'node:http';
import { createReadStream } from 'node:fs';
import { readFile, stat } from 'node:fs/promises';
import { extname, join, normalize, sep } from 'node:path';
import { dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const argRootIdx = process.argv.indexOf('--root');
const root = argRootIdx > 0
  ? process.argv[argRootIdx + 1]
  : join(dirname(fileURLToPath(import.meta.url)), '..');
const port = Number(process.argv[2] || process.env.PORT || 5177);
const ranges = !process.argv.includes('--no-range');

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

/** `bytes=a-b` / `bytes=a-` / `bytes=-n` -> {start, end} or null. */
function parseRange(header, size) {
  if (!header || typeof header !== 'string') return null;
  const m = /^bytes=(\d*)-(\d*)$/.exec(header.trim());
  if (!m) return null;
  const [, a, b] = m;
  if (a === '' && b === '') return null;
  if (a === '') {                       // suffix range: last N bytes
    const n = Number(b);
    if (!n) return null;
    return { start: Math.max(0, size - n), end: size - 1 };
  }
  const start = Number(a);
  const end = b === '' ? size - 1 : Number(b);
  if (!Number.isFinite(start) || !Number.isFinite(end) || end < start) return null;
  return { start, end };
}

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
    const type = types[extname(file)] || 'application/octet-stream';
    const range = ranges ? parseRange(req.headers.range, info.size) : null;
    if (range) {
      // 416 for an unsatisfiable range, as the spec requires.
      if (range.start >= info.size) {
        res.writeHead(416, { 'content-range': `bytes */${info.size}` }).end();
        return;
      }
      const end = Math.min(range.end, info.size - 1);
      res.writeHead(206, {
        'content-type': type,
        'cache-control': 'no-store',
        'accept-ranges': 'bytes',
        'content-range': `bytes ${range.start}-${end}/${info.size}`,
        'content-length': end - range.start + 1,
      });
      if (req.method === 'HEAD') { res.end(); return; }
      createReadStream(file, { start: range.start, end }).pipe(res);
      return;
    }
    if (req.method === 'HEAD') {
      res.writeHead(200, {
        'content-type': type, 'accept-ranges': 'bytes',
        'content-length': info.size, 'cache-control': 'no-store',
      }).end();
      return;
    }
    const body = await readFile(file);
    res.writeHead(200, {
      'content-type': type,
      'cache-control': 'no-store',
      'accept-ranges': 'bytes',
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
