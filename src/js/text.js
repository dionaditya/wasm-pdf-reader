// text.js — turn the glyph-run layer into lines, plain text and search hits.
//
// The reader hands back one entry per glyph run, in content-stream order, with
// boxes in page space (points, y up, origin at the crop box corner). Everything
// here is pure: given the same items you get the same text, so both the vanilla
// controller and the React components use it.

/**
 * @typedef {Object} TextItem
 * @property {string} text
 * @property {number} x0 @property {number} y0  ink box, lower-left
 * @property {number} x1 @property {number} y1  ink box, upper-right
 * @property {number} ox @property {number} oy  baseline origin of the run
 * @property {number} adv  advance width in points
 * @property {number} fs   font size in points
 */

/**
 * Group glyph runs into visual lines, top to bottom.
 * @param {TextItem[]} items
 * @param {Object} [options]
 * @param {number} [options.threshold=0.5] baseline distance, in em, that starts a new line
 * @returns {{text: string, items: TextItem[], x0: number, y0: number, x1: number, y1: number, size: number}[]}
 */
export function groupLines(items, options = {}) {
  const threshold = options.threshold ?? 0.5;
  const sorted = [...items].sort((a, b) => (b.oy - a.oy) || (a.ox - b.ox));
  const lines = [];

  for (const it of sorted) {
    const size = it.fs > 0.1 ? it.fs : 10;
    const last = lines[lines.length - 1];
    if (last && Math.abs(last.oy - it.oy) <= size * threshold) {
      last.items.push(it);
      last.oy = (last.oy * (last.items.length - 1) + it.oy) / last.items.length;
    } else {
      lines.push({ oy: it.oy, items: [it] });
    }
  }

  return lines.map((line) => {
    const items = line.items.sort((a, b) => a.ox - b.ox);
    let text = '';
    let prev = null;
    for (const it of items) {
      if (prev) {
        const fs = prev.fs > 0.1 ? prev.fs : 10;
        const gap = it.ox - (prev.ox + prev.adv);
        if (gap > fs * 0.14 && !text.endsWith(' ')) text += ' ';
      }
      text += it.text;
      prev = it;
    }
    const x0 = Math.min(...items.map((i) => i.x0));
    const y0 = Math.min(...items.map((i) => i.y0));
    const x1 = Math.max(...items.map((i) => i.x1));
    const y1 = Math.max(...items.map((i) => i.y1));
    const size = Math.max(...items.map((i) => i.fs || 0));
    return { text, items, oy: line.oy, x0, y0, x1, y1, size };
  });
}

/**
 * Plain text for a page: lines joined by newlines.
 * @param {TextItem[]} items
 * @param {{lineSeparator?: string}} [options]
 * @returns {string}
 */
export function itemsToText(items, options = {}) {
  const sep = options.lineSeparator ?? '\n';
  return groupLines(items).map((l) => l.text).join(sep);
}

/**
 * Case-insensitive (by default) search across one page's items.
 *
 * Returns matches with the rectangles needed to highlight them: the box of each
 * line that contains part of the match, in page space.
 *
 * @param {TextItem[]} items
 * @param {string} query
 * @param {{caseSensitive?: boolean, limit?: number}} [options]
 * @returns {{page: number|undefined, index: number, length: number, line: string, rects: {x0:number,y0:number,x1:number,y1:number}[]}[]}
 */
export function searchItems(items, query, options = {}) {
  const limit = options.limit ?? 1000;
  if (!query) return [];
  const needle = options.caseSensitive ? query : query.toLowerCase();
  const hits = [];

  for (const line of groupLines(items)) {
    const hay = options.caseSensitive ? line.text : line.text.toLowerCase();
    let from = 0;
    for (;;) {
      const at = hay.indexOf(needle, from);
      if (at < 0 || hits.length >= limit) break;
      hits.push({
        index: at,
        length: query.length,
        line: line.text,
        rects: [{ x0: line.x0, y0: line.y0, x1: line.x1, y1: line.y1 }],
      });
      from = at + Math.max(1, needle.length);
    }
  }
  return hits;
}

/**
 * Convert a page-space box to CSS pixels inside a rendered page.
 * Only meaningful for unrotated pages; rotation-aware overlays are left to the
 * caller (the React component hides its highlight layer for rotated pages).
 * @param {{x0:number,y0:number,x1:number,y1:number}} box
 * @param {{height:number, boxY0:number, scale:number}} page
 */
export function boxToCss(box, page) {
  const s = page.scale;
  const left = (box.x0 - (page.boxX0 ?? 0)) * s;
  const top = (page.height - (box.y1 - (page.boxY0 ?? 0))) * s;
  return {
    left,
    top,
    width: (box.x1 - box.x0) * s,
    height: (box.y1 - box.y0) * s,
  };
}
