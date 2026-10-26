// Type definitions for wasm-pdf-reader/react.

import type * as React from 'react';
import type {
  DocumentSource, ModuleOptions, PdfDocument, RenderOptions, RenderStats,
  SearchHit, TextItem, TextLine, ViewerLabels,
} from '../index.js';

export * from '../index.js';

export interface PdfPageProps {
  document: PdfDocument | null;
  page: number;
  scale?: number;
  /** View rotation in degrees (0/90/180/270), applied on top of the page's /Rotate. */
  rotation?: number;
  dpr?: number;
  pixelTransform?: RenderOptions['pixelTransform'];
  /** Render only when scrolled into view (continuous mode). */
  lazy?: boolean;
  renderer?: { render(doc: PdfDocument, page: number, options: any): Promise<ImageData> };
  onRender?: (info: { page: number; scale: number; stats: RenderStats | null }) => void;
  onError?: (error: Error) => void;
  className?: string;
  canvasClassName?: string;
  style?: React.CSSProperties;
  placeholder?: React.ReactNode;
  children?: React.ReactNode;
}

export declare function PdfPage(props: PdfPageProps): React.ReactElement;

export interface PdfToolbarProps {
  page: number;
  pageCount: number;
  scale: number;
  /** Current view rotation in degrees; the toolbar only reports the buttons. */
  rotation?: number;
  onPageChange?: (page: number) => void;
  onZoom?: (scale: number) => void;
  onFit?: (fit: 'width' | 'page') => void;
  /** Turn the view 90°: 1 = clockwise, -1 = counter-clockwise. */
  onRotate?: (dir: 1 | -1) => void;
  title?: string;
  labels?: ViewerLabels;
  children?: React.ReactNode;
  tools?: React.ReactNode;
  showPageNav?: boolean;
  showZoom?: boolean;
  showRotate?: boolean;
  className?: string;
}

export declare function PdfToolbar(props: PdfToolbarProps): React.ReactElement;
export declare const toolbarLabels: ViewerLabels;

export interface PdfReaderApi {
  readonly document: PdfDocument | null;
  readonly page: number;
  readonly scale: number;
  readonly pageCount: number;
  goToPage(page: number): void;
  nextPage(): void;
  prevPage(): void;
  setScale(scale: number): void;
  zoomIn(): void;
  zoomOut(): void;
  fitWidth(): void;
  fitPage(): void;
  readonly rotation: number;
  setRotation(rotation: number): void;
  rotateLeft(): void;
  rotateRight(): void;
  reload(): void;
  pageText(page?: number): string;
  search(query: string, options?: { caseSensitive?: boolean; limit?: number }): SearchHit[];
}

export interface PdfReaderProps extends ModuleOptions {
  /** Document to display: ArrayBuffer, Blob, File, URL or Response. */
  /* `worker`, `workerUrl` and `workerFactory` come from ModuleOptions: a
     worker-backed reader hands `PdfRemoteDocument` to the callbacks below, whose
     text/search accessors are Promises. */
  file?: DocumentSource | null;
  /** Controlled page (zero-based). */
  page?: number;
  defaultPage?: number;
  onPageChange?: (page: number) => void;
  /** Controlled scale in CSS px per point. */
  scale?: number;
  defaultScale?: number;
  onZoom?: (scale: number) => void;
  /** Initial view rotation in degrees (0/90/180/270). Default 0. */
  defaultRotation?: number;
  onRotationChange?: (rotation: number) => void;
  fit?: 'none' | 'width' | 'page' | 'auto';
  continuous?: boolean;
  /** true, false, a node, or a render function receiving the toolbar props. */
  toolbar?: boolean | React.ReactNode | ((api: PdfToolbarProps & { pageCount: number }) => React.ReactNode);
  components?: {
    Page?: React.ComponentType<PdfPageProps>;
    Toolbar?: React.ComponentType<PdfToolbarProps>;
  };
  theme?: Record<string, string | number>;
  labels?: ViewerLabels;
  className?: string;
  style?: React.CSSProperties;
  showStatus?: boolean;
  invert?: boolean;
  dpr?: number;
  pixelTransform?: RenderOptions['pixelTransform'];
  renderer?: PdfPageProps['renderer'];
  highlights?: (page: number, scale: number) => SearchHit[] | undefined;
  /** Query to highlight across the document. */
  search?: string;
  renderLoading?: () => React.ReactNode;
  renderError?: (error: Error) => React.ReactNode;
  renderEmpty?: () => React.ReactNode;
  pageOverlay?: (page: number, info: {
    document: PdfDocument | null; scale: number; pageCount: number;
  }) => React.ReactNode;
  /**
   * Called when a link annotation is clicked. Return `false` to keep the
   * default (open the URI / jump to the page) from running.
   */
  onLink?: (
    link: import('./index.d.ts').LinkAnnotation,
    info: { document: PdfDocument | null; goToPage: (page: number) => void; pageCount: number },
  ) => boolean | void;
  onLoad?: (info: {
    document: PdfDocument | null;
    pageCount: number;
    metadata: Record<string, string>;
    warnings: string[];
  }) => void;
  onError?: (error: Error) => void;
  onRender?: (info: { page: number; scale: number; stats: RenderStats | null }) => void;
  title?: string;
  children?: React.ReactNode;
}

export declare const PdfReader: React.ForwardRefExoticComponent<
  PdfReaderProps & React.RefAttributes<PdfReaderApi>
>;

export interface UsePdfDocumentResult {
  document: PdfDocument | null;
  status: 'idle' | 'loading' | 'ready' | 'error';
  error: Error | null;
  pageCount: number;
  metadata: Record<string, string>;
  warnings: string[];
  bytes: number;
  reload: () => void;
}

export declare function usePdfDocument(
  source: DocumentSource | null | undefined,
  options?: ModuleOptions & { filename?: string },
  deps?: any[],
): UsePdfDocumentResult;

export interface UsePdfPageResult {
  canvasRef: (node: HTMLCanvasElement | null) => void;
  width: number;
  height: number;
  cssWidth: number;
  cssHeight: number;
  rendering: boolean;
  error: Error | null;
  stats: RenderStats | null;
  render: (canvas?: HTMLCanvasElement | null) => Promise<void>;
}

export declare function usePdfPage(
  doc: PdfDocument | null,
  page: number,
  options?: RenderOptions & { enabled?: boolean; renderer?: PdfPageProps['renderer'] },
): UsePdfPageResult;

export declare function useInView(options?: {
  root?: Element | null;
  rootMargin?: string;
}): [(node: Element | null) => void, boolean];

export declare const groupLines: (items: TextItem[], options?: { threshold?: number }) => TextLine[];
