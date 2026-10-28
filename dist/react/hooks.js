import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { PdfDocument } from "../document.js";
function usePdfDocument(source, options = {}, deps = []) {
  const [state, setState] = useState({
    document: null,
    status: source ? "loading" : "idle",
    error: null,
    bytes: 0
  });
  const [nonce, setNonce] = useState(0);
  const alive = useRef(true);
  useEffect(() => {
    alive.current = true;
    return () => {
      alive.current = false;
    };
  }, []);
  useEffect(() => {
    if (!source) {
      setState({ document: null, status: "idle", error: null, bytes: 0 });
      return void 0;
    }
    let cancelled = false;
    let opened = null;
    setState((s) => ({ ...s, status: "loading", error: null }));
    PdfDocument.open(source, options).then((doc) => {
      opened = doc;
      if (cancelled) {
        doc.close();
        return;
      }
      setState({ document: doc, status: "ready", error: null, bytes: doc.pageCount });
    }).catch((err) => {
      if (cancelled) return;
      setState({ document: null, status: "error", error: err, bytes: 0 });
    });
    return () => {
      cancelled = true;
      opened?.close();
    };
  }, [source, nonce, ...deps]);
  const reload = useCallback(() => setNonce((n) => n + 1), []);
  const metadata = useMemo(() => state.document?.metadata ?? {}, [state.document]);
  const warnings = useMemo(() => state.document?.warnings ?? [], [state.document]);
  const pageCount = state.document?.pageCount ?? 0;
  return { ...state, pageCount, metadata, warnings, reload };
}
function usePdfPage(doc, page, options = {}) {
  const { scale = 1.5, dpr, pixelTransform, enabled = true, renderer } = options;
  const canvasRef = useRef(null);
  const [state, setState] = useState({
    rendering: false,
    error: null,
    width: 0,
    height: 0,
    cssWidth: 0,
    cssHeight: 0,
    stats: null
  });
  const token = useRef(0);
  const deviceDpr = dpr ?? (typeof window !== "undefined" ? window.devicePixelRatio || 1 : 1);
  const render = useCallback(async (canvas) => {
    const target = canvas || canvasRef.current;
    if (!doc || !target || !enabled) return;
    const my = ++token.current;
    setState((s) => ({ ...s, rendering: true, error: null }));
    try {
      if (renderer) {
        const image = await renderer.render(doc, page, { scale, dpr: deviceDpr, pixelTransform });
        const ctx = target.getContext("2d");
        target.width = image.width;
        target.height = image.height;
        target.style.width = `${image.width / deviceDpr}px`;
        target.style.height = `${image.height / deviceDpr}px`;
        if (typeof ImageData !== "undefined" && image instanceof ImageData) ctx.putImageData(image, 0, 0);
        else ctx.drawImage(image, 0, 0);
        if (my !== token.current) return;
        setState({
          rendering: false,
          error: null,
          width: image.width,
          height: image.height,
          cssWidth: image.width / deviceDpr,
          cssHeight: image.height / deviceDpr,
          stats: null
        });
        return;
      }
      const out = await doc.renderToCanvas(target, page, {
        scale,
        dpr: deviceDpr,
        pixelTransform,
        stats: true
      });
      if (my !== token.current) return;
      setState({
        rendering: false,
        error: null,
        width: out.width,
        height: out.height,
        cssWidth: out.cssWidth,
        cssHeight: out.cssHeight,
        stats: out.stats ?? null
      });
    } catch (err) {
      if (my !== token.current) return;
      setState((s) => ({ ...s, rendering: false, error: err }));
    }
  }, [doc, page, scale, deviceDpr, pixelTransform, enabled, renderer]);
  useEffect(() => {
    render();
  }, [render]);
  const setCanvas = useCallback((node) => {
    canvasRef.current = node;
    if (node) render(node);
  }, [render]);
  return { canvasRef: setCanvas, ...state, render };
}
function useInView(options = {}) {
  const { rootMargin = "400px 0px", root = null } = options;
  const [visible, setVisible] = useState(false);
  const [node, setNode] = useState(null);
  const setRef = useCallback((el) => {
    setNode(el);
  }, []);
  useEffect(() => {
    if (!node) return void 0;
    if (typeof IntersectionObserver === "undefined") {
      setVisible(true);
      return void 0;
    }
    const observer = new IntersectionObserver(
      ([entry]) => setVisible(entry.isIntersecting),
      { root, rootMargin }
    );
    observer.observe(node);
    return () => observer.disconnect();
  }, [node, root, rootMargin]);
  return [setRef, visible];
}
export {
  useInView,
  usePdfDocument,
  usePdfPage
};
