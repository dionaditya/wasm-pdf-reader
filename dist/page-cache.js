// page-cache.js — a byte-budgeted LRU of rendered page bitmaps (M9.3).
//
// Why: scrolling a 200-page document renders each page as it comes into view,
// and the wasm module owns a single bitmap, so every re-render is a full
// interpretation of the content stream. The viewer throws those pixels away the
// moment the page leaves the viewport, which makes scrolling back and zooming
// out cost as much as the first paint.
//
// What this is not: a display list. The rasterizer still interprets the content
// stream per page; this only avoids repeating that work for a page at a scale
// already rendered. A *new* scale is still a full render — see ROADMAP M9.3 for
// the record/replay half that would fix it.
//
// Budget: bytes of RGBA, not entries, because a fit-width A4 page at dpr 2 is
// 8 MB and "cache 10 pages" can mean 80 MB or 8 MB depending on the page.
// Default comes from `navigator.deviceMemory` (heavily capped: this is a helper,
// not the document), and can be set with `pageCacheBytes` or disabled with
// `pageCache: false`.

export const DEFAULT_BUDGET = 48 * 1024 * 1024;

function defaultBudget() {
  if (typeof navigator === 'undefined') return DEFAULT_BUDGET;
  const gb = Number(navigator.deviceMemory);
  if (!Number.isFinite(gb) || gb <= 0) return DEFAULT_BUDGET;
  // a quarter of device RAM, clamped: phones keep ~8 MB, desktops cap at 96 MB
  return Math.max(8 * 1024 * 1024, Math.min(96 * 1024 * 1024, (gb * 1024 * 1024 * 1024) / 4));
}

export class PageCache {
  /**
   * @param {{budgetBytes?: number, enabled?: boolean}} [options]
   */
  constructor(options = {}) {
    this.budget = Math.max(0, Number(options.budgetBytes) || defaultBudget());
    if (options.budgetBytes === 0) this.budget = 0;
    this.enabled = options.enabled !== false && this.budget > 0;
    this.map = new Map();          // key -> { result, bytes, width, height }
    this.bytes = 0;
    this.hits = 0;
    this.misses = 0;
    this.evictions = 0;
    this.pinned = null;
  }

  static key(index, scale) {
    // 0.01 precision: the same scale computed two ways (fit-width arithmetic)
    // must hit, and a 1 % scale change is not worth a separate bitmap.
    return `${index}@${Math.round(scale * 100) / 100}`;
  }

  get(index, scale) {
    if (!this.enabled) return null;
    const key = PageCache.key(index, scale);
    const hit = this.map.get(key);
    if (!hit) { this.misses++; return null; }
    // refresh LRU position
    this.map.delete(key);
    this.map.set(key, hit);
    this.hits++;
    return hit.result;
  }

  /**
   * @param {number} index
   * @param {number} scale
   * @param {Object} result RenderResult to keep (imageData is retained, not copied)
   * @param {string} [pinKey] a key that must survive eviction (the visible page)
   */
  set(index, scale, result, pinKey) {
    if (!this.enabled) return;
    const key = PageCache.key(index, scale);
    const bytes = result.width * result.height * 4;
    if (bytes > this.budget) { this.misses++; return; }   // bigger than the whole cache
    const existing = this.map.get(key);
    if (existing) {
      this.bytes -= existing.bytes;
      this.map.delete(key);
    }
    this.map.set(key, { result, bytes });
    this.bytes += bytes;
    if (pinKey) this.pinned = pinKey;
    this.evict();
  }

  evict() {
    while (this.bytes > this.budget && this.map.size > 1) {
      // oldest keys are at the front of the Map; never evict the pinned page
      let victim = null;
      for (const k of this.map.keys()) {
        if (k !== this.pinned) { victim = k; break; }
      }
      if (!victim) break;
      const entry = this.map.get(victim);
      this.map.delete(victim);
      this.bytes -= entry.bytes;
      this.evictions++;
    }
  }

  /** Prefer keeping the page the viewer is showing. */
  pin(index, scale) {
    this.pinned = PageCache.key(index, scale);
  }

  clear() {
    this.map.clear();
    this.bytes = 0;
    this.pinned = null;
  }

  stats() {
    return {
      entries: this.map.size,
      bytes: this.bytes,
      budget: this.budget,
      hits: this.hits,
      misses: this.misses,
      evictions: this.evictions,
    };
  }
}

export default PageCache;
