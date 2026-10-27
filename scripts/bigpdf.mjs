// bigpdf.mjs — synthesises a valid PDF of any size (2–4 GiB) without writing it.
//
// The 4 GiB question is about offsets, not about holding bytes, so the file only
// needs to be *big*: a header at offset 0, every object in the last few KB, and
// a hole in between. Streaming that hole as zeros costs no disk and no memory,
// and because the reader fetches ranges it never pulls more than a few hundred
// KB anyway.
//
// Used by examples/server.mjs (`/bigfile/generate?size=…`) for the big-file lab
// page. HTTP ranges are answered out of the three virtual segments, so a 4 GiB
// URL behaves like a 4 GiB file on disk.

const HEADER = Buffer.from('%PDF-1.7\n%\xe2\xe3\xcf\xd3\n', 'binary');

function contentStream(pageNo, pageCount, sizeText) {
  const text = `Big-file lab - page ${pageNo} of ${pageCount} (${sizeText})`;
  const body = `BT /F1 22 Tf 72 720 Td (${text.replace(/[()\\]/g, '')}) Tj ET\n`
    + '0.1 0.1 0.1 rg 72 700 468 2 re f\n'
    + `BT /F1 12 Tf 72 680 Td (Opened from byte ranges - the file is not in memory) Tj ET\n`;
  return body;
}

/**
 * Build the tail (all objects + xref + trailer) for a file of `size` bytes.
 * Every xref entry is an absolute offset near the end, i.e. above 2^32 - 1 MiB
 * once the size is above ~2 GiB — which is the point.
 */
export function buildTail(size, pageCount = 12, sizeText = '') {
  const n = Math.max(1, pageCount | 0);
  const objs = new Map();
  objs.set(1, '<</Type/Catalog/Pages 2 0 R>>');
  objs.set(2, `<</Type/Pages/Kids[${Array.from({ length: n }, (_, i) => `${4 + i * 2} 0 R`).join(' ')}]/Count ${n}>>`);
  objs.set(3, '<</Type/Font/Subtype/Type1/BaseFont/Helvetica/Encoding/WinAnsiEncoding>>');
  for (let i = 0; i < n; i++) {
    const pageNum = 4 + i * 2, contentNum = pageNum + 1;
    const body = contentStream(i + 1, n, sizeText || `${(size / 2 ** 30).toFixed(2)} GiB`);
    objs.set(pageNum, `<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]`
      + `/Resources<</Font<</F1 3 0 R>>>>/Contents ${contentNum} 0 R>>`);
    objs.set(contentNum, `STREAM:${body}`);
  }

  const maxNum = 3 + n * 2;
  const write = (tailStart) => {
    const parts = [];
    const offsets = new Map();
    let pos = tailStart;
    for (let num = 1; num <= maxNum; num++) {
      const body = objs.get(num);
      offsets.set(num, pos);
      let chunk;
      if (body.startsWith('STREAM:')) {
        const data = Buffer.from(body.slice(7), 'binary');
        chunk = Buffer.concat([Buffer.from(`${num} 0 obj\n<</Length ${data.length}>>\nstream\n`), data,
          Buffer.from('endstream\nendobj\n')]);
      } else {
        chunk = Buffer.from(`${num} 0 obj\n${body}\nendobj\n`);
      }
      parts.push({ start: pos, buf: chunk });
      pos += chunk.length;
    }
    const xrefAt = pos;
    const lines = [`xref`, `0 ${maxNum + 1}`, '0000000000 65535 f '];
    for (let num = 1; num <= maxNum; num++) {
      lines.push(`${String(offsets.get(num)).padStart(10, '0')} 00000 n `);
    }
    lines.push('trailer', `<</Size ${maxNum + 1}/Root 1 0 R>>`, 'startxref', String(xrefAt), '%%EOF', '');
    const tail = Buffer.concat([...parts.map((p) => p.buf), Buffer.from(lines.join('\n'))]);
    return { tail, tailStart };
  };

  // The tail's length depends on the number of digits in the offsets, so
  // converge on a start that lands the tail exactly at EOF.
  let tailLen = 0;
  let tail = Buffer.alloc(0);
  let tailStart = size;
  for (let i = 0; i < 6; i++) {
    tailStart = size - tailLen;
    const out = write(tailStart);
    tail = out.tail;
    if (out.tail.length === tailLen) break;
    tailLen = out.tail.length;
  }
  return { header: HEADER, tailStart, tail: write(tailStart).tail, pageCount: n };
}

/** Total size of the virtual file for a requested size. */
export function pdfSize(size, pageCount) {
  const { tail, header } = buildTail(size, pageCount);
  if (tail.length + header.length >= size - 1024) return null;   // too small to lay out
  return size;
}

/**
 * Read `[start, end]` (inclusive) out of the virtual file. Returns a Buffer.
 * Bounded by the caller to a sane range size; this is for HTTP `Range:` replies.
 */
export function readSlice(header, tailStart, tail, start, end) {
  const chunks = [];
  const headerEnd = header.length;
  const overlap = (segStart, buf) => {
    const s = Math.max(start, segStart), e = Math.min(end, segStart + buf.length - 1);
    if (e >= s) chunks.push(buf.subarray(s - segStart, e - segStart + 1));
  };
  overlap(0, header);
  const zs = Math.max(start, headerEnd), ze = Math.min(end, tailStart - 1);
  if (ze >= zs) chunks.push(Buffer.alloc(ze - zs + 1));           // the hole
  overlap(tailStart, tail);
  return Buffer.concat(chunks);
}
