// PdfPage — one page as a canvas, with room for overlays.
//
// Small on purpose: it renders pixel data and nothing else, so it can be
// dropped into any layout. Everything visual comes from `className`/`style`
// and the CSS custom properties in styles.css.

import React, { useEffect, useRef } from 'react';
import { usePdfPage, useInView } from './hooks.js';
import { buildTextLayer } from '../text.js';
import { turnedPageBox } from '../rotation.js';

/**
 * @param {Object} props
 * @param {import('../document.js').PdfDocument|null} props.document
 * @param {number} props.page zero-based
 * @param {number} [props.scale=1.5] CSS px per point
 * @param {0|90|180|270} [props.rotation=0] view rotation, on top of the page's own /Rotate
 * @param {number} [props.dpr] defaults to window.devicePixelRatio
 * @param {Function} [props.pixelTransform] RGBA post-processing hook
 * @param {boolean} [props.lazy=false] render only when scrolled into view
 * @param {React.ReactNode} [props.children] overlay content (positioned over the page)
 * @param {(info:Object) => void} [props.onRender]
 * @param {(err:Error) => void} [props.onError]
 * @param {string} [props.className]
 * @param {string} [props.canvasClassName]
 * @param {React.CSSProperties} [props.style]
 * @param {React.ReactNode} [props.placeholder] shown before the bitmap lands
 */
export function PdfPage(props) {
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
    className = '',
    canvasClassName = '',
    style,
    placeholder,
    ...rest
  } = props;

  const [inViewRef, visible] = useInView();
  const { canvasRef, rendering, error, cssWidth, cssHeight, stats } = usePdfPage(doc, page, {
    scale, dpr, pixelTransform, enabled: !lazy || visible, renderer,
  });

  const size = doc ? doc.getPageSize(page) : { width: 612, height: 792 };
  const innerWidth = cssWidth || size.width * scale;
  const innerHeight = cssHeight || size.height * scale;
  // The bitmap is never re-rendered for a turn: the box around it is swapped and
  // the layer holding canvas + text + overlays is rotated onto it.
  const box = turnedPageBox({ width: innerWidth, height: innerHeight }, 1, rotation);
  const width = box.width;
  const height = box.height;

  const textRef = useRef(null);
  useEffect(() => {
    const host = textRef.current;
    if (!host) return undefined;
    host.replaceChildren();
    if (!doc || textLayer === false) return undefined;
    const size = doc.getPageSize(page);
    const paint = (items) => {
      if (!items || !items.length) return;
      host.replaceChildren(buildTextLayer(items, {
        scale, height: size.height, boxY0: size.boxY0,
      }));
    };
    // A worker-backed document fills its text cache with the render that drew
    // the page, so this is usually immediate; loadText covers the case where
    // this effect runs first (mount, or a cached bitmap).
    let alive = true;
    let sync = null;
    try { sync = doc.getText(page); } catch { sync = null; }
    if (sync && sync.length) paint(sync);
    else if (typeof doc.loadText === 'function') {
      doc.loadText(page).then((items) => { if (alive) paint(items); }).catch(() => {});
    }
    return () => { alive = false; };
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

  return (
    <div
      ref={lazy ? inViewRef : undefined}
      className={`wpdf-page ${className}`.trim()}
      style={{ width: `${width}px`, height: `${height}px`, ...style }}
      data-page={page}
      {...rest}
    >
      <div
        className="wpdf-page__turn"
        style={{
          width: `${innerWidth}px`,
          height: `${innerHeight}px`,
          transform: box.transform,
        }}
        data-rotation={box.rotation}
      >
        <canvas
          ref={canvasRef}
          className={`wpdf-page__canvas ${canvasClassName}`.trim()}
          style={{ width: `${innerWidth}px`, height: `${innerHeight}px` }}
          aria-label={`Page ${page + 1}`}
          role="img"
        />
        <div className="wpdf-page__text" ref={textRef} aria-hidden={textLayer === false} />
        {children && <div className="wpdf-page__overlay">{children}</div>}
      </div>
      {!stats && !error && (placeholder || (rendering ? null : null))}
      {error && (
        <div className="wpdf-page__placeholder" style={{ position: 'absolute', inset: 0 }}>
          {error.message}
        </div>
      )}
    </div>
  );
}

export default PdfPage;
