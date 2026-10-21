// React demo — shows the customization points the components expose:
// a custom toolbar, a themed viewer, a page overlay and the text layer.
//
// Built by `node examples/build-react.mjs` into examples/react/bundle.js.

import React, { useCallback, useRef, useState } from 'react';
import { createRoot } from 'react-dom/client';
import { PdfReader } from '../../dist/react/index.js';

const theme = {
  bg: '#0d1117',
  toolbarBg: '#161b22',
  toolbarFg: '#e6edf3',
  toolbarBorder: '#21262d',
  accent: '#58a6ff',
  muted: '#8b949e',
  radius: '8px',
  pageShadow: '0 12px 32px rgba(0,0,0,.55)',
};

function CustomToolbar({ page, pageCount, scale, onPageChange, onZoom, onFit, onExtract }) {
  return (
    <div className="wpdf-toolbar" role="toolbar">
      <strong style={{ fontWeight: 600, marginRight: 8 }}>Report</strong>
      <div className="wpdf-toolbar__group">
        <button className="wpdf-button" onClick={() => onPageChange(page - 1)} disabled={page <= 0} aria-label="Previous page">‹</button>
        <span style={{ minWidth: 72, textAlign: 'center' }}>{page + 1} / {pageCount}</span>
        <button className="wpdf-button" onClick={() => onPageChange(page + 1)} disabled={page >= pageCount - 1} aria-label="Next page">›</button>
      </div>
      <span className="wpdf-toolbar__spacer" />
      <button className="wpdf-button" onClick={onExtract}>Extract text</button>
      <div className="wpdf-toolbar__group">
        <button className="wpdf-button" onClick={() => onZoom(scale / 1.25)} aria-label="Zoom out">−</button>
        <span style={{ minWidth: 48, textAlign: 'center' }}>{Math.round(scale * 100)}%</span>
        <button className="wpdf-button" onClick={() => onZoom(scale * 1.25)} aria-label="Zoom in">+</button>
        <button className="wpdf-button" onClick={() => onFit('width')}>fit width</button>
        <button className="wpdf-button" onClick={() => onFit('page')}>fit page</button>
      </div>
    </div>
  );
}

function App() {
  const reader = useRef(null);
  const [page, setPage] = useState(0);
  const [text, setText] = useState('');
  const [meta, setMeta] = useState({});

  const extract = useCallback(() => {
    const body = reader.current?.pageText(page) ?? '';
    setText(body.trim());
  }, [page]);

  return (
    <div className="app">
      <div className="reader">
        <PdfReader
          ref={reader}
          file="/test/fixtures/reader-sample.pdf"
          wasmUrl="/dist/wasm/pdf-reader.wasm"
          page={page}
          onPageChange={setPage}
          fit="width"
          theme={theme}
          toolbar={(api) => (
            <CustomToolbar
              page={page}
              pageCount={api.pageCount}
              scale={api.scale}
              onPageChange={setPage}
              onZoom={api.onZoom}
              onFit={api.onFit}
              onExtract={extract}
            />
          )}
          pageOverlay={(index) => (
            index === 0 ? <div className="badge">custom overlay</div> : null
          )}
          onLoad={({ metadata }) => setMeta(metadata || {})}
        />
      </div>

      <aside className="side">
        <h2>Document</h2>
        <dl>
          <dt>title</dt><dd>{meta.Title || '—'}</dd>
          <dt>author</dt><dd>{meta.Author || '—'}</dd>
          <dt>producer</dt><dd>{meta.Producer || '—'}</dd>
        </dl>
        <h2>Text layer</h2>
        <p className="hint">Pressed “Extract text”? The reader returns the page text from wasm.</p>
        <pre>{text || '—'}</pre>
      </aside>
    </div>
  );
}

createRoot(document.getElementById('root')).render(<App />);
