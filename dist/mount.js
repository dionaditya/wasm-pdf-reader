// mount.js — the framework-free viewer: `mountPdfReader(element, options)`.
//
// Everything is built with DOM APIs (no innerHTML, no template strings), so it
// is safe with a strict CSP and with untrusted file names. The returned
// controller is the imperative API; the React components are a thin layer over
// the same PdfDocument class.

import { PdfDocument } from './document.js';
import { buildTextLayer } from './text.js';
import { turnedPageBox, normaliseRotation, stepRotation } from './rotation.js';

const ICONS = {
  prev: '‹',
  next: '›',
  zoomOut: '−',
  zoomIn: '+',
  fitWidth: '⇔',
  fitPage: '⤢',
  rotateLeft: '⟲',
  rotateRight: '⟳',
  thumbnails: '▤',
  print: '⎙',
};

/** @typedef {Object} ViewerLabels */
export const defaultLabels = {
  previousPage: 'Previous page',
  nextPage: 'Next page',
  zoomIn: 'Zoom in',
  zoomOut: 'Zoom out',
  fitWidth: 'Fit width',
  fitPage: 'Fit page',
  rotateLeft: 'Rotate counter-clockwise',
  rotateRight: 'Rotate clockwise',
  thumbnails: 'Thumbnails',
  print: 'Print',
  page: 'Page',
  of: 'of',
  loading: 'Loading document…',
  rendering: 'Rendering…',
  error: 'Could not open this PDF',
  empty: 'Drop a PDF here, or call setFile()',
};

/** Map a theme object ({bg, accent, radius, ...}) onto CSS custom properties. */
export function applyTheme(root, theme) {
  if (!theme) return;
  for (const [key, value] of Object.entries(theme)) {
    if (value == null) continue;
    const name = key.replace(/[A-Z]/g, (m) => `-${m.toLowerCase()}`);
    root.style.setProperty(`--wpdf-${name}`, String(value));
  }
}

function el(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text != null) node.textContent = text;
  return node;
}

function button(className, label, text) {
  const b = el('button', className, text);
  b.type = 'button';
  b.title = label;
  b.setAttribute('aria-label', label);
  return b;
}

/**
 * Mount a complete viewer (toolbar + viewport + status bar) into `target`.
 *
 * @param {HTMLElement|string} target element or selector
 * @param {Object} [options]
 * @param {ArrayBuffer|Uint8Array|Blob|File|string|URL} [options.file]
 * @param {number} [options.page=0]   zero-based page to show
 * @param {number} [options.scale=1.5] CSS pixels per point
 * @param {'none'|'width'|'page'|'auto'} [options.fit='auto']  'auto' fits width on first open
 * @param {0|90|180|270} [options.rotation=0] view rotation added on top of each page's /Rotate
 * @param {(rotation:number)=>void} [options.onRotationChange]
 * @param {boolean} [options.continuous=false] continuous scroll instead of one page
 * @param {boolean} [options.toolbar=true]  build the default toolbar
 * @param {HTMLElement|Function} [options.toolbarSlot] custom toolbar (node, or fn(state) -> node)
 * @param {HTMLElement} [options.statusSlot] custom status bar content
 * @param {boolean} [options.status=true]
 * @param {Object} [options.theme] CSS variable overrides
 * @param {ViewerLabels} [options.labels]
 * @param {Function} [options.pixelTransform] passed to PdfDocument.render
 * @param {boolean|'strict'} [options.worker] parse and rasterize in a Web Worker
 * @param {string} [options.workerUrl] URL of dist/worker.js (bundled hosts must set it)
 * @param {Function} [options.workerFactory] (options) => Worker, for custom worker builds
 * @param {boolean} [options.invert=false] dark-mode page filter
 * @param {Function} [options.onReady]
 * @param {Function} [options.onPageChange]
 * @param {Function} [options.onScaleChange]
 * @param {Function} [options.onError]
 * @param {Function} [options.renderError]  (error) => Node, replaces built-in message
 * @returns {Object} controller
 */
