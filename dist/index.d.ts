// Type definitions for wasm-pdf-reader (core, framework-agnostic).

export interface PageSize {
  /** Display width in points, already swapped for /Rotate 90 and 270. */
  width: number;
  /** Display height in points. */
  height: number;
  /** 0, 90, 180 or 270. */
  rotation: number;
  /** Crop box origin, for positioning an overlay in page space. */
  boxX0: number;
  boxY0: number;
  /** Unrotated media box size in points. */
  mediaWidth: number;
  mediaHeight: number;
}

export interface TextItem {
  text: string;
  x0: number; y0: number;
  x1: number; y1: number;
  ox: number; oy: number;
  adv: number;
  fs: number;
}

export interface TextLine {
  text: string;
  items: TextItem[];
  oy: number;
  x0: number; y0: number; x1: number; y1: number;
  size: number;
}

export interface SearchHit {
  page: number;
  index: number;
  length: number;
  line: string;
  rects: { x0: number; y0: number; x1: number; y1: number }[];
}

/** A /Link annotation: page-space box (y up) plus its target. */
export interface LinkAnnotation {
  x0: number; y0: number; x1: number; y1: number;
  /** External target of a /URI (or /GoToR) action. */
  uri?: string;
  /** Zero-based page index of a /GoTo destination. */
  page?: number;
}

export interface ModuleOptions {
  /** URL of pdf-reader.wasm; defaults to the file next to the JS glue. */
  wasmUrl?: string;
  /** Pre-fetched wasm bytes, for CSP or offline use. */
  wasmBinary?: ArrayBuffer | Uint8Array;
  /** A pre-instantiated emscripten Module to share. */
  module?: any;
  /** Extra fields passed to the emscripten factory. */
  moduleOptions?: Record<string, any>;
  /** A different emscripten factory. */
  factory?: (options: any) => Promise<any>;
}

export type DocumentSource =
  | ArrayBuffer
  | Uint8Array
  | Blob
  | File
  | string
  | URL
  | Response
  | { arrayBuffer(): Promise<ArrayBuffer> };

export interface RenderOptions {
  /** CSS pixels per PDF point (72 = 1 inch). Default 1.5. */
  scale?: number;
  /** Device pixel ratio, multiplies `scale` for the bitmap. Default 1. */
  dpr?: number;
  /** Page index, supplied to `pixelTransform`. */
  page?: number;
  /** Post-process the RGBA bytes in place (dark mode, tinting, watermarks). */
  pixelTransform?: (rgba: Uint8ClampedArray, info: {
    width: number; height: number; page: number; scale: number;
  }) => ImageData | void;
  /** Include render statistics in the result. */
  stats?: boolean;
}

export interface RenderStats {
  ops: number; texts: number; glyphs: number; paths: number;
  fills: number; strokes: number; unsupported: number;
}

export interface RenderResult {
  imageData: ImageData;
  width: number;
  height: number;
  cssWidth: number;
  cssHeight: number;
  scale: number;
  stats?: RenderStats;
}

export declare class PdfDocument {
  static open(source: DocumentSource, options?: ModuleOptions & { filename?: string }): Promise<PdfDocument>;

  readonly pageCount: number;
  readonly metadata: Record<string, string>;
  readonly warnings: string[];
  filename: string;

  getPageSize(index: number): PageSize;
  render(index: number, options?: RenderOptions): Promise<RenderResult>;
  renderToCanvas(
    canvas: HTMLCanvasElement,
    index: number,
    options?: RenderOptions & { cssScale?: boolean },
  ): Promise<RenderResult>;

  /** Link annotations on a page (external URIs and internal page targets). */
  getLinks(index: number): LinkAnnotation[];

  getText(index: number): TextItem[];
  getPageText(index: number, options?: { lineSeparator?: string }): string;
  getAllText(options?: { lineSeparator?: string }): string[];
  search(query: string, options?: {
    caseSensitive?: boolean;
    limit?: number;
    pages?: number[];
  }): SearchHit[];

  boxToCss(
    box: { x0: number; y0: number; x1: number; y1: number },
    index: number,
    scale: number,
  ): { left: number; top: number; width: number; height: number };

  close(): void;
}

export declare function toBytes(source: DocumentSource): Promise<Uint8Array>;
export declare function loadModule(options?: ModuleOptions): Promise<any>;
export declare function resetModule(): void;

export interface ViewerLabels {
  previousPage?: string;
  nextPage?: string;
  zoomIn?: string;
  zoomOut?: string;
  fitWidth?: string;
  fitPage?: string;
  page?: string;
  of?: string;
  loading?: string;
  rendering?: string;
  error?: string;
  empty?: string;
}

export declare const defaultLabels: ViewerLabels;

export interface ViewerState {
  page: number;
  pageCount: number;
  scale: number;
  fit: string | null;
  status: 'idle' | 'loading' | 'ready' | 'error';
  error: Error | null;
  filename: string;
  metadata: Record<string, string>;
}

export interface ViewerOptions {
  file?: DocumentSource;
  page?: number;
  scale?: number;
  fit?: 'none' | 'width' | 'page' | 'auto';
  continuous?: boolean;
  toolbar?: boolean;
  toolbarSlot?: HTMLElement | ((api: { getState(): ViewerState }) => HTMLElement);
  status?: boolean;
  statusSlot?: HTMLElement;
  theme?: Record<string, string | number>;
  labels?: ViewerLabels;
  pixelTransform?: RenderOptions['pixelTransform'];
  invert?: boolean;
  dpr?: number;
  wasmUrl?: string;
  wasmBinary?: ArrayBuffer | Uint8Array;
  module?: any;
  highlights?: (page: number, scale: number) => SearchHit[];
  renderError?: (error: Error) => Node;
  onReady?: (info: { pageCount: number; metadata: Record<string, string>; document: PdfDocument }) => void;
  onPageChange?: (page: number) => void;
  onScaleChange?: (scale: number) => void;
  onRender?: (info: { page: number; scale: number } & RenderResult) => void;
  onError?: (error: Error) => void;
}

export interface ViewerController {
  readonly element: HTMLElement;
  readonly document: PdfDocument | null;
  readonly state: ViewerState;
  goToPage(page: number): void;
  nextPage(): void;
  prevPage(): void;
  setScale(scale: number, fit?: string): void;
  zoomIn(): void;
  zoomOut(): void;
  fitWidth(): void;
  fitPage(): void;
  setFile(source: DocumentSource, options?: { page?: number; filename?: string }): Promise<void>;
  setTheme(theme: Record<string, string | number>): void;
  setInverted(on: boolean): void;
  search(query: string, options?: { caseSensitive?: boolean; limit?: number }): SearchHit[];
  rerender(): void;
  destroy(): void;
}

export declare function mountPdfReader(target: HTMLElement | string, options?: ViewerOptions): ViewerController;
export declare function applyTheme(root: HTMLElement, theme?: Record<string, string | number>): void;

export declare function groupLines(items: TextItem[], options?: { threshold?: number }): TextLine[];
export declare function itemsToText(items: TextItem[], options?: { lineSeparator?: string }): string;
export declare function searchItems(
  items: TextItem[],
  query: string,
  options?: { caseSensitive?: boolean; limit?: number },
): Omit<SearchHit, 'page'>[];
export declare function boxToCss(
  box: { x0: number; y0: number; x1: number; y1: number },
  page: { height: number; boxX0?: number; boxY0?: number; scale: number },
): { left: number; top: number; width: number; height: number };

export declare const version: string;
