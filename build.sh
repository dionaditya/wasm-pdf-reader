#!/usr/bin/env bash
# Build the WebAssembly module from the C++ reader.
#
#   ./build.sh          # release build -> dist/wasm/pdf-reader.mjs + .wasm
#   ./build.sh --debug  # assertions on, readable stack traces
#
# Requires emsdk on PATH:  . ~/emsdk/emsdk_env.sh
set -euo pipefail
cd "$(dirname "$0")"

if ! command -v em++ >/dev/null 2>&1; then
  echo "em++ not found. Install emsdk and run '. ~/emsdk/emsdk_env.sh' first." >&2
  exit 1
fi

MODE="release"
if [ "${1:-}" = "--debug" ]; then MODE="debug"; fi

OUT_DIR="dist/wasm"
mkdir -p "$OUT_DIR"

EXPORTS='_wpdf_abi_version,_wpdf_engine_info,_wpdf_last_error,_wpdf_open,_wpdf_open_pw,_wpdf_close,_wpdf_page_count,_wpdf_page_size,_wpdf_render_page,_wpdf_bitmap_ptr,_wpdf_bitmap_width,_wpdf_bitmap_height,_wpdf_page_text_json,_wpdf_page_stats,_wpdf_info_json,_wpdf_warnings_json,_wpdf_alloc,_wpdf_free,_malloc,_free,_wpdf_pending_images_json,_wpdf_clear_pending_images,_wpdf_image_bytes,_wpdf_image_bytes_len,_wpdf_set_image,_wpdf_page_links_json'
RUNTIME='HEAPU8,HEAPU32,UTF8ToString,stringToUTF8,ccall,cwrap,getValue,setValue'

FLAGS=(
  -I src/cpp
  -std=c++17
  -Wall -Wno-unused-function
  -fno-exceptions
  -s USE_ZLIB=1
  -s MODULARIZE=1
  -s EXPORT_NAME=createPdfReaderModule
  -s EXPORT_ES6=1
  -s ENVIRONMENT=web,worker,node
  -s ALLOW_MEMORY_GROWTH=1
  -s MAXIMUM_MEMORY=1073741824
  -s INITIAL_MEMORY=16777216
  -s STACK_SIZE=2097152
  -s FILESYSTEM=0
  -s DYNAMIC_EXECUTION=0
  -s "EXPORTED_FUNCTIONS=$EXPORTS"
  -s "EXPORTED_RUNTIME_METHODS=$RUNTIME"
)

if [ "$MODE" = "debug" ]; then
  FLAGS+=(-O0 -g3 -s ASSERTIONS=2 -s SAFE_HEAP=1 -s STACK_OVERFLOW_CHECK=2)
else
  FLAGS+=(-O3 -flto --closure=0 -s ASSERTIONS=0)
fi

echo "building ($MODE) ..."
em++ \
  src/cpp/pdfdoc.cpp \
  src/cpp/pdfannots.cpp \
  src/cpp/pdfraster.cpp \
  src/cpp/wpdf_api.cpp \
  src/cpp/cfffont.cpp \
  src/cpp/cff-parser.cc \
  src/cpp/jpx-decoder.cc \
  "${FLAGS[@]}" \
  -o "$OUT_DIR/pdf-reader.mjs"

ls -lh "$OUT_DIR/pdf-reader.mjs" "$OUT_DIR/pdf-reader.wasm"
echo "build complete: $OUT_DIR"
