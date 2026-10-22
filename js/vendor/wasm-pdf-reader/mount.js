// mount.js — the framework-free viewer: `mountPdfReader(element, options)`.
//
// Everything is built with DOM APIs (no innerHTML, no template strings), so it
// is safe with a strict CSP and with untrusted file names. The returned
// controller is the imperative API; the React components are a thin layer over
// the same PdfDocument class.

import { PdfDocument } from './document.js';
import { buildTextLayer } from './text.js';

const ICONS = {
  prev: '‹',
  next: '›',
  zoomOut: '−',
  zoomIn: '+',
  fitWidth: '⇔',
  fitPage: '⤢',
};

/** @typedef {Object} ViewerLabels */
export const defaultLabels = {
  previousPage: 'Previous page',
  nextPage: 'Next page',
  zoomIn: 'Zoom in',
  zoomOut: 'Zoom out',
  fitWidth: 'Fit width',
  fitPage: 'Fit page',
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
 * @param {boolean} [options.continuous=false] continuous scroll instead of one page
 * @param {boolean} [options.toolbar=true]  build the default toolbar
 * @param {HTMLElement|Function} [options.toolbarSlot] custom toolbar (node, or fn(state) -> node)
 * @param {HTMLElement} [options.statusSlot] custom status bar content
 * @param {boolean} [options.status=true]
 * @param {Object} [options.theme] CSS variable overrides
 * @param {ViewerLabels} [options.labels]
 * @param {Function} [options.pixelTransform] passed to PdfDocument.render
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

    root.appendChild(toolbarEl);
  } else if (opts.toolbarSlot) {
    const custom = typeof opts.toolbarSlot === 'function'
      ? opts.toolbarSlot({ getState: () => publicState() })
      : opts.toolbarSlot;
    if (custom) root.appendChild(custom);
  }

  // ── viewport + status ─────────────────────────────────────────────────────
  const viewport = el('div', `wpdf-viewport${opts.continuous ? ' wpdf-viewport--continuous' : ''}`);
  root.appendChild(viewport);

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

  function pageScale() {
    const size = state.doc?.getPageSize(state.page) || { width: 612, height: 792 };
    const pad = 32;
    // Before the first layout pass the viewport can report 0; fall back to the
    // window so the first page is never rendered at thumbnail size.
    const vw = Math.max(120, (viewport.clientWidth || window.innerWidth) - pad);
    const vh = Math.max(120, (viewport.clientHeight || window.innerHeight - 120) - pad);
    if (state.fit === 'width') return vw / size.width;
    if (state.fit === 'page') return Math.min(vw / size.width, vh / size.height);
    return state.scale;
  }

  function effectiveScale() {
    const s = state.fit && state.fit !== 'none' ? pageScale() : state.scale;
    return Math.max(0.05, Math.min(8, s || 1));
  }

  function makePage(scale) {
    const pageEl = el('div', 'wpdf-page');
    const canvas = el('canvas', 'wpdf-page__canvas');
    pageEl.appendChild(canvas);
    const size = state.doc.getPageSize(state.page);
    pageEl.style.width = `${size.width * scale}px`;
    pageEl.style.height = `${size.height * scale}px`;
    return pageEl;
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
      if (opts.textLayer !== false) {
        const size = state.doc.getPageSize(state.page);
        const textEl = buildTextLayer(state.doc.getText(state.page), {
          scale, height: size.height, boxY0: size.boxY0,
        });
        pageEl.appendChild(textEl);
      }
      const linkEl = linkLayer(pageEl, state.page);
      if (linkEl) pageEl.appendChild(linkEl);
      const hl = opts.highlights?.(state.page, scale) || [];
      if (hl.length) pageEl.appendChild(highlightLayer(hl, pageEl));
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
    }, { root: viewport, rootMargin: '300px 0px' });

    for (let i = 0; i < state.doc.pageCount; i++) {
      const size = state.doc.getPageSize(i);
      const pageEl = el('div', 'wpdf-page');
      pageEl.dataset.page = String(i);
      pageEl.style.width = `${size.width * scale}px`;
      pageEl.style.height = `${size.height * scale}px`;
      const ph = el('div', 'wpdf-page__placeholder');
      ph.style.width = '100%';
      ph.style.height = '100%';
      ph.textContent = `${i + 1}`;
      pageEl.appendChild(ph);
      viewport.appendChild(pageEl);
      state.pageEls[i] = pageEl;
      observer.observe(pageEl);
    }
    updateToolbar();
  }

  async function renderInto(pageEl, index, scale, token) {
    if (pageEl.dataset.rendered === String(scale)) return;
    pageEl.dataset.rendered = String(scale);
    let canvas = pageEl.querySelector('canvas');
    if (!canvas) {
      canvas = el('canvas', 'wpdf-page__canvas');
      pageEl.replaceChildren(canvas);
    }
    try {
      const out = await state.doc.renderToCanvas(canvas, index, {
        scale,
        dpr: opts.dpr ?? window.devicePixelRatio ?? 1,
        pixelTransform: opts.pixelTransform,
      });
      if (token !== renderToken || destroyed) return;
      emit('onRender', { page: index, scale, ...out });
      const hl = opts.highlights?.(index, scale) || [];
      if (hl.length) pageEl.appendChild(highlightLayer(hl, pageEl));
      if (index === state.page) updateStatus();
    } catch (err) {
      pageEl.dataset.rendered = '';
      if (token === renderToken) showError(err);
    }
  }

  // Link annotations: every /Link with a URI or an internal /GoTo becomes a
  // transparent anchor over the page bitmap.
  function linkLayer(pageEl, pageIndex) {
    const links = state.doc.getLinks(pageIndex);
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
      else if (b.title === labels.zoomIn) setScale(effectiveScale() * 1.25);
      else if (b.title === labels.zoomOut) setScale(effectiveScale() / 1.25);
      else if (b.title === labels.fitWidth) setScale(state.scale, 'width');
      else if (b.title === labels.fitPage) setScale(state.scale, 'page');
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
    setFile: (source, o) => setDocument(source, o),
    setTheme: (theme) => applyTheme(root, theme),
    setInverted: (on) => root.classList.toggle('wpdf-root--invert', !!on),
    search: (query, o) => (state.doc ? state.doc.search(query, o) : []),
    rerender: () => (opts.continuous ? renderContinuous() : renderSingle()),
    destroy() {
      destroyed = true;
      resizeObserver.disconnect();
      observer?.disconnect();
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
