// PdfReader — a complete viewer, assembled from parts you can replace.
//
//   <PdfReader file={file} />
//
// Every layer is swappable:
//   toolbar={false | node | (api) => node}   chrome
//   components={{ Page, Toolbar }}           implementations
//   pageOverlay={(page, api) => node}        highlight/annotation layer
//   renderLoading / renderError / renderEmpty
//   theme={{ accent: '#6ee7b7' }}            CSS variables
//   pixelTransform={(rgba) => {...}}         post-process page bitmaps
//   renderer={{ render: async (doc, i, o) => ImageData }}  different backend
//
// Controlled or uncontrolled: pass `page`/`scale` to control them, or
// `defaultPage`/`defaultScale` and read changes from the callbacks.

import React, {
  forwardRef, useCallback, useEffect, useImperativeHandle, useMemo, useRef, useState,
} from 'react';
import { usePdfDocument } from './hooks.js';
import { PdfPage } from './PdfPage.jsx';
import { PdfToolbar } from './PdfToolbar.jsx';
import { turnedPageBox, normaliseRotation, stepRotation } from '../rotation.js';

const PAD = 32;

function themeVars(theme) {
  if (!theme) return undefined;
  const style = {};
  for (const [key, value] of Object.entries(theme)) {
    if (value == null) continue;
    const name = key.replace(/[A-Z]/g, (m) => `-${m.toLowerCase()}`);
    style[`--wpdf-${name}`] = String(value);
  }
  return style;
}

