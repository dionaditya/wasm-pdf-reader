import { jsx, jsxs } from "react/jsx-runtime";
import React, { useEffect, useRef } from "react";
import { usePdfPage, useInView } from "./hooks.js";
import { buildTextLayer } from "../text.js";
import { turnedPageBox } from "../rotation.js";
function PdfPage(props) {
  const {
    document: doc,
    page = 0,
    scale = 1.5,
    rotation = 0,
    dpr,
    pixelTransform,
    renderer,
    lazy = false,
    textLayer = true,
    children,
    onRender,
    onError,
    className = "",
    canvasClassName = "",
    style,
    placeholder,
    ...rest
  } = props;
  const [inViewRef, visible] = useInView();
  const { canvasRef, rendering, error, cssWidth, cssHeight, stats } = usePdfPage(doc, page, {
    scale,
    dpr,
    pixelTransform,
    enabled: !lazy || visible,
    renderer
  });
  const size = doc ? doc.getPageSize(page) : { width: 612, height: 792 };
  const innerWidth = cssWidth || size.width * scale;
  const innerHeight = cssHeight || size.height * scale;
  const box = turnedPageBox({ width: innerWidth, height: innerHeight }, 1, rotation);
  const width = box.width;
  const height = box.height;
  const textRef = useRef(null);
  useEffect(() => {
    const host = textRef.current;
    if (!host) return void 0;
    host.replaceChildren();
    if (!doc || textLayer === false) return void 0;
    const size2 = doc.getPageSize(page);
    const paint = (items) => {
      if (!items || !items.length) return;
      host.replaceChildren(buildTextLayer(items, {
        scale,
        height: size2.height,
        boxY0: size2.boxY0
      }));
    };
    let alive = true;
    let sync = null;
    try {
      sync = doc.getText(page);
    } catch {
      sync = null;
    }
    if (sync && sync.length) paint(sync);
    else if (typeof doc.loadText === "function") {
      doc.loadText(page).then((items) => {
        if (alive) paint(items);
      }).catch(() => {
      });
    }
    return () => {
      alive = false;
    };
  }, [doc, page, scale, textLayer, width, height]);
  const reported = useRef(null);
  useEffect(() => {
    if (error) onError?.(error);
  }, [error, onError]);
  useEffect(() => {
    if (stats && reported.current !== `${page}:${scale}`) {
      reported.current = `${page}:${scale}`;
      onRender?.({ page, scale, stats });
    }
  }, [stats, page, scale, onRender]);
  return /* @__PURE__ */ jsxs(
    "div",
    {
      ref: lazy ? inViewRef : void 0,
      className: `wpdf-page ${className}`.trim(),
      style: { width: `${width}px`, height: `${height}px`, ...style },
      "data-page": page,
      ...rest,
      children: [
        /* @__PURE__ */ jsxs(
          "div",
          {
            className: "wpdf-page__turn",
            style: {
              width: `${innerWidth}px`,
              height: `${innerHeight}px`,
              transform: box.transform
            },
            "data-rotation": box.rotation,
            children: [
              /* @__PURE__ */ jsx(
                "canvas",
                {
                  ref: canvasRef,
                  className: `wpdf-page__canvas ${canvasClassName}`.trim(),
                  style: { width: `${innerWidth}px`, height: `${innerHeight}px` },
                  "aria-label": `Page ${page + 1}`,
                  role: "img"
                }
              ),
              /* @__PURE__ */ jsx("div", { className: "wpdf-page__text", ref: textRef, "aria-hidden": textLayer === false }),
              children && /* @__PURE__ */ jsx("div", { className: "wpdf-page__overlay", children })
            ]
          }
        ),
        !stats && !error && (placeholder || (rendering ? null : null)),
        error && /* @__PURE__ */ jsx("div", { className: "wpdf-page__placeholder", style: { position: "absolute", inset: 0 }, children: error.message })
      ]
    }
  );
}
var PdfPage_default = PdfPage;
export {
  PdfPage,
  PdfPage_default as default
};
