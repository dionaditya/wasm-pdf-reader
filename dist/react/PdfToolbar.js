import { jsx, jsxs } from "react/jsx-runtime";
import React, { useEffect, useState } from "react";
const toolbarLabels = {
  previousPage: "Previous page",
  nextPage: "Next page",
  zoomIn: "Zoom in",
  zoomOut: "Zoom out",
  fitWidth: "Fit width",
  fitPage: "Fit page",
  rotateLeft: "Rotate counter-clockwise",
  rotateRight: "Rotate clockwise",
  page: "Page"
};
function PdfToolbar(props) {
  const {
    page,
    pageCount,
    scale,
    onPageChange,
    onZoom,
    onFit,
    onRotate,
    title = "",
    labels = toolbarLabels,
    children,
    tools,
    showPageNav = true,
    showZoom = true,
    showRotate = true,
    className = ""
  } = props;
  const [draft, setDraft] = useState(String(page + 1));
  useEffect(() => setDraft(String(page + 1)), [page]);
  const commit = () => {
    const n = parseInt(draft, 10);
    if (Number.isFinite(n)) onPageChange?.(Math.min(pageCount, Math.max(1, n)) - 1);
    else setDraft(String(page + 1));
  };
  return /* @__PURE__ */ jsxs("div", { className: `wpdf-toolbar ${className}`.trim(), role: "toolbar", children: [
    title ? /* @__PURE__ */ jsx("span", { className: "wpdf-toolbar__title", title, children: title }) : null,
    showPageNav && /* @__PURE__ */ jsxs("div", { className: "wpdf-toolbar__group", children: [
      /* @__PURE__ */ jsx(
        "button",
        {
          type: "button",
          className: "wpdf-button",
          title: labels.previousPage,
          "aria-label": labels.previousPage,
          disabled: page <= 0,
          onClick: () => onPageChange?.(page - 1),
          children: "\u2039"
        }
      ),
      /* @__PURE__ */ jsx(
        "input",
        {
          className: "wpdf-input",
          value: draft,
          inputMode: "numeric",
          "aria-label": labels.page,
          onChange: (e) => setDraft(e.target.value.replace(/[^\d]/g, "")),
          onBlur: commit,
          onKeyDown: (e) => {
            if (e.key === "Enter") {
              e.currentTarget.blur();
            }
          }
        }
      ),
      /* @__PURE__ */ jsxs("span", { className: "wpdf-toolbar__count", children: [
        "/ ",
        pageCount
      ] }),
      /* @__PURE__ */ jsx(
        "button",
        {
          type: "button",
          className: "wpdf-button",
          title: labels.nextPage,
          "aria-label": labels.nextPage,
          disabled: page >= pageCount - 1,
          onClick: () => onPageChange?.(page + 1),
          children: "\u203A"
        }
      )
    ] }),
    /* @__PURE__ */ jsx("span", { className: "wpdf-toolbar__spacer" }),
    children,
    showZoom && /* @__PURE__ */ jsxs("div", { className: "wpdf-toolbar__group", children: [
      /* @__PURE__ */ jsx(
        "button",
        {
          type: "button",
          className: "wpdf-button",
          title: labels.zoomOut,
          "aria-label": labels.zoomOut,
          onClick: () => onZoom?.(scale / 1.25),
          children: "\u2212"
        }
      ),
      /* @__PURE__ */ jsxs("span", { className: "wpdf-toolbar__zoom", children: [
        Math.round(scale * 100),
        "%"
      ] }),
      /* @__PURE__ */ jsx(
        "button",
        {
          type: "button",
          className: "wpdf-button",
          title: labels.zoomIn,
          "aria-label": labels.zoomIn,
          onClick: () => onZoom?.(scale * 1.25),
          children: "+"
        }
      ),
      /* @__PURE__ */ jsx(
        "button",
        {
          type: "button",
          className: "wpdf-button",
          title: labels.fitWidth,
          "aria-label": labels.fitWidth,
          onClick: () => onFit?.("width"),
          children: "\u21D4"
        }
      ),
      /* @__PURE__ */ jsx(
        "button",
        {
          type: "button",
          className: "wpdf-button",
          title: labels.fitPage,
          "aria-label": labels.fitPage,
          onClick: () => onFit?.("page"),
          children: "\u2922"
        }
      )
    ] }),
    showRotate && /* @__PURE__ */ jsxs("div", { className: "wpdf-toolbar__group", children: [
      /* @__PURE__ */ jsx(
        "button",
        {
          type: "button",
          className: "wpdf-button",
          title: labels.rotateLeft,
          "aria-label": labels.rotateLeft,
          onClick: () => onRotate?.(-1),
          children: "\u27F2"
        }
      ),
      /* @__PURE__ */ jsx(
        "button",
        {
          type: "button",
          className: "wpdf-button",
          title: labels.rotateRight,
          "aria-label": labels.rotateRight,
          onClick: () => onRotate?.(1),
          children: "\u27F3"
        }
      )
    ] }),
    tools
  ] });
}
var PdfToolbar_default = PdfToolbar;
export {
  PdfToolbar,
  PdfToolbar_default as default,
  toolbarLabels
};