export const PdfReader = forwardRef(function PdfReader(props, ref) {
  const {
    file,
    page: pageProp,
    defaultPage = 0,
    onPageChange,
    scale: scaleProp,
    defaultScale = 1.5,
    defaultRotation = 0,
    onRotationChange,
    fit: fitProp,
    onZoom,
    continuous = false,
    toolbar = true,
    components = {},
    theme,
    labels,
    className = '',
    style,
    showStatus = true,
    invert = false,
    dpr,
    pixelTransform,
    renderer,
    highlights,
    search,
    wasmUrl,
    wasmBinary,
    module,
    worker,
    workerUrl,
    workerFactory,
    renderLoading,
    renderError,
    renderEmpty,
    pageOverlay,
    onLink,
    onLoad,
    onError,
    onRender,
    children,
    title,
    ...rest
  } = props;

  const Toolbar = components.Toolbar || PdfToolbar;
  const Page = components.Page || PdfPage;

  const [innerPage, setInnerPage] = useState(Math.max(0, defaultPage | 0));
  const [innerScale, setInnerScale] = useState(defaultScale);
  const [rotation, setRotationState] = useState(normaliseRotation(defaultRotation));
  const [fit, setFit] = useState(fitProp ?? 'auto');
  const [manualFit, setManualFit] = useState(false);
  const [viewportBox, setViewportBox] = useState({ width: 0, height: 0 });
  const [lastStats, setLastStats] = useState(null);

  const viewportRef = useRef(null);
  const rootRef = useRef(null);

  const { document: doc, status, error, pageCount, metadata, warnings, reload } = usePdfDocument(
    file,
    { wasmUrl, wasmBinary, module, worker, workerUrl, workerFactory, filename: file?.name },
  );

  const page = pageProp !== undefined ? pageProp : innerPage;
  const controlledScale = scaleProp !== undefined ? scaleProp : innerScale;
  const activeFit = manualFit ? fit : (fitProp !== undefined ? fitProp : 'auto');

  // ── geometry ──────────────────────────────────────────────────────────────
  useEffect(() => {
    const el = viewportRef.current;
    if (!el || typeof ResizeObserver === 'undefined') return undefined;
    const ro = new ResizeObserver(() => {
      setViewportBox({ width: el.clientWidth, height: el.clientHeight });
    });
    ro.observe(el);
    setViewportBox({ width: el.clientWidth, height: el.clientHeight });
    return () => ro.disconnect();
  }, []);

  const effectiveScale = useMemo(() => {
    const size = doc && pageCount ? doc.getPageSize(Math.min(page, pageCount - 1)) : null;
    if (!size) return controlledScale;
    const fitMode = activeFit === undefined || activeFit === null ? 'none' : activeFit;
    if (fitMode === 'none') return controlledScale;
    // Fit the turned box: a landscape view of a portrait page has to fit on its
    // shorter axis, not the one the bitmap was rendered at.
    const box = turnedPageBox(size, 1, rotation);
    const availW = Math.max(120, (viewportBox.width || 800) - PAD);
    const availH = Math.max(120, (viewportBox.height || 800) - PAD);
    let s = fitMode === 'page'
      ? Math.min(availW / box.width, availH / box.height)
      : availW / box.width;
    // The viewport is a flex item, so a wide page can stretch it and feed
    // fit-width a larger width next frame - the scale then runs away to the
    // clamp. A fitted page never needs to be wider than the window.
    if (typeof window !== 'undefined' && box.width > 0) {
      s = Math.min(s, (window.innerWidth * 0.98) / box.width);
    }
    return Math.max(0.1, Math.min(8, s || controlledScale));
  }, [doc, pageCount, page, controlledScale, activeFit, viewportBox, rotation]);

  useEffect(() => { onLoad?.({ document: doc, pageCount, metadata, warnings }); }, [doc, pageCount, metadata, warnings, onLoad]);
  useEffect(() => { if (error) onError?.(error); }, [error, onError]);

  useEffect(() => {
    if (pageCount && page > pageCount - 1) setInnerPage(pageCount - 1);
  }, [pageCount, page]);

  // ── navigation ────────────────────────────────────────────────────────────
  const goToPage = useCallback((next) => {
    const clamped = Math.max(0, Math.min((pageCount || 1) - 1, Math.round(next)));
    if (pageProp === undefined) setInnerPage(clamped);
    onPageChange?.(clamped);
    if (continuous && viewportRef.current) {
      const target = viewportRef.current.querySelector(`[data-page="${clamped}"]`);
      target?.scrollIntoView({ block: 'start' });
    }
  }, [pageCount, pageProp, onPageChange, continuous]);

  const setScale = useCallback((next) => {
    const clamped = Math.max(0.1, Math.min(8, Number(next) || 1));
    setManualFit(true);
    setFit('none');
    if (scaleProp === undefined) setInnerScale(clamped);
    onZoom?.(clamped);
  }, [scaleProp, onZoom]);

  const fitTo = useCallback((mode) => {
    setManualFit(false);
    setFit(mode);
    onZoom?.(effectiveScale);
  }, [effectiveScale, onZoom]);

  // A turn is a view change, not document state: nothing is re-rendered, the
  // cached bitmap is re-laid-out with the box swapped.
  const rotate = useCallback((dir = 1) => {
    setRotationState((prev) => {
      const next = stepRotation(prev, dir);
      if (next !== prev) onRotationChange?.(next);
      return next;
    });
  }, [onRotationChange]);

  const rotateTo = useCallback((value) => {
    const next = normaliseRotation(value);
    setRotationState((prev) => {
      if (next !== prev) onRotationChange?.(next);
      return next;
    });
  }, [onRotationChange]);

  useImperativeHandle(ref, () => ({
    get document() { return doc; },
    get page() { return page; },
    get scale() { return effectiveScale; },
    pageCount,
    goToPage,
    nextPage: () => goToPage(page + 1),
    prevPage: () => goToPage(page - 1),
    setScale,
    zoomIn: () => setScale(effectiveScale * 1.25),
    zoomOut: () => setScale(effectiveScale / 1.25),
    fitWidth: () => fitTo('width'),
    fitPage: () => fitTo('page'),
    get rotation() { return rotation; },
    setRotation: rotateTo,
    rotateLeft: () => rotate(-1),
    rotateRight: () => rotate(1),
    reload,
    /** Plain text for a page (extracts and caches it in wasm). A worker-backed
     *  document returns a Promise, so `await` it when `worker` is set. */
    pageText: (i = page) => doc?.getPageText(i) ?? '',
    /** All matches for a query: {page, index, length, line, rects}. Returns a
     *  Promise on a worker-backed document. */
    search: (q, o) => doc?.search(q, o) ?? [],
  }), [doc, page, effectiveScale, pageCount, goToPage, setScale, fitTo, reload,
      rotation, rotateTo, rotate]);

  // ── keyboard ──────────────────────────────────────────────────────────────
  const onKeyDown = useCallback((ev) => {
    const held = ev.ctrlKey || ev.metaKey;
    const keys = {
      ArrowRight: () => goToPage(page + 1),
      PageDown: () => goToPage(page + 1),
      ArrowLeft: () => goToPage(page - 1),
      PageUp: () => goToPage(page - 1),
      Home: () => goToPage(0),
      End: () => goToPage(pageCount - 1),
    };
    if (keys[ev.key]) { ev.preventDefault(); keys[ev.key](); return; }
    if (held && (ev.key === '+' || ev.key === '=')) { ev.preventDefault(); setScale(effectiveScale * 1.25); }
    else if (held && ev.key === '-') { ev.preventDefault(); setScale(effectiveScale / 1.25); }
    else if (held && ev.key === '0') { ev.preventDefault(); fitTo('width'); }
  }, [goToPage, page, pageCount, setScale, effectiveScale, fitTo]);

  // ── highlights ────────────────────────────────────────────────────────────
  // `doc.search()` is synchronous inline and a Promise on a worker-backed
  // document, so both shapes land in the same state.
  const [searchHits, setSearchHits] = useState([]);
  const searchKey = typeof search === 'string' ? search : JSON.stringify(search ?? null);
  useEffect(() => {
    if (!doc || !search) { setSearchHits([]); return undefined; }
    let alive = true;
    let result;
    try { result = doc.search(search); } catch { setSearchHits([]); return undefined; }
    if (result && typeof result.then === 'function') {
      result
        .then((hits) => { if (alive) setSearchHits(hits || []); })
        .catch(() => { if (alive) setSearchHits([]); });
    } else {
      setSearchHits(result || []);
    }
    return () => { alive = false; };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [doc, searchKey]);

  const overlayFor = useCallback((index) => {
    const parts = [];
    const rects = [];
    const links = doc ? doc.getLinks(index) : [];
    if (links.length) {
      parts.push(links.map((link, i) => {
        const css = doc.boxToCss(link, index, effectiveScale);
        const external = !!link.uri;
        return (
          <a
            key={`ln-${i}`}
            className="wpdf-link"
            href={external ? link.uri : `#page-${link.page + 1}`}
            target={external ? '_blank' : undefined}
            rel={external ? 'noopener noreferrer' : undefined}
            title={external ? link.uri : `Go to page ${link.page + 1}`}
            style={{ left: css.left, top: css.top, width: css.width, height: css.height }}
            onClick={(ev) => {
              const handled = onLink?.(link, { document: doc, goToPage, pageCount });
              if (handled === false) return;         // custom handler wants the event
              if (!external && link.page >= 0) { ev.preventDefault(); goToPage(link.page); }
            }}
          />
        );
      }));
    }
    if (searchHits.length) {
      for (const hit of searchHits) if (hit.page === index) rects.push(...hit.rects);
    }
    const custom = typeof highlights === 'function' ? highlights(index, effectiveScale) : highlights;
    if (Array.isArray(custom)) {
      for (const hit of custom) {
        if (hit.page !== undefined && hit.page !== index) continue;
        rects.push(...(hit.rects || [hit]));
      }
    }
    if (rects.length && doc) {
      parts.push(rects.map((rect, i) => {
        const css = doc.boxToCss(rect, index, effectiveScale);
        return (
          <div
            key={`hl-${i}`}
            className="wpdf-highlight"
            style={{ left: css.left, top: css.top, width: css.width, height: css.height }}
          />
        );
      }));
    }
    if (pageOverlay) parts.push(pageOverlay(index, { document: doc, scale: effectiveScale, pageCount }));
    return parts;
  }, [searchHits, highlights, pageOverlay, onLink, doc, effectiveScale, goToPage, pageCount]);

  // ── render ────────────────────────────────────────────────────────────────
  const pageSize = doc && pageCount ? doc.getPageSize(Math.min(page, pageCount - 1)) : null;
  const turnedBox = turnedPageBox(pageSize || { width: 612, height: 792 }, 1, rotation);
  const titleText = title || metadata?.Title || (typeof file === 'object' && file ? file.name : '') || '';
  const toolbarProps = {
    page,
    pageCount,
    scale: effectiveScale,
    rotation,
    onPageChange: goToPage,
    onZoom: setScale,
    onFit: fitTo,
    onRotate: rotate,
    labels,
    title: toolbar === true ? titleText : undefined,
  };

  let toolbarNode = null;
  if (toolbar === true) toolbarNode = <Toolbar {...toolbarProps} />;
  else if (typeof toolbar === 'function') toolbarNode = toolbar(toolbarProps);
  else if (React.isValidElement(toolbar)) toolbarNode = toolbar;

  let overlayNode = null;
  if (status === 'loading') {
    overlayNode = renderLoading ? renderLoading() : (
      <div><span className="wpdf-spinner" />Loading document…</div>
    );
  } else if (status === 'error') {
    overlayNode = renderError ? renderError(error) : (
      <div className="wpdf-overlay--error">Could not open this PDF — {error?.message}</div>
    );
  } else if (status === 'idle') {
    overlayNode = renderEmpty ? renderEmpty() : <div>Drop a PDF here, or pass a `file` prop</div>;
  }

  const pageNode = (index, lazy) => (
    <Page
      key={index}
      document={doc}
      page={index}
      scale={effectiveScale}
      rotation={rotation}
      dpr={dpr}
      pixelTransform={pixelTransform}
      renderer={renderer}
      lazy={lazy}
      onRender={(info) => { if (index === page) setLastStats(info.stats); onRender?.(info); }}
      onError={onError}
    >
      {overlayFor(index)}
    </Page>
  );

  return (
    <div
      ref={rootRef}
      className={`wpdf-root ${invert ? 'wpdf-root--invert' : ''} ${className}`.trim()}
      style={{ ...themeVars(theme), ...style }}
      tabIndex={0}
      onKeyDown={onKeyDown}
      data-status={status}
      {...rest}
    >
      {toolbarNode}
      <div
        ref={viewportRef}
        className={`wpdf-viewport ${continuous ? 'wpdf-viewport--continuous' : ''}`.trim()}
      >
        {status === 'ready' && doc
          ? (continuous
            ? Array.from({ length: pageCount }, (_, i) => pageNode(i, true))
            : pageNode(Math.min(page, pageCount - 1), false))
          : null}
      </div>

      {showStatus && (
        <div className="wpdf-status">
          <span>{pageCount ? `Page ${page + 1} of ${pageCount}` : '—'}</span>
          {pageSize && <span>{Math.round(turnedBox.width)} × {Math.round(turnedBox.height)} pt</span>}
          {rotation ? <span>{rotation}°</span> : null}
          <span>{Math.round(effectiveScale * 150)} dpi</span>
          {lastStats && <span>{lastStats.glyphs} glyphs</span>}
          {warnings.length > 0 && <span>{warnings.length} warning{warnings.length === 1 ? '' : 's'}</span>}
          <span className="wpdf-toolbar__spacer" />
          {titleText && <span title={titleText}>{titleText}</span>}
        </div>
      )}

      {overlayNode && <div className="wpdf-overlay">{overlayNode}</div>}
      {children}
    </div>
  );
});

export default PdfReader;
