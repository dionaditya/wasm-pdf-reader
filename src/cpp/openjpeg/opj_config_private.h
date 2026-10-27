/* Hand-written for the wasm-pdf-reader build (normally CMake generates this).
 *
 * Deliberately minimal: wasm32 inside Emscripten has <stdint.h>/<stdlib.h> and
 * <string.h>, no <malloc.h>, no posix_memalign, no fseeko/ftello (there is no
 * filesystem here — the codestream arrives through a memory stream), and no
 * threading, so OPJ_HAVE_PTHREAD_H stays undefined and thread.c falls back to
 * running synchronously. */

#define OPJ_PACKAGE_VERSION "2.5.4"

#define OPJ_HAVE_STDINT_H 1

/* Byte order: wasm is little-endian, so OPJ_BIG_ENDIAN must stay undefined. */
