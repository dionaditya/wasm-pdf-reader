// PdfToolbar — the default chrome. Replacement, not requirement: pass
// `toolbar={false}` and your own buttons, or `toolbar={<MyBar/>}`.
//
// It is a controlled component: page and scale come in as props and leave
// through callbacks, so it can be reused with any renderer.

import React, { useEffect, useState } from 'react';

export const toolbarLabels = {
  previousPage: 'Previous page',
  nextPage: 'Next page',
  zoomIn: 'Zoom in',
  zoomOut: 'Zoom out',
  fitWidth: 'Fit width',
  fitPage: 'Fit page',
  page: 'Page',
};

/**
 * @param {Object} props
 * @param {number} props.page
 * @param {number} props.pageCount
 * @param {number} props.scale
 * @param {(page:number)=>void} [props.onPageChange]
 * @param {(scale:number)=>void} [props.onZoom]
 * @param {(fit:'width'|'page')=>void} [props.onFit]
 * @param {string} [props.title]
 * @param {Object} [props.labels]
 * @param {React.ReactNode} [props.children]  extra tools, before the spacer
 * @param {React.ReactNode} [props.tools]     extra tools, after the zoom group
 * @param {boolean} [props.showPageNav=true]
 * @param {boolean} [props.showZoom=true]
 * @param {string} [props.className]
 */
export function PdfToolbar(props) {
  const {
    page, pageCount, scale,
    onPageChange, onZoom, onFit,
    title = '', labels = toolbarLabels, children, tools,
    showPageNav = true, showZoom = true, className = '',
  } = props;

  const [draft, setDraft] = useState(String(page + 1));
  useEffect(() => setDraft(String(page + 1)), [page]);

  const commit = () => {
    const n = parseInt(draft, 10);
    if (Number.isFinite(n)) onPageChange?.(Math.min(pageCount, Math.max(1, n)) - 1);
    else setDraft(String(page + 1));
  };

  return (
    <div className={`wpdf-toolbar ${className}`.trim()} role="toolbar">
      {title ? <span className="wpdf-toolbar__title" title={title}>{title}</span> : null}
      {showPageNav && (
        <div className="wpdf-toolbar__group">
          <button type="button" className="wpdf-button" title={labels.previousPage}
                  aria-label={labels.previousPage} disabled={page <= 0}
                  onClick={() => onPageChange?.(page - 1)}>‹</button>
          <input
            className="wpdf-input"
            value={draft}
            inputMode="numeric"
            aria-label={labels.page}
            onChange={(e) => setDraft(e.target.value.replace(/[^\d]/g, ''))}
            onBlur={commit}
            onKeyDown={(e) => { if (e.key === 'Enter') { e.currentTarget.blur(); } }}
          />
          <span className="wpdf-toolbar__count">/ {pageCount}</span>
          <button type="button" className="wpdf-button" title={labels.nextPage}
                  aria-label={labels.nextPage} disabled={page >= pageCount - 1}
                  onClick={() => onPageChange?.(page + 1)}>›</button>
        </div>
      )}

      <span className="wpdf-toolbar__spacer" />
      {children}

      {showZoom && (
        <div className="wpdf-toolbar__group">
          <button type="button" className="wpdf-button" title={labels.zoomOut}
                  aria-label={labels.zoomOut} onClick={() => onZoom?.(scale / 1.25)}>−</button>
          <span className="wpdf-toolbar__zoom">{Math.round(scale * 100)}%</span>
          <button type="button" className="wpdf-button" title={labels.zoomIn}
                  aria-label={labels.zoomIn} onClick={() => onZoom?.(scale * 1.25)}>+</button>
          <button type="button" className="wpdf-button" title={labels.fitWidth}
                  aria-label={labels.fitWidth} onClick={() => onFit?.('width')}>⇔</button>
          <button type="button" className="wpdf-button" title={labels.fitPage}
                  aria-label={labels.fitPage} onClick={() => onFit?.('page')}>⤢</button>
        </div>
      )}
      {tools}
    </div>
  );
}

export default PdfToolbar;
