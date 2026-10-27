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

/** A non-link annotation: a note, a markup mark, a stamp or a widget. */
export interface Annot {
  /** /Subtype without the slash: `Text`, `Highlight`, `Stamp`, `Widget`, ... */
  sub: string;
  /** /Rect in page space (y up, origin bottom-left). */
  x0: number; y0: number; x1: number; y1: number;
  /** /Contents — the comment text. Absent when the annotation carries none. */
  contents?: string;
  /** /T — the comment's author. */
  author?: string;
  /** /Subj — the subject line. */
  subject?: string;
  /** /Name — the icon name for notes and the stamp name for stamps. */
  name?: string;
  /** /M — the modification date, as written by the producer. */
  date?: string;
  /** /C as RGB, 0..1 per component. */
  color?: [number, number, number];
  /** The /Popup rectangle, when the annotation has an openable popup. */
  popup?: [number, number, number, number];
}

export interface ModuleOptions extends WorkerOptions {
  /** Keep rendered page bitmaps in a byte-budgeted LRU. Default true. */
  pageCache?: boolean;
  /** Cache budget in bytes (default: from navigator.deviceMemory, 8–96 MB). */
  pageCacheBytes?: number;
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

export interface WorkerOptions {
  /**
   * Parse and rasterize in a Web Worker. `true` falls back to the inline path
   * when no worker can start (the reason lands on `document.workerError`);
   * `'strict'` rejects instead.
   */
  worker?: boolean | 'strict';
  /** URL of dist/worker.js. Required when the library is bundled — the default
   *  resolves against import.meta.url, which is the page in a bundle. */
  workerUrl?: string;
  /** Build the worker yourself (different URL, blob worker, instrumentation). */
  workerFactory?: (options: ModuleOptions) => Worker;
}

export interface OpenOptions extends ModuleOptions, WorkerOptions {
  filename?: string;
  password?: string;
}

/**
 * A document the engine reads on demand instead of copying into wasm (M9.2).
 * Requires `{ worker: true }`: the reads are synchronous.
 */
export interface RangeSpec {
  /** HTTP(S) URL serving `Range: bytes=` requests (a 200 is also handled). */
  url?: string;
  /** Or a Blob/File to slice locally in the worker. */
  blob?: Blob;
  /** Total size in bytes. Required for `url`; taken from `blob` otherwise. */
  size?: number;
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
  /** Inline: the wasm engine runs on the calling thread. */
  static open(
    source: DocumentSource,
    options?: OpenOptions & { worker?: false | undefined },
  ): Promise<PdfDocument>;
  /** Worker mode: the engine runs off the main thread (see PdfRemoteDocument). */
  static open(
    source: DocumentSource | RangeSpec,
    options: OpenOptions & { worker: true | 'strict' },
  ): Promise<PdfRemoteDocument>;

  /** Parse straight out of Range requests / Blob slices — no copy into wasm.
   *  Worker-only, because the parser's reads are synchronous. */
  static openRanged(spec: RangeSpec, options?: OpenOptions): Promise<PdfDocument>;

  /** Which path this document runs on. */
  readonly mode: 'inline';
  readonly isWorker: false;
  /** Why `{ worker: true }` fell back to the inline path, or null. */
  readonly workerError: Error | null;

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

  /** Notes, markup, stamps and widgets on a page — `getLinks` covers /Link. */
  getAnnots(index: number): Annot[];

  getText(index: number): TextItem[];
  /** Async form of getText, so one call site works on both document kinds. */
  loadText(index: number): Promise<TextItem[]>;
  /** Async form of `getLinks`. */
  loadLinks(index: number): Promise<LinkAnnotation[]>;
  /** Async form of `getAnnots`. */
  loadAnnots(index: number): Promise<Annot[]>;

  /** Bytes the parser actually pulled (ranged sources) and the window size. */
  readonly sourceStats: {
    fetched: number; fetches: number; windowBytes: number;
  } | null;

  /** Page-cache counters, or null when the cache is disabled. */
  readonly cacheStats: {
    entries: number; bytes: number; budget: number;
    hits: number; misses: number; evictions: number;
  } | null;

  /** Keep this page resident across cache eviction (the one on screen). */
  pinPage(index: number, scale?: number, dpr?: number): void;

