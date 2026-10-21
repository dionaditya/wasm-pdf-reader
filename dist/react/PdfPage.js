import { jsx, jsxs } from "react/jsx-runtime";
import React, { useEffect, useRef } from "react";
import { usePdfPage, useInView } from "./hooks.js";
function PdfPage(props) {
  const {
    document: doc,
    page = 0,
    scale = 1.5,
    dpr,
    pixelTransform,
    renderer,
    lazy = false,
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
  const width = cssWidth || size.width * scale;
  const height = cssHeight || size.height * scale;
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
        /* @__PURE__ */ jsx(
          "canvas",
          {
            ref: canvasRef,
            className: `wpdf-page__canvas ${canvasClassName}`.trim(),
            "aria-label": `Page ${page + 1}`,
            role: "img"
          }
        ),
        !stats && !error && (placeholder || (rendering ? null : null)),
        error && /* @__PURE__ */ jsx("div", { className: "wpdf-page__placeholder", style: { position: "absolute", inset: 0 }, children: error.message }),
        children && /* @__PURE__ */ jsx("div", { className: "wpdf-page__overlay", children })
      ]
    }
  );
}
var PdfPage_default = PdfPage;
export {
  PdfPage,
  PdfPage_default as default
};
