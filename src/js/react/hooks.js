// hooks.js — React state for documents and page bitmaps.
//
// These are the pieces to reach for when the built-in <PdfReader> is too much
// chrome: they do no layout and no styling, they only own the document
// lifecycle and the pixel copy out of wasm.

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { PdfDocument } from '../document.js';

/**
 * Open (and close) a document for the lifetime of a component.
 *
 * @param {ArrayBuffer|Uint8Array|Blob|File|string|URL|null} source
 * @param {{wasmUrl?: string, wasmBinary?: ArrayBuffer, module?: Object, filename?: string}} [options]
 * @param {any[]} [deps] extra dependencies that should reopen the document
 * @returns {{document: PdfDocument|null, status: 'idle'|'loading'|'ready'|'error',
 *            error: Error|null, pageCount: number, metadata: Object, warnings: string[],
 *            bytes: number, reload: () => void}}
 */
export function usePdfDocument(source, options = {}, deps = []) {
  const [state, setState] = useState({
    document: null, status: source ? 'loading' : 'idle', error: null, bytes: 0,
  });
  const [nonce, setNonce] = useState(0);
  const alive = useRef(true);

  useEffect(() => {
    alive.current = true;
    return () => { alive.current = false; };
  }, []);

  useEffect(() => {
    if (!source) {
      setState({ document: null, status: 'idle', error: null, bytes: 0 });
      return undefined;
    }
    let cancelled = false;
    let opened = null;
    setState((s) => ({ ...s, status: 'loading', error: null }));

    PdfDocument.open(source, options)
      .then((doc) => {
        opened = doc;
        if (cancelled) { doc.close(); return; }
        setState({ document: doc, status: 'ready', error: null, bytes: doc.pageCount });
      })
      .catch((err) => {
        if (cancelled) return;
        setState({ document: null, status: 'error', error: err, bytes: 0 });
      });

    return () => {
      cancelled = true;
      opened?.close();
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [source, nonce, ...deps]);

  const reload = useCallback(() => setNonce((n) => n + 1), []);

  const metadata = useMemo(() => state.document?.metadata ?? {}, [state.document]);
  const warnings = useMemo(() => state.document?.warnings ?? [], [state.document]);
  const pageCount = state.document?.pageCount ?? 0;

  return { ...state, pageCount, metadata, warnings, reload };
}

/**
 * Render one page into a canvas owned by the hook.
 *
 * @param {PdfDocument|null} doc
 * @param {number} page zero-based
 * @param {{scale?: number, dpr?: number, pixelTransform?: Function, enabled?: boolean,
 *          renderer?: {render: (doc:PdfDocument, page:number, opts:Object) => Promise<ImageData>}}} [options]
 * @returns {{canvasRef: Function, width: number, height: number, cssWidth: number, cssHeight: number,
 *            rendering: boolean, error: Error|null, stats: Object|null, render: Function}}
 */
export function usePdfPage(doc, page, options = {}) {
  const { scale = 1.5, dpr, pixelTransform, enabled = true, renderer } = options;
  const canvasRef = useRef(null);
  const [state, setState] = useState({
    rendering: false, error: null, width: 0, height: 0, cssWidth: 0, cssHeight: 0, stats: null,
  });
  const token = useRef(0);

  const deviceDpr = dpr ?? (typeof window !== 'undefined' ? window.devicePixelRatio || 1 : 1);

  const render = useCallback(async (canvas) => {
    const target = canvas || canvasRef.current;
    if (!doc || !target || !enabled) return;
    const my = ++token.current;
    setState((s) => ({ ...s, rendering: true, error: null }));
    try {
      if (renderer) {
        // A host-supplied backend (server render, pdf.js, a PDF writer, ...).
        const image = await renderer.render(doc, page, { scale, dpr: deviceDpr, pixelTransform });
        const ctx = target.getContext('2d');
        target.width = image.width;
        target.height = image.height;
        target.style.width = `${image.width / deviceDpr}px`;
        target.style.height = `${image.height / deviceDpr}px`;
        if (typeof ImageData !== 'undefined' && image instanceof ImageData) ctx.putImageData(image, 0, 0);
        else ctx.drawImage(image, 0, 0);
        if (my !== token.current) return;
        setState({
          rendering: false, error: null,
          width: image.width, height: image.height,
          cssWidth: image.width / deviceDpr, cssHeight: image.height / deviceDpr,
          stats: null,
        });
        return;
      }
      const out = await doc.renderToCanvas(target, page, {
        scale, dpr: deviceDpr, pixelTransform, stats: true,
      });
      if (my !== token.current) return;
      setState({
        rendering: false,
        error: null,
        width: out.width,
        height: out.height,
        cssWidth: out.cssWidth,
        cssHeight: out.cssHeight,
        stats: out.stats ?? null,
      });
    } catch (err) {
      if (my !== token.current) return;
      setState((s) => ({ ...s, rendering: false, error: err }));
    }
  }, [doc, page, scale, deviceDpr, pixelTransform, enabled, renderer]);

  useEffect(() => { render(); }, [render]);

  const setCanvas = useCallback((node) => {
    canvasRef.current = node;
    if (node) render(node);
  }, [render]);

  return { canvasRef: setCanvas, ...state, render };
}

/**
 * Lazily render the page that sits in view (for continuous mode).
 * Returns a ref callback and a boolean telling you whether to render now.
 */
export function useInView(options = {}) {
  const [visible, setVisible] = useState(false);
  const ref = useRef(null);
  const observer = useRef(null);
  const { rootMargin = '400px 0px', root = null } = options;

  const setRef = useCallback((node) => {
    observer.current?.disconnect();
    ref.current = node;
    if (!node || typeof IntersectionObserver === 'undefined') { setVisible(true); return; }
    observer.current = new IntersectionObserver(
      ([entry]) => setVisible(entry.isIntersecting),
      { root, rootMargin },
    );
    observer.current.observe(node);
  }, [root, rootMargin]);

  useEffect(() => () => observer.current?.disconnect(), []);
  return [setRef, visible];
}
