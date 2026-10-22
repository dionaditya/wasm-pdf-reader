#pragma once
// JBIG2 (and CCITT) decoding, compiled into the engine from PDFium's decoder.
// See jbig2native.cpp for how the wasm module is turned into C.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pdf {

// Decodes a JBIG2 image to 1 bit per pixel, rows packed MSB-first, top row
// first, pitch = (width + 7) / 8. `globals` may be null/empty.
bool jbig2Decode(const uint8_t* data, size_t len, int width, int height,
                 const uint8_t* globals, size_t globalsLen,
                 std::vector<uint8_t>& out);

}  // namespace pdf
