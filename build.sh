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

EXPORTS='_wpdf_abi_version,_wpdf_engine_info,_wpdf_last_error,_wpdf_open,_wpdf_open_pw,_wpdf_open_stream,_wpdf_open_windowed,_wpdf_source_stats,_wpdf_close,_wpdf_page_count,_wpdf_page_size,_wpdf_render_page,_wpdf_bitmap_ptr,_wpdf_bitmap_width,_wpdf_bitmap_height,_wpdf_page_text_json,_wpdf_page_stats,_wpdf_info_json,_wpdf_warnings_json,_wpdf_alloc,_wpdf_free,_malloc,_free,_wpdf_page_links_json,_wpdf_outline_json,_wpdf_form_json,_wpdf_set_field'
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
# PDFium's JBIG2 decoder, translated from the pdf.js.jbig2 wasm module with
# wabt's wasm2c. The .c files are C, not C++: compile them separately.
JBIG2_DIR="src/cpp/jbig2gen"
JBIG2_OBJ_DIR="build/jbig2"
mkdir -p "$JBIG2_OBJ_DIR"
emcc -O2 -msimd128 -I "$JBIG2_DIR" -c "$JBIG2_DIR/jbig2.c" -o "$JBIG2_OBJ_DIR/jbig2.o"
emcc -O2 -I "$JBIG2_DIR" -c "$JBIG2_DIR/wasm-rt-impl.c" -o "$JBIG2_OBJ_DIR/wasmrt.o" \
  -DWASM_RT_TRAP_HANDLER=pdf_jbig2_trap
emcc -O2 -I "$JBIG2_DIR" -c "$JBIG2_DIR/wasm-rt-mem-impl.c" -o "$JBIG2_OBJ_DIR/wasmrtmem.o"

em++ \
  src/cpp/pdfdoc.cpp \
  src/cpp/jbig2native.cpp \
  "$JBIG2_OBJ_DIR/jbig2.o" "$JBIG2_OBJ_DIR/wasmrt.o" "$JBIG2_OBJ_DIR/wasmrtmem.o" \
  -I "$JBIG2_DIR" \
  src/cpp/pdfannots.cpp \
  src/cpp/pdfraster.cpp \
  src/cpp/iccp.cpp \
  src/cpp/wpdf_api.cpp \
  src/cpp/cfffont.cpp \
  src/cpp/cff-parser.cc \
  src/cpp/cmap.cpp \
  src/cpp/cmaps_data.cpp \
  src/cpp/jpx-decoder.cc \
  "${FLAGS[@]}" \
  -o "$OUT_DIR/pdf-reader.mjs"

ls -lh "$OUT_DIR/pdf-reader.mjs" "$OUT_DIR/pdf-reader.wasm"
echo "build complete: $OUT_DIR"
