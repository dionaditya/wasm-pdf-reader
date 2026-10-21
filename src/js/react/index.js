// wasm-pdf-reader/react — React bindings.
//
//   import { PdfReader } from '@dionaditya/wasm-pdf-reader/react';
//   import '@dionaditya/wasm-pdf-reader/styles.css';
//
// Components are optional: the hooks and the core PdfDocument work on their
// own, and the components are just one way to lay them out.

export { PdfReader } from './PdfReader.jsx';
export { PdfPage } from './PdfPage.jsx';
export { PdfToolbar, toolbarLabels } from './PdfToolbar.jsx';
export { usePdfDocument, usePdfPage, useInView } from './hooks.js';

export { PdfDocument } from '../document.js';
export { loadModule, resetModule } from '../loader.js';
export { groupLines, itemsToText, searchItems, boxToCss } from '../text.js';
export { version } from '../index.js';

export { PdfReader as default } from './PdfReader.jsx';