export function mountPdfReader(target, options = {}) {
  const host = typeof target === 'string' ? document.querySelector(target) : target;
  if (!host) throw new Error('wasm-pdf-reader: target element not found');

  const opts = {
    page: 0,
    scale: 1.5,
    fit: 'auto',
    rotation: 0,
    continuous: false,
    toolbar: true,
    status: true,
    labels: defaultLabels,
    ...options,
  };
  const labels = { ...defaultLabels, ...(options.labels || {}) };

  const root = el('div', 'wpdf-root');
  applyTheme(root, opts.theme);
  if (opts.invert) root.classList.add('wpdf-root--invert');
  host.appendChild(root);

  // ── toolbar ────────────────────────────────────────────────────────────────
  let toolbarEl = null;
  let pageInput = null;
  let pageCountEl = null;
  let zoomEl = null;
  let prevBtn = null;
  let nextBtn = null;
  let rotateLeftBtn = null;
  let rotateRightBtn = null;
  let thumbsBtn = null;
  let printBtn = null;

  if (opts.toolbar) {
    toolbarEl = el('div', 'wpdf-toolbar');

    const title = el('span', 'wpdf-toolbar__title', '');
    title.hidden = true;
    toolbarEl.appendChild(title);

    const group = el('div', 'wpdf-toolbar__group');
    prevBtn = button('wpdf-button', labels.previousPage, ICONS.prev);
    nextBtn = button('wpdf-button', labels.nextPage, ICONS.next);
    pageInput = el('input', 'wpdf-input');
    pageInput.type = 'text';
    pageInput.inputMode = 'numeric';
    pageInput.setAttribute('aria-label', labels.page);
    pageCountEl = el('span', 'wpdf-toolbar__count', '—');
    group.append(prevBtn, pageInput, pageCountEl, nextBtn);
    toolbarEl.appendChild(group);

    toolbarEl.appendChild(el('span', 'wpdf-toolbar__spacer'));

    const zoomGroup = el('div', 'wpdf-toolbar__group');
    const zoomOut = button('wpdf-button', labels.zoomOut, ICONS.zoomOut);
    const zoomIn = button('wpdf-button', labels.zoomIn, ICONS.zoomIn);
    zoomEl = el('span', 'wpdf-toolbar__zoom', '');
    const fitWidth = button('wpdf-button', labels.fitWidth, ICONS.fitWidth);
    const fitPage = button('wpdf-button', labels.fitPage, ICONS.fitPage);
    zoomGroup.append(zoomOut, zoomEl, zoomIn, fitWidth, fitPage);
    toolbarEl.appendChild(zoomGroup);

    // Turning the view is not editing the page: the rotation stays on the reader
    // and is applied when the next page is drawn, so a document's own /Rotate is
    // never rewritten.
    const rotateGroup = el('div', 'wpdf-toolbar__group');
    rotateLeftBtn = button('wpdf-button', labels.rotateLeft, ICONS.rotateLeft);
    rotateRightBtn = button('wpdf-button', labels.rotateRight, ICONS.rotateRight);
    rotateGroup.append(rotateLeftBtn, rotateRightBtn);
    toolbarEl.appendChild(rotateGroup);

    const viewGroup = el('div', 'wpdf-toolbar__group');
    thumbsBtn = button('wpdf-button', labels.thumbnails, ICONS.thumbnails);
    thumbsBtn.setAttribute('aria-pressed', 'false');
    printBtn = button('wpdf-button', labels.print, ICONS.print);
    viewGroup.append(thumbsBtn, printBtn);
    toolbarEl.appendChild(viewGroup);

    root.appendChild(toolbarEl);
  } else if (opts.toolbarSlot) {
    const custom = typeof opts.toolbarSlot === 'function'
      ? opts.toolbarSlot({ getState: () => publicState() })
      : opts.toolbarSlot;
    if (custom) root.appendChild(custom);
  }

  // ── viewport + status ─────────────────────────────────────────────────────
  const body = el('div', 'wpdf-body');
  const thumbsEl = el('aside', 'wpdf-thumbs');
  thumbsEl.hidden = true;
  thumbsEl.setAttribute('aria-label', labels.thumbnails);
  const viewport = el('div', `wpdf-viewport${opts.continuous ? ' wpdf-viewport--continuous' : ''}`);
  body.append(thumbsEl, viewport);
  root.appendChild(body);

  const statusEl = opts.status ? el('div', 'wpdf-status') : null;
  if (statusEl) {
    root.appendChild(statusEl);
    if (opts.statusSlot) statusEl.appendChild(opts.statusSlot);
  }

  const overlay = el('div', 'wpdf-overlay');
  overlay.hidden = true;
  root.appendChild(overlay);

  // ── state ─────────────────────────────────────────────────────────────────
  const state = {
    doc: null,
    page: Math.max(0, opts.page | 0),
    scale: opts.scale,
    fit: opts.fit,
    rotation: normaliseRotation(opts.rotation),
    status: 'idle',       // idle | loading | ready | error
    error: null,
    renderMs: 0,
    pageEls: [],
    canvases: [],
  };
  let renderToken = 0;
  let destroyed = false;
  let observer = null;

  function publicState() {
    const count = state.doc ? state.doc.pageCount : 0;
    return {
      page: state.page,
      pageCount: count,
      scale: state.scale,
      fit: state.fit,
      rotation: state.rotation,
      status: state.status,
      error: state.error,
      filename: state.doc?.filename || '',
      metadata: state.doc?.metadata || {},
    };
  }

  function setOverlay(node) {
    overlay.replaceChildren();
    overlay.hidden = !node;
    if (node) overlay.appendChild(node);
  }

  function messageNode(text, kind) {
    const wrap = el('div', kind === 'error' ? 'wpdf-overlay--error' : null);
    if (kind === 'loading') wrap.appendChild(el('span', 'wpdf-spinner'));
    wrap.appendChild(el('span', null, text));
    return wrap;
  }

  function updateToolbar() {
    if (!toolbarEl) return;
    const count = state.doc ? state.doc.pageCount : 0;
    if (pageInput) {
      pageInput.value = count ? String(state.page + 1) : '0';
      pageInput.disabled = !count;
    }
    if (pageCountEl) pageCountEl.textContent = `/ ${count}`;
    if (prevBtn) prevBtn.disabled = state.page <= 0;
    if (nextBtn) nextBtn.disabled = state.page >= count - 1;
    if (zoomEl) zoomEl.textContent = `${Math.round(effectiveScale() * 100)}%`;
    const title = toolbarEl.querySelector('.wpdf-toolbar__title');
    if (title) {
      const name = state.doc?.filename || state.doc?.metadata?.Title || '';
      title.textContent = name;
      title.hidden = !name;
    }
  }

  function updateStatus() {
    if (!statusEl || typeof opts.statusSlot === 'object') return;
    statusEl.replaceChildren();
    const size = state.doc && state.doc.pageCount ? state.doc.getPageSize(state.page) : null;
    const bits = [];
    if (state.doc) bits.push(`Page ${state.page + 1} of ${state.doc.pageCount}`);
    if (state.doc?.filename) bits.push(state.doc.filename);
    if (size) bits.push(`${Math.round(size.width)} × ${Math.round(size.height)} pt`);
    if (state.rotation) bits.push(`${state.rotation}°`);
    bits.push(`${Math.round(state.scale * 150)} dpi`);
    if (state.renderMs) bits.push(`${state.renderMs} ms`);
    const warns = state.doc?.warnings?.length || 0;
    if (warns) bits.push(`${warns} warning${warns === 1 ? '' : 's'}`);
    for (const b of bits) statusEl.appendChild(el('span', null, b));
  }

  function emit(name, value) {
    const fn = opts[name];
    if (typeof fn === 'function') fn(value);
  }

  /** The turned box of a page at `scale` — what the viewport has to fit. */
  function pageBox(scale, index = state.page) {
    const size = state.doc?.getPageSize(index) || { width: 612, height: 792 };
    return turnedPageBox(size, scale, state.rotation);
  }

  function pageScale() {
    const box = pageBox(1);
    const pad = 32;
    // Before the first layout pass the viewport can report 0; fall back to the
    // window so the first page is never rendered at thumbnail size.
    const vw = Math.max(120, (viewport.clientWidth || window.innerWidth) - pad);
    const vh = Math.max(120, (viewport.clientHeight || window.innerHeight - 120) - pad);
    if (state.fit === 'width') return vw / box.width;
    if (state.fit === 'page') return Math.min(vw / box.width, vh / box.height);
    return state.scale;
  }

  function effectiveScale() {
    const s = state.fit && state.fit !== 'none' ? pageScale() : state.scale;
    return Math.max(0.05, Math.min(8, s || 1));
  }

  function makePage(scale) {
    const pageEl = el('div', 'wpdf-page');
    const box = pageBox(scale);
    pageEl.style.width = `${box.width}px`;
    pageEl.style.height = `${box.height}px`;
    pageEl.appendChild(turnLayer(box, scale));
    return pageEl;
  }

  /**
   * The turning layer: canvas, text, links and highlights all live inside it, so
   * one transform keeps them registered with each other. It is positioned in
   * unrotated page space and the transform maps that box exactly onto the outer
   * (already swapped) page box.
   */
  function turnLayer(box, scale) {
    const turn = el('div', 'wpdf-page__turn');
    turn.style.width = `${box.innerWidth}px`;
    turn.style.height = `${box.innerHeight}px`;
    turn.style.transform = box.transform;
    turn.dataset.rotation = String(box.rotation);
    const canvas = el('canvas', 'wpdf-page__canvas');
    canvas.style.width = `${box.innerWidth}px`;
    canvas.style.height = `${box.innerHeight}px`;
    turn.appendChild(canvas);
    turn.dataset.scale = String(scale);
    return turn;
  }

  /** The turning layer of a page element, if it has one. */
  function turnOf(pageEl) {
    return pageEl.querySelector(':scope > .wpdf-page__turn');
  }

  async function renderSingle() {
    if (!state.doc) return;
    const token = ++renderToken;
    const scale = effectiveScale();
    viewport.replaceChildren();
    const pageEl = makePage(scale);
    viewport.appendChild(pageEl);
    updateToolbar();
    updateStatus();
    const started = performance.now();
    try {
      const out = await state.doc.renderToCanvas(pageEl.querySelector('canvas'), state.page, {
        scale,
        dpr: opts.dpr ?? window.devicePixelRatio ?? 1,
        pixelTransform: opts.pixelTransform,
      });
      if (token !== renderToken || destroyed) return;
      state.renderMs = Math.round(performance.now() - started);
      emit('onRender', { page: state.page, scale, ...out });
      const turn = turnOf(pageEl) || pageEl;
      if (opts.textLayer !== false) {
        const size = state.doc.getPageSize(state.page);
        const text = await loadText(state.page);
        if (token !== renderToken || destroyed) return;
        turn.appendChild(buildTextLayer(text, {
          scale, height: size.height, boxY0: size.boxY0,
        }));
      }
      const linkEl = await linkLayer(pageEl, state.page);
      if (linkEl) turn.appendChild(linkEl);
      // Selection bars are painted in screen space relative to the *outer* page
      // box, so they stay outside the turning layer.
      selectionLayer(pageEl);
      const hl = opts.highlights?.(state.page, scale) || [];
      if (hl.length) turn.appendChild(highlightLayer(hl, pageEl));
    } catch (err) {
      if (token === renderToken) showError(err);
    }
    updateStatus();
  }

  async function renderContinuous() {
    if (!state.doc) return;
    const token = ++renderToken;
    const scale = effectiveScale();
    viewport.replaceChildren();
    state.pageEls = [];
    state.canvases = [];
    observer?.disconnect();
    observer = new IntersectionObserver((entries) => {
      for (const entry of entries) {
        if (!entry.isIntersecting) continue;
        const index = Number(entry.target.dataset.page);
        renderInto(entry.target, index, scale, token);
      }
    // A tall fit-width page can fill more than one viewport, so a fixed pixel
    // margin left the next page unrendered until it was already on screen.
    }, { root: viewport, rootMargin: '50% 0px 50% 0px' });

    for (let i = 0; i < state.doc.pageCount; i++) {
      const box = pageBox(scale, i);
      const pageEl = el('div', 'wpdf-page');
      pageEl.dataset.page = String(i);
      pageEl.style.width = `${box.width}px`;
      pageEl.style.height = `${box.height}px`;
      const ph = el('div', 'wpdf-page__placeholder');
      ph.style.width = '100%';
      ph.style.height = '100%';
      ph.textContent = `${i + 1}`;
      pageEl.appendChild(ph);
      viewport.appendChild(pageEl);
      state.pageEls[i] = pageEl;
      observer.observe(pageEl);
    }
    // Always paint the current page and its neighbours: at fit-width a page can
    // be taller than a viewport, which puts the next one outside any sane
    // rootMargin and leaves it as a placeholder until the user scrolls.
    for (const i of [state.page - 1, state.page, state.page + 1]) {
      const node = state.pageEls[i];
      if (node) renderInto(node, i, scale, token);
    }
    updateToolbar();
  }

  async function renderInto(pageEl, index, scale, token) {
    const rotationKey = `${scale}:${state.rotation}`;
    if (pageEl.dataset.rendered === rotationKey) return;
    pageEl.dataset.rendered = rotationKey;
    let turn = turnOf(pageEl);
    if (!turn) {
      const box = pageBox(scale, index);
      pageEl.replaceChildren(turnLayer(box, scale));
      turn = turnOf(pageEl);
    }
    const canvas = turn.querySelector('canvas');
    try {
      const out = await state.doc.renderToCanvas(canvas, index, {
        scale,
        dpr: opts.dpr ?? window.devicePixelRatio ?? 1,
        pixelTransform: opts.pixelTransform,
      });
      if (token !== renderToken || destroyed) return;
      emit('onRender', { page: index, scale, ...out });
      // Continuous pages are built lazily as they scroll into view, so the
      // text and link layers have to be attached here, not once up front.
      if (opts.textLayer !== false) {
        const size = state.doc.getPageSize(index);
        const text = await loadText(index);
        if (token !== renderToken || destroyed) return;
        turn.appendChild(buildTextLayer(text, {
          scale, height: size.height, boxY0: size.boxY0,
        }));
      }
      const linkEl = await linkLayer(pageEl, index);
      if (linkEl) turn.appendChild(linkEl);
      selectionLayer(pageEl);
      const hl = opts.highlights?.(index, scale) || [];
      if (hl.length) turn.appendChild(highlightLayer(hl, pageEl));
      if (index === state.page) updateStatus();
    } catch (err) {
      pageEl.dataset.rendered = '';
      if (token === renderToken) showError(err);
    }
  }

  // ── selection highlight ──────────────────────────────────────────────────
  // The browser paints one box per text span, so a selection over per-glyph runs
  // shows ragged, overlapping rectangles. The native highlight is made
  // transparent in CSS and we paint a single rect per line instead, which is
  // what a document viewer looks like.
  const selLayers = new Map();

  function selectionLayer(pageEl) {
    let layer = selLayers.get(pageEl);
    if (!layer) {
      layer = el('div', 'wpdf-selection');
      pageEl.appendChild(layer);
      selLayers.set(pageEl, layer);
    }
    return layer;
  }

  function selectionLines(range, pageRect) {
    const groups = [];
    for (const r of range.getClientRects()) {
      if (!r.width || !r.height) continue;
      if (r.bottom < pageRect.top || r.top > pageRect.bottom) continue;
      const top = r.top - pageRect.top;
      const bottom = r.bottom - pageRect.top;
      const center = (top + bottom) / 2;
      // Cluster by vertical centre, not by top: runs on one line have different
      // box heights (caps vs descenders), and grouping on top leaves ragged
      // per-glyph strips.
      const g = groups.find((x) => Math.abs(x.center - center) <= Math.max(3, (x.bottom - x.top) * 0.5));
      if (g) {
        g.left = Math.min(g.left, r.left - pageRect.left);
        g.right = Math.max(g.right, r.right - pageRect.left);
        g.top = Math.min(g.top, top);
        g.bottom = Math.max(g.bottom, bottom);
        g.center = (g.top + g.bottom) / 2;
      } else {
        groups.push({
          left: r.left - pageRect.left,
          right: r.right - pageRect.left,
          top,
          bottom,
          center,
        });
      }
    }
    // Second pass: stitch strips that touch into one bar, so a line selected
    // across runs of different sizes is a single rectangle.
    groups.sort((a, b) => a.top - b.top);
    const merged = [];
    for (const g of groups) {
      const host = merged.find(
        (m) =>
          g.left < m.right + 2 &&
          g.right > m.left - 2 &&
          g.top < m.bottom &&
          g.bottom > m.top,
      );
      if (host) {
        host.left = Math.min(host.left, g.left);
        host.right = Math.max(host.right, g.right);
        host.top = Math.min(host.top, g.top);
        host.bottom = Math.max(host.bottom, g.bottom);
      } else {
        merged.push({ ...g });
      }
    }
    return merged;
  }

  function paintSelection() {
    const sel = window.getSelection();
    const live = [...selLayers.entries()];
    for (const [, layer] of live) layer.replaceChildren();
    if (!sel || sel.isCollapsed || !sel.rangeCount) return;
    const range = sel.getRangeAt(0);
    if (!root.contains(range.commonAncestorContainer)) return;
    for (const [pageEl, layer] of live) {
      for (const g of selectionLines(range, pageEl.getBoundingClientRect())) {
        const bar = document.createElement('i');
        bar.style.left = `${g.left}px`;
        bar.style.top = `${g.top - 1}px`;
        bar.style.width = `${Math.max(1, g.right - g.left)}px`;
        bar.style.height = `${g.bottom - g.top + 2}px`;
        layer.appendChild(bar);
      }
    }
  }

  let selFrame = 0;
  function queueSelectionPaint() {
    if (selFrame) return;
    selFrame = requestAnimationFrame(() => {
      selFrame = 0;
      paintSelection();
    });
  }
  document.addEventListener('selectionchange', queueSelectionPaint);
  viewport.addEventListener('scroll', queueSelectionPaint, { passive: true });
  window.addEventListener('resize', queueSelectionPaint);

  // Text and link data for a page. Both are cached by the render that drew it,
  // so this is usually synchronous even for a worker-backed document; the await
  // is what makes the first paint correct instead of racing the cache.
  async function loadText(index) {
    try { return await state.doc.loadText(index); } catch { return []; }
  }

  async function loadLinks(index) {
    try { return await state.doc.loadLinks(index); } catch { return []; }
  }

  // Link annotations: every /Link with a URI or an internal /GoTo becomes a
  // transparent anchor over the page bitmap.
  async function linkLayer(pageEl, pageIndex) {
    const links = await loadLinks(pageIndex);
    if (!links.length) return null;
    const scale = effectiveScale();
    const layer = el('div', 'wpdf-page__overlay');
    for (const link of links) {
      const css = state.doc.boxToCss(link, pageIndex, scale);
      const a = el('a', 'wpdf-link');
      a.style.left = `${css.left}px`;
      a.style.top = `${css.top}px`;
      a.style.width = `${css.width}px`;
      a.style.height = `${css.height}px`;
      if (link.uri) {
        a.href = link.uri;
        a.target = '_blank';
        a.rel = 'noopener noreferrer';
        a.title = link.uri;
      } else if (link.page >= 0) {
        a.href = `#page-${link.page + 1}`;
        a.title = `Go to page ${link.page + 1}`;
        a.addEventListener('click', (ev) => {
          ev.preventDefault();
          goToPage(link.page);
        });
      }
      a.setAttribute('aria-label', a.title || 'link');
      layer.appendChild(a);
    }
    return layer;
  }

  function highlightLayer(hits, pageEl) {
    const layer = el('div', 'wpdf-page__overlay');
    const scale = effectiveScale();
    const pageIndex = Number(pageEl.dataset.page ?? state.page);
    for (const hit of hits) {
      for (const rect of hit.rects || [hit]) {
        const css = state.doc.boxToCss(rect, pageIndex, scale);
        const mark = el('div', 'wpdf-highlight');
        mark.style.left = `${css.left}px`;
        mark.style.top = `${css.top}px`;
        mark.style.width = `${css.width}px`;
        mark.style.height = `${css.height}px`;
        layer.appendChild(mark);
      }
    }
    return layer;
  }

  function showError(err) {
    state.status = 'error';
    state.error = err;
    const node = typeof opts.renderError === 'function'
      ? opts.renderError(err)
      : messageNode(`${labels.error}\n${err?.message || err}`, 'error');
    setOverlay(node);
    emit('onError', err);
    updateToolbar();
  }

  function goToPage(index, { scroll = true } = {}) {
    if (!state.doc) return;
    const next = Math.max(0, Math.min(state.doc.pageCount - 1, index | 0));
    if (next === state.page && !opts.continuous) return;
    state.page = next;
    // the page on screen must survive cache eviction
    state.doc.pinPage?.(next, effectiveScale(), opts.dpr ?? window.devicePixelRatio ?? 1);
    syncThumbsActive();
    if (opts.continuous) {
      const target = state.pageEls[next];
      if (target && scroll) target.scrollIntoView({ block: 'start', behavior: 'auto' });
      updateToolbar();
      updateStatus();
    } else {
      renderSingle();
    }
    emit('onPageChange', next);
  }

  function setScale(scale, fit = 'none') {
    const next = Math.max(0.1, Math.min(8, Number(scale) || 1));
    state.scale = next;
    state.fit = fit;
    if (state.doc) opts.continuous ? renderContinuous() : renderSingle();
    updateToolbar();
    emit('onScaleChange', next);
  }

  // Rotation is view state, not an edit: the bitmap is reused, so this is a
  // re-layout and (via the page cache) an immediate repaint. The document's own
  // /Rotate is never touched.
  function setRotation(next) {
    const rot = normaliseRotation(next);
    if (rot === state.rotation) return;
    state.rotation = rot;
    opts.rotation = rot;
    if (state.doc) opts.continuous ? renderContinuous() : renderSingle();
    updateToolbar();
    updateStatus();
    emit('onRotationChange', rot);
  }

  /** Turn the view 90° clockwise (dir > 0) or counter-clockwise. */
  function rotateBy(dir = 1) {
    setRotation(stepRotation(state.rotation, dir));
  }

  // Switch between one-page and continuous scrolling without remounting, which
  // is what the demo used to do (and it lost scroll position and search hits).
  function setContinuous(on) {
    const next = !!on;
    if (next === !!opts.continuous) return;
    opts.continuous = next;
    viewport.classList.toggle('wpdf-viewport--continuous', next);
    state.pageEls = [];
    if (state.doc) next ? renderContinuous() : renderSingle();
    updateToolbar();
  }

  async function setDocument(source, { page = 0, filename } = {}) {
    state.status = 'loading';
    state.error = null;
    setOverlay(messageNode(labels.loading, 'loading'));
    try {
      const previous = state.doc;
      const filename2 = filename || (typeof source === 'object' && 'name' in source ? source.name : '');
      let doc;
      try {
        doc = await PdfDocument.open(source, { ...opts, filename: filename2 });
      } catch (err) {
        // Encrypted with a real user password: ask once, then let the error through.
        if (!/user password is required/.test(err.message) || typeof prompt !== 'function') throw err;
        const password = prompt('This PDF is password-protected. Enter the password:');
        if (password === null) throw err;
        doc = await PdfDocument.open(source, { ...opts, filename: filename2, password });
      }
      previous?.close();
      if (destroyed) { doc.close(); return; }
      state.doc = doc;
      state.page = Math.max(0, Math.min(doc.pageCount - 1, page));
      if (state.fit === 'auto') state.fit = 'width';
      state.status = 'ready';
      doc.pinPage?.(state.page, effectiveScale(), opts.dpr ?? window.devicePixelRatio ?? 1);
      setOverlay(null);
      state.pageEls = [];
      opts.continuous ? await renderContinuous() : await renderSingle();
      // fit modes depend on the viewport, which may only settle after this
      // frame; re-render once the layout is real.
      if (state.fit === 'width' || state.fit === 'page') {
        requestAnimationFrame(() => {
          if (!destroyed && state.doc) (opts.continuous ? renderContinuous() : renderSingle());
        });
      }
      updateToolbar();
      updateStatus();
      emit('onReady', { pageCount: doc.pageCount, metadata: doc.metadata, document: doc });
    } catch (err) {
      showError(err);
    }
  }

  // ── wiring ────────────────────────────────────────────────────────────────
  if (toolbarEl) {
    toolbarEl.addEventListener('click', (ev) => {
      const b = ev.target.closest('.wpdf-button');
      if (!b) return;
      if (b === prevBtn) goToPage(state.page - 1);
      else if (b === nextBtn) goToPage(state.page + 1);
      else if (b === rotateLeftBtn) rotateBy(-1);
      else if (b === rotateRightBtn) rotateBy(1);
      else if (b.title === labels.zoomIn) setScale(effectiveScale() * 1.25);
      else if (b.title === labels.zoomOut) setScale(effectiveScale() / 1.25);
      else if (b.title === labels.fitWidth) setScale(state.scale, 'width');
      else if (b.title === labels.fitPage) setScale(state.scale, 'page');
      else if (b === thumbsBtn) setThumbnails(thumbsEl.hidden);
      else if (b === printBtn) printDocument();
    });
    pageInput?.addEventListener('change', () => {
      const n = parseInt(pageInput.value, 10);
      if (Number.isFinite(n)) goToPage(n - 1);
      else updateToolbar();
    });
  }

  function onKey(ev) {
    if (ev.defaultPrevented) return;
    const held = ev.ctrlKey || ev.metaKey;
    if (ev.key === 'ArrowRight' || ev.key === 'PageDown' || (ev.key === ' ' && !ev.shiftKey)) {
      goToPage(state.page + 1); ev.preventDefault();
    } else if (ev.key === 'ArrowLeft' || ev.key === 'PageUp' || (ev.key === ' ' && ev.shiftKey)) {
      goToPage(state.page - 1); ev.preventDefault();
    } else if (ev.key === 'Home') {
      goToPage(0); ev.preventDefault();
    } else if (ev.key === 'End') {
      goToPage((state.doc?.pageCount || 1) - 1); ev.preventDefault();
    } else if (held && (ev.key === '+' || ev.key === '=')) {
      setScale(effectiveScale() * 1.25); ev.preventDefault();
    } else if (held && ev.key === '-') {
      setScale(effectiveScale() / 1.25); ev.preventDefault();
    } else if (held && ev.key === '0') {
      setScale(state.scale, 'width'); ev.preventDefault();
    }
  }
  root.addEventListener('keydown', onKey);
  root.tabIndex = 0;

  const onWheel = (ev) => {
    if (!(ev.ctrlKey || ev.metaKey)) return;
    ev.preventDefault();
    setScale(effectiveScale() * (ev.deltaY < 0 ? 1.1 : 1 / 1.1));
  };
  viewport.addEventListener('wheel', onWheel, { passive: false });

  let resizeTimer = 0;
  const resizeObserver = new ResizeObserver(() => {
    if (!state.doc || !(state.fit === 'width' || state.fit === 'page')) return;
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(() => {
      opts.continuous ? renderContinuous() : renderSingle();
    }, 120);
  });
  resizeObserver.observe(viewport);

  // track the visible page while scrolling in continuous mode
  if (opts.continuous) {
    viewport.addEventListener('scroll', () => {
      let best = state.page, bestDist = Infinity;
      for (let i = 0; i < state.pageEls.length; i++) {
        const elm = state.pageEls[i];
        if (!elm) continue;
        const dist = Math.abs(elm.offsetTop - viewport.scrollTop);
        if (dist < bestDist) { bestDist = dist; best = i; }
      }
      if (best !== state.page) {
        state.page = best;
        updateToolbar();
        updateStatus();
        emit('onPageChange', best);
      }
    }, { passive: true });
  }

  // ── thumbnails ────────────────────────────────────────────────────────────
  // A page rail is the fastest way to navigate a long document, but rendering
  // every page up front costs more than the first page does. Thumbnails are
  // therefore built on demand: the rail is populated with placeholders, and an
  // IntersectionObserver renders a page only when it scrolls into view.
  let thumbsReady = false;
  let thumbsObserver = null;

  function thumbScale(doc, index) {
    const { width } = doc.getPageSize(index);
    return Math.min(0.5, 130 / Math.max(1, width));
  }

  function buildThumbs() {
    if (thumbsReady || !state.doc) return;
    thumbsReady = true;
    const doc = state.doc;
    thumbsEl.textContent = '';
    thumbsObserver?.disconnect();
    thumbsObserver = new IntersectionObserver((entries) => {
      for (const entry of entries) {
        if (!entry.isIntersecting) continue;
        const btn = entry.target;
        thumbsObserver.unobserve(btn);
        drawThumb(btn, Number(btn.dataset.page));
      }
    }, { root: thumbsEl, rootMargin: '200px' });

    for (let i = 0; i < doc.pageCount; i++) {
      const btn = el('button', 'wpdf-thumb');
      btn.type = 'button';
      btn.dataset.page = String(i);
      btn.setAttribute('aria-label', `${labels.page} ${i + 1}`);
      if (i === state.page) btn.classList.add('wpdf-thumb--active');
      btn.appendChild(el('span', 'wpdf-thumb__num', String(i + 1)));
      btn.appendChild(el('div', 'wpdf-thumb__canvas'));
      thumbsEl.appendChild(btn);
      thumbsObserver.observe(btn);
    }
  }

  async function drawThumb(btn, index) {
    const doc = state.doc;
    if (!doc || index < 0 || index >= doc.pageCount) return;
    try {
      const out = await doc.render(index, { scale: thumbScale(doc, index) });
      const holder = btn.querySelector('.wpdf-thumb__canvas');
      if (!holder) return;
      const canvas = document.createElement('canvas');
      canvas.width = out.imageData.width;
      canvas.height = out.imageData.height;
      canvas.getContext('2d').putImageData(out.imageData, 0, 0);
      holder.textContent = '';
      holder.appendChild(canvas);
    } catch (err) {
      btn.classList.add('wpdf-thumb--error');
    }
  }

  function syncThumbsActive() {
    if (thumbsEl.hidden) return;
    for (const btn of thumbsEl.querySelectorAll('.wpdf-thumb')) {
      const on = Number(btn.dataset.page) === state.page;
      btn.classList.toggle('wpdf-thumb--active', on);
      if (on && btn.scrollIntoView) {
        btn.scrollIntoView({ block: 'nearest', inline: 'nearest' });
      }
    }
  }

  function setThumbnails(on) {
    const show = !!on;
    thumbsEl.hidden = !show;
    if (thumbsBtn) {
      thumbsBtn.classList.toggle('wpdf-button--active', show);
      thumbsBtn.setAttribute('aria-pressed', show ? 'true' : 'false');
    }
    if (show) {
      buildThumbs();
      syncThumbsActive();
    }
  }

  thumbsEl.addEventListener('click', (ev) => {
    const btn = ev.target.closest('.wpdf-thumb');
    if (!btn) return;
    goToPage(Number(btn.dataset.page));
  });

  // ── print ─────────────────────────────────────────────────────────────────
  // The viewer prints what it renders: every page is rasterized at ~120 dpi
  // into a detached container, printed, then thrown away. A host stylesheet
  // cannot reach into that container, and the visible viewer is untouched.
  let printing = false;

  async function printDocument() {
    const doc = state.doc;
    if (!doc || printing) return;
    printing = true;
    const wasStatus = state.status;
    if (printBtn) printBtn.disabled = true;
    const sheet = el('div', 'wpdf-print');

    try {
      for (let i = 0; i < doc.pageCount; i++) {
        const { width, height } = doc.getPageSize(i);
        const scale = Math.min(2, 120 / 72);
        const out = await doc.render(i, { scale });
        const page = el('div', 'wpdf-print__page');
        page.style.width = `${width}pt`;
        page.style.height = `${height}pt`;
        const canvas = document.createElement('canvas');
        canvas.width = out.imageData.width;
        canvas.height = out.imageData.height;
        canvas.getContext('2d').putImageData(out.imageData, 0, 0);
        page.appendChild(canvas);
        sheet.appendChild(page);
      }
      document.body.appendChild(sheet);
      document.body.classList.add('wpdf-printing');
      window.print();
    } catch (err) {
      showError(err);
    } finally {
      setTimeout(() => {
        sheet.remove();
        document.body.classList.remove('wpdf-printing');
        if (printBtn) printBtn.disabled = false;
        printing = false;
        state.status = wasStatus;
      }, 250);
    }
  }

  const controller = {
    element: root,
    get document() { return state.doc; },
    get state() { return publicState(); },
    goToPage,
    nextPage: () => goToPage(state.page + 1),
    prevPage: () => goToPage(state.page - 1),
    setScale,
    zoomIn: () => setScale(effectiveScale() * 1.25),
    zoomOut: () => setScale(effectiveScale() / 1.25),
    fitWidth: () => setScale(state.scale, 'width'),
    fitPage: () => setScale(state.scale, 'page'),
    setRotation,
    rotateLeft: () => rotateBy(-1),
    rotateRight: () => rotateBy(1),
    get rotation() { return state.rotation; },
    setFile: (source, o) => setDocument(source, o),
    setTheme: (theme) => applyTheme(root, theme),
    setInverted: (on) => root.classList.toggle('wpdf-root--invert', !!on),
    search: (query, o) => (state.doc ? state.doc.search(query, o) : []),
    setContinuous,
    setThumbnails,
    isThumbnailsVisible: () => !thumbsEl.hidden,
    print: printDocument,
    isContinuous: () => !!opts.continuous,
    rerender: () => (opts.continuous ? renderContinuous() : renderSingle()),
    destroy() {
      destroyed = true;
      resizeObserver.disconnect();
      observer?.disconnect();
      thumbsObserver?.disconnect();
      viewport.removeEventListener('wheel', onWheel);
      root.remove();
      state.doc?.close();
      state.doc = null;
    },
  };

  if (opts.file) setDocument(opts.file, { page: opts.page });
  else setOverlay(messageNode(labels.empty));

  return controller;
}

export default mountPdfReader;