  /** Drop cached bitmaps (after a theme change, or under memory pressure). */
  clearPageCache(): void;
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

/**
 * A document whose wasm instance lives in a worker (`open(file, { worker: true })`).
 *
 * The synchronous part of `PdfDocument` survives because page geometry,
 * metadata, the outline, form fields and warnings are captured once at open
 * time. Text and links for a page are filled by the render that drew it, so the
 * cached getters are warm in the viewer; use the async loaders otherwise.
 */
export interface PdfRemoteDocument {
  readonly mode: 'worker';
  readonly isWorker: true;
  readonly pageCount: number;
  readonly metadata: Record<string, string>;
  readonly warnings: string[];
  readonly outline: { title: string; page: number }[];
  readonly form: { name: string; type: string; value: string; page: number; rect: any }[];
  filename: string;

  getPageSize(index: number): PageSize;
  render(index: number, options?: RenderOptions): Promise<RenderResult>;
  renderToCanvas(
    canvas: HTMLCanvasElement,
    index: number,
    options?: RenderOptions & { cssScale?: boolean },
  ): Promise<RenderResult>;

  /** Bytes the parser actually pulled, and the worker's wasm heap. */
  stats(): Promise<{ source: { fetched: number; fetches: number; windowBytes: number } | null; heapBytes: number }>;
  /** Page-cache counters. */
  readonly cacheStats: {
    entries: number; bytes: number; budget: number;
    hits: number; misses: number; evictions: number;
  };
  pinPage(index: number, scale?: number, dpr?: number): void;
  clearPageCache(): void;
  /** True when the file was never copied into wasm (ranged source). */
  readonly ranged: boolean;

  /** Cached runs/links for a page that has been rendered; [] otherwise. */
  getText(index: number): TextItem[];
  getLinks(index: number): LinkAnnotation[];
  loadText(index: number): Promise<TextItem[]>;
  loadLinks(index: number): Promise<LinkAnnotation[]>;
  getAnnots(index: number): Annot[];
  loadAnnots(index: number): Promise<Annot[]>;
  getPageText(index: number, options?: { lineSeparator?: string }): Promise<string>;
  getAllText(options?: { lineSeparator?: string }): Promise<string[]>;

  search(query: string, options?: {
    caseSensitive?: boolean;
    limit?: number;
    pages?: number[];
  }): Promise<SearchHit[]>;

  setField(name: string, value: string): Promise<boolean>;

  boxToCss(
    box: { x0: number; y0: number; x1: number; y1: number },
    index: number,
    scale: number,
  ): { left: number; top: number; width: number; height: number };

  /** Terminates the worker when this was the last document it served. */
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
  /** Rotation buttons: clockwise and counter-clockwise (both 90° steps). */
  rotateLeft?: string;
  rotateRight?: string;
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
  /** View rotation in degrees, 0/90/180/270, on top of each page's own /Rotate. */
  rotation: number;
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
  /** Initial view rotation in degrees (0/90/180/270). Default 0. */
  rotation?: number;
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
  /** Parse and rasterize in a Web Worker. Both demos run this path. */
  worker?: boolean | 'strict';
  /** URL of dist/worker.js; required when your app bundles the library. */
  workerUrl?: string;
  workerFactory?: (options: ModuleOptions) => Worker;
  highlights?: (page: number, scale: number) => SearchHit[];
  renderError?: (error: Error) => Node;
  onReady?: (info: { pageCount: number; metadata: Record<string, string>; document: PdfDocument }) => void;
  onPageChange?: (page: number) => void;
  onScaleChange?: (scale: number) => void;
  onRotationChange?: (rotation: number) => void;
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
  /** Current view rotation in degrees (0/90/180/270). */
  readonly rotation: number;
  setRotation(rotation: number): void;
  rotateLeft(): void;
  rotateRight(): void;
  setFile(source: DocumentSource, options?: { page?: number; filename?: string }): Promise<void>;
  setTheme(theme: Record<string, string | number>): void;
  setInverted(on: boolean): void;
  /** A Promise on a worker-backed document (open with `worker: true`). */
  search(query: string, options?: { caseSensitive?: boolean; limit?: number }): SearchHit[] | Promise<SearchHit[]>;
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
