// wasm-pdf-reader — public entry point (framework-agnostic).
//
//   import { PdfDocument, mountPdfReader } from '@dionaditya/wasm-pdf-reader';
//
// React users get the same core plus components from '@dionaditya/wasm-pdf-reader/react'.

export { PdfDocument, toBytes } from './document.js';
export { loadModule, resetModule } from './loader.js';
export { mountPdfReader, defaultLabels, applyTheme } from './mount.js';
export { groupLines, itemsToText, searchItems, boxToCss } from './text.js';

export const version = '0.3.0';
