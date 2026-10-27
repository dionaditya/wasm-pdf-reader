// JPEG 2000 decoding through OpenJPEG (src/cpp/openjpeg/).
//
// Accepts a raw J2K codestream or a JP2 container and produces interleaved
// 8-bit samples: `ncomp` bytes per pixel, 1 = grey, 3 = RGB, 4 = CMYK (already
// inverse-transformed out of YCCK by the decoder's MCT).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nanopdf {
namespace jpx {

bool decodeJpx(const uint8_t* data, size_t size, std::vector<uint8_t>& out,
               uint32_t& width, uint32_t& height, int& ncomp, std::string& error);

}  // namespace jpx
}  // namespace nanopdf
