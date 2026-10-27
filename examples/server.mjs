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
import { buildTail, readSlice } from '../scripts/bigpdf.mjs';

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
    // /bigfile/generate?size=…&pages=… — a synthetic PDF of any size, answered
    // from three virtual segments (header, a hole of zeros, the object tail) so
    // a 4 GiB URL costs no disk and no memory. The big-file lab page opens it
    // with ranged loading. See scripts/bigpdf.mjs.
    if (url.pathname === '/bigfile/generate') {
      const total = Math.floor(Number(url.searchParams.get('size')) || 0);
      const pages = Math.min(Math.max(Number(url.searchParams.get('pages')) || 12, 1), 200);
      if (!(total > 64 * 1024) || total > 16 * 2 ** 30) {
        res.writeHead(400, { 'content-type': 'text/plain' })
          .end('size must be between 64 KB and 16 GiB');
        return;
      }
      const sizeText = `${(total / 2 ** 30).toFixed(2)} GiB`;
      const { header, tailStart, tail } = buildTail(total, pages, sizeText);
      const range = ranges ? parseRange(req.headers.range, total) : null;
      const base = {
        'content-type': 'application/pdf',
        'accept-ranges': 'bytes',
        'cache-control': 'no-store',
      };
      if (req.method === 'HEAD') {
        res.writeHead(range ? 206 : 200, { ...base, 'content-length': total }).end();
        return;
      }
      if (range) {
        if (range.start >= total) {
          res.writeHead(416, { 'content-range': `bytes */${total}` }).end();
          return;
        }
        const end = Math.min(range.end, total - 1);
        res.writeHead(206, {
          ...base,
          'content-range': `bytes ${range.start}-${end}/${total}`,
          'content-length': end - range.start + 1,
        });
        let at = range.start;
        const CH = 4 << 20;
        const pump = () => {
          while (at <= end) {
            const stop = Math.min(end, at + CH - 1);
            const buf = readSlice(header, tailStart, tail, at, stop);
            at = stop + 1;
            if (!res.write(buf)) { res.once('drain', pump); return; }
          }
          res.end();
        };
        pump();
        return;
      }
      // No Range: the whole file. Correct, just expensive — that is the point
      // the page's "server ignores Range" note makes.
      res.writeHead(200, { ...base, 'content-length': total });
      res.write(header);
      let written = header.length;
      const zeros = Buffer.alloc(1 << 20);
      const flush = () => {
        while (written < tailStart) {
          const n = Math.min(zeros.length, tailStart - written);
          written += n;
          if (!res.write(n === zeros.length ? zeros : zeros.subarray(0, n))) {
            res.once('drain', flush);
            return;
          }
        }
        res.end(tail);
      };
      flush();
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
