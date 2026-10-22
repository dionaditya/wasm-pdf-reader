import { jsx, jsxs } from "react/jsx-runtime";
import React, {
  forwardRef,
  useCallback,
  useEffect,
  useImperativeHandle,
  useMemo,
  useRef,
  useState
} from "react";
import { usePdfDocument } from "./hooks.js";
import { PdfPage } from "./PdfPage.js";
import { PdfToolbar } from "./PdfToolbar.js";
const PAD = 32;
function themeVars(theme) {
  if (!theme) return void 0;
  const style = {};
  for (const [key, value] of Object.entries(theme)) {
    if (value == null) continue;
    const name = key.replace(/[A-Z]/g, (m) => `-${m.toLowerCase()}`);
    style[`--wpdf-${name}`] = String(value);
  }
  return style;
}
const PdfReader = forwardRef(function PdfReader2(props, ref) {
  const {
    file,
    page: pageProp,
    defaultPage = 0,
    onPageChange,
    scale: scaleProp,
    defaultScale = 1.5,
    fit: fitProp,
    onZoom,
    continuous = false,
    toolbar = true,
    components = {},
    theme,
    labels,
    className = "",
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
  const [fit, setFit] = useState(fitProp ?? "auto");
  const [manualFit, setManualFit] = useState(false);
  const [viewportBox, setViewportBox] = useState({ width: 0, height: 0 });
  const [lastStats, setLastStats] = useState(null);
  const viewportRef = useRef(null);
  const rootRef = useRef(null);
  const { document: doc, status, error, pageCount, metadata, warnings, reload } = usePdfDocument(
    file,
    { wasmUrl, wasmBinary, module, filename: file?.name }
  );
  const page = pageProp !== void 0 ? pageProp : innerPage;
  const controlledScale = scaleProp !== void 0 ? scaleProp : innerScale;
  const activeFit = manualFit ? fit : fitProp !== void 0 ? fitProp : "auto";
  useEffect(() => {
    const el = viewportRef.current;
    if (!el || typeof ResizeObserver === "undefined") return void 0;
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
    const fitMode = activeFit === void 0 || activeFit === null ? "none" : activeFit;
    if (fitMode === "none") return controlledScale;
    const availW = Math.max(120, (viewportBox.width || 800) - PAD);
    const availH = Math.max(120, (viewportBox.height || 800) - PAD);
    const s = fitMode === "page" ? Math.min(availW / size.width, availH / size.height) : availW / size.width;
    return Math.max(0.1, Math.min(8, s || controlledScale));
  }, [doc, pageCount, page, controlledScale, activeFit, viewportBox]);
  useEffect(() => {
    onLoad?.({ document: doc, pageCount, metadata, warnings });
  }, [doc, pageCount, metadata, warnings, onLoad]);
  useEffect(() => {
    if (error) onError?.(error);
  }, [error, onError]);
  useEffect(() => {
    if (pageCount && page > pageCount - 1) setInnerPage(pageCount - 1);
  }, [pageCount, page]);
  const goToPage = useCallback((next) => {
    const clamped = Math.max(0, Math.min((pageCount || 1) - 1, Math.round(next)));
    if (pageProp === void 0) setInnerPage(clamped);
    onPageChange?.(clamped);
    if (continuous && viewportRef.current) {
      const target = viewportRef.current.querySelector(`[data-page="${clamped}"]`);
      target?.scrollIntoView({ block: "start" });
    }
  }, [pageCount, pageProp, onPageChange, continuous]);
  const setScale = useCallback((next) => {
    const clamped = Math.max(0.1, Math.min(8, Number(next) || 1));
    setManualFit(true);
    setFit("none");
    if (scaleProp === void 0) setInnerScale(clamped);
    onZoom?.(clamped);
  }, [scaleProp, onZoom]);
  const fitTo = useCallback((mode) => {
    setManualFit(false);
    setFit(mode);
    onZoom?.(effectiveScale);
  }, [effectiveScale, onZoom]);
  useImperativeHandle(ref, () => ({
    get document() {
      return doc;
    },
    get page() {
      return page;
    },
    get scale() {
      return effectiveScale;
    },
    pageCount,
    goToPage,
    nextPage: () => goToPage(page + 1),
    prevPage: () => goToPage(page - 1),
    setScale,
    zoomIn: () => setScale(effectiveScale * 1.25),
    zoomOut: () => setScale(effectiveScale / 1.25),
    fitWidth: () => fitTo("width"),
    fitPage: () => fitTo("page"),
    reload,
    /** Plain text for a page (extracts and caches it in wasm). */
    pageText: (i = page) => doc?.getPageText(i) ?? "",
    /** All matches for a query: {page, index, length, line, rects}. */
    search: (q, o) => doc?.search(q, o) ?? []
  }), [doc, page, effectiveScale, pageCount, goToPage, setScale, fitTo, reload]);
  const onKeyDown = useCallback((ev) => {
    const held = ev.ctrlKey || ev.metaKey;
    const keys = {
      ArrowRight: () => goToPage(page + 1),
      PageDown: () => goToPage(page + 1),
      ArrowLeft: () => goToPage(page - 1),
      PageUp: () => goToPage(page - 1),
      Home: () => goToPage(0),
      End: () => goToPage(pageCount - 1)
    };
    if (keys[ev.key]) {
      ev.preventDefault();
      keys[ev.key]();
      return;
    }
    if (held && (ev.key === "+" || ev.key === "=")) {
      ev.preventDefault();
      setScale(effectiveScale * 1.25);
    } else if (held && ev.key === "-") {
      ev.preventDefault();
      setScale(effectiveScale / 1.25);
    } else if (held && ev.key === "0") {
      ev.preventDefault();
      fitTo("width");
    }
  }, [goToPage, page, pageCount, setScale, effectiveScale, fitTo]);
  const searchHits = useMemo(() => {
    if (!doc || !search) return [];
    try {
      return doc.search(search);
    } catch {
      return [];
    }
  }, [doc, search]);
  const overlayFor = useCallback((index) => {
    const parts = [];
    const rects = [];
    const links = doc ? doc.getLinks(index) : [];
    if (links.length) {
      parts.push(links.map((link, i) => {
        const css = doc.boxToCss(link, index, effectiveScale);
        const external = !!link.uri;
        return /* @__PURE__ */ jsx(
          "a",
          {
            className: "wpdf-link",
            href: external ? link.uri : `#page-${link.page + 1}`,
            target: external ? "_blank" : void 0,
            rel: external ? "noopener noreferrer" : void 0,
            title: external ? link.uri : `Go to page ${link.page + 1}`,
            style: { left: css.left, top: css.top, width: css.width, height: css.height },
            onClick: (ev) => {
              const handled = onLink?.(link, { document: doc, goToPage, pageCount });
              if (handled === false) return;
              if (!external && link.page >= 0) {
                ev.preventDefault();
                goToPage(link.page);
              }
            }
          },
          `ln-${i}`
        );
      }));
    }
    if (searchHits.length) {
      for (const hit of searchHits) if (hit.page === index) rects.push(...hit.rects);
    }
    const custom = typeof highlights === "function" ? highlights(index, effectiveScale) : highlights;
    if (Array.isArray(custom)) {
      for (const hit of custom) {
        if (hit.page !== void 0 && hit.page !== index) continue;
        rects.push(...hit.rects || [hit]);
      }
    }
    if (rects.length && doc) {
      parts.push(rects.map((rect, i) => {
        const css = doc.boxToCss(rect, index, effectiveScale);
        return /* @__PURE__ */ jsx(
          "div",
          {
            className: "wpdf-highlight",
            style: { left: css.left, top: css.top, width: css.width, height: css.height }
          },
          `hl-${i}`
        );
      }));
    }
    if (pageOverlay) parts.push(pageOverlay(index, { document: doc, scale: effectiveScale, pageCount }));
    return parts;
  }, [searchHits, highlights, pageOverlay, onLink, doc, effectiveScale, goToPage, pageCount]);
  const pageSize = doc && pageCount ? doc.getPageSize(Math.min(page, pageCount - 1)) : null;
  const titleText = title || metadata?.Title || (typeof file === "object" && file ? file.name : "") || "";
  const toolbarProps = {
    page,
    pageCount,
    scale: effectiveScale,
    onPageChange: goToPage,
    onZoom: setScale,
    onFit: fitTo,
    labels,
    title: toolbar === true ? titleText : void 0
  };
  let toolbarNode = null;
  if (toolbar === true) toolbarNode = /* @__PURE__ */ jsx(Toolbar, { ...toolbarProps });
  else if (typeof toolbar === "function") toolbarNode = toolbar(toolbarProps);
  else if (React.isValidElement(toolbar)) toolbarNode = toolbar;
  let overlayNode = null;
  if (status === "loading") {
    overlayNode = renderLoading ? renderLoading() : /* @__PURE__ */ jsxs("div", { children: [
      /* @__PURE__ */ jsx("span", { className: "wpdf-spinner" }),
      "Loading document\u2026"
    ] });
  } else if (status === "error") {
    overlayNode = renderError ? renderError(error) : /* @__PURE__ */ jsxs("div", { className: "wpdf-overlay--error", children: [
      "Could not open this PDF \u2014 ",
      error?.message
    ] });
  } else if (status === "idle") {
    overlayNode = renderEmpty ? renderEmpty() : /* @__PURE__ */ jsx("div", { children: "Drop a PDF here, or pass a `file` prop" });
  }
  const pageNode = (index, lazy) => /* @__PURE__ */ jsx(
    Page,
    {
      document: doc,
      page: index,
      scale: effectiveScale,
      dpr,
      pixelTransform,
      renderer,
      lazy,
      onRender: (info) => {
        if (index === page) setLastStats(info.stats);
        onRender?.(info);
      },
      onError,
      children: overlayFor(index)
    },
    index
  );
  return /* @__PURE__ */ jsxs(
    "div",
    {
      ref: rootRef,
      className: `wpdf-root ${invert ? "wpdf-root--invert" : ""} ${className}`.trim(),
      style: { ...themeVars(theme), ...style },
      tabIndex: 0,
      onKeyDown,
      "data-status": status,
      ...rest,
      children: [
        toolbarNode,
        /* @__PURE__ */ jsx(
          "div",
          {
            ref: viewportRef,
            className: `wpdf-viewport ${continuous ? "wpdf-viewport--continuous" : ""}`.trim(),
            children: status === "ready" && doc ? continuous ? Array.from({ length: pageCount }, (_, i) => pageNode(i, true)) : pageNode(Math.min(page, pageCount - 1), false) : null
          }
        ),
        showStatus && /* @__PURE__ */ jsxs("div", { className: "wpdf-status", children: [
          /* @__PURE__ */ jsx("span", { children: pageCount ? `Page ${page + 1} of ${pageCount}` : "\u2014" }),
          pageSize && /* @__PURE__ */ jsxs("span", { children: [
            Math.round(pageSize.width),
            " \xD7 ",
            Math.round(pageSize.height),
            " pt"
          ] }),
          /* @__PURE__ */ jsxs("span", { children: [
            Math.round(effectiveScale * 150),
            " dpi"
          ] }),
          lastStats && /* @__PURE__ */ jsxs("span", { children: [
            lastStats.glyphs,
            " glyphs"
          ] }),
          warnings.length > 0 && /* @__PURE__ */ jsxs("span", { children: [
            warnings.length,
            " warning",
            warnings.length === 1 ? "" : "s"
          ] }),
          /* @__PURE__ */ jsx("span", { className: "wpdf-toolbar__spacer" }),
          titleText && /* @__PURE__ */ jsx("span", { title: titleText, children: titleText })
        ] }),
        overlayNode && /* @__PURE__ */ jsx("div", { className: "wpdf-overlay", children: overlayNode }),
        children
      ]
    }
  );
});
var PdfReader_default = PdfReader;
export {
  PdfReader,
  PdfReader_default as default
};
