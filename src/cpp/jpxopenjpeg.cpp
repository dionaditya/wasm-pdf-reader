// JPEG 2000 (JPXDecode) via OpenJPEG — vendored under src/cpp/openjpeg/.
//
// The previous in-tree decoder (jpx-decoder.cc, from nanopdf) assumed
// single-tile, single-tile-part codestreams and decoded packet headers without
// the tag-tree coding the format mandates, so real producer output (Photoshop,
// InDesign, MiKTeX covers) came out as noise. OpenJPEG is what MuPDF and
// PDFium ship for the same job; it handles tiles, layered tile-parts, all five
// progressions and the 5/3 and 9/7 transforms.
//
// The codestream can be a raw J2K codestream (0xFF4F) or a JP2 container. Both
// are accepted here and the whole PDF stream is passed in untouched, so a JP2's
// colour/ICC boxes are readable too.

#include "openjpeg/openjpeg.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace nanopdf {
namespace jpx {

namespace {

// OpenJPEG reports problems through callbacks; keep the last error so the
// caller's warning line says why a page lost its image.
std::string gLastError;

void infoCallback(const char*, void*) {}

void warnCallback(const char* msg, void*) {
    if (msg) gLastError = msg;
}

void errorCallback(const char* msg, void*) {
    if (msg) gLastError = msg;
}

struct MemStream {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;
};

OPJ_SIZE_T readFn(void* buffer, OPJ_SIZE_T bytes, void* user) {
    MemStream* s = static_cast<MemStream*>(user);
    size_t left = s->pos < s->size ? s->size - s->pos : 0;
    size_t take = std::min(static_cast<size_t>(bytes), left);
    if (take == 0) return static_cast<OPJ_SIZE_T>(-1);   // end of stream
    std::memcpy(buffer, s->data + s->pos, take);
    s->pos += take;
    return static_cast<OPJ_SIZE_T>(take);
}

OPJ_OFF_T skipFn(OPJ_OFF_T bytes, void* user) {
    MemStream* s = static_cast<MemStream*>(user);
    if (bytes < 0) return -1;
    size_t want = static_cast<size_t>(bytes);
    size_t left = s->pos < s->size ? s->size - s->pos : 0;
    size_t take = std::min(want, left);
    s->pos += take;
    return static_cast<OPJ_OFF_T>(take);
}

OPJ_BOOL seekFn(OPJ_OFF_T pos, void* user) {
    MemStream* s = static_cast<MemStream*>(user);
    if (pos < 0 || static_cast<size_t>(pos) > s->size) return OPJ_FALSE;
    s->pos = static_cast<size_t>(pos);
    return OPJ_TRUE;
}

// One sample of `comp`, scaled to 8 bits the way the rest of the image path
// expects (0..255, unsigned). JPEG 2000 samples can be sub-sampled, signed, or
// deeper than 8 bits; PDF producers normally use none of those, but a subsample
// factor of 0 would divide by zero, so it is treated as 1.
inline int sample8(const opj_image_comp_t& comp, uint32_t x, uint32_t y) {
    uint32_t dx = comp.dx ? comp.dx : 1;
    uint32_t dy = comp.dy ? comp.dy : 1;
    uint32_t cx = x / dx;
    uint32_t cy = y / dy;
    if (cx >= comp.w || cy >= comp.h || !comp.data) return 0;
    int v = comp.data[static_cast<size_t>(cy) * comp.w + cx];
    int prec = static_cast<int>(comp.prec);
    if (prec <= 0) prec = 8;
    if (comp.sgnd) v += 1 << (prec - 1);   // signed -> unsigned level shift
    if (prec > 8) v >>= (prec - 8);
    else if (prec < 8) v <<= (8 - prec);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return v;
}

bool isJp2Container(const uint8_t* p, size_t n) {
    return n >= 12 && p[0] == 0x00 && p[1] == 0x00 && p[2] == 0x00 && p[3] == 0x0C &&
           p[4] == 'j' && p[5] == 'P' && p[6] == ' ' && p[7] == ' ' &&
           p[8] == 0x0D && p[9] == 0x0A && p[10] == 0x87 && p[11] == 0x0A;
}

}  // namespace

// Decode a JPEG 2000 codestream or JP2 file into interleaved 8-bit samples,
// `ncomp` bytes per pixel (1 = grey, 3 = RGB, 4 = CMYK/YCCK-expanded).
bool decodeJpx(const uint8_t* data, size_t size, std::vector<uint8_t>& out,
               uint32_t& width, uint32_t& height, int& ncomp, std::string& error) {
    out.clear();
    width = height = 0;
    ncomp = 0;
    error.clear();
    gLastError.clear();

    if (!data || size < 2) {
        error = "empty codestream";
        return false;
    }

    OPJ_CODEC_FORMAT fmt = isJp2Container(data, size) ? OPJ_CODEC_JP2 : OPJ_CODEC_J2K;
    // A JP2 container that starts elsewhere (some producers pad the stream) is
    // still recognisable by its first box before the codestream.
    if (fmt == OPJ_CODEC_J2K && !(data[0] == 0xFF && data[1] == 0x4F)) {
        if (size >= 12 && data[4] == 'j' && data[5] == 'P') fmt = OPJ_CODEC_JP2;
        else {
            error = "not a JPEG 2000 codestream";
            return false;
        }
    }

    opj_dparameters_t params;
    opj_set_default_decoder_parameters(&params);

    opj_codec_t* codec = opj_create_decompress(fmt);
    if (!codec) {
        error = "opj_create_decompress failed";
        return false;
    }
    opj_set_info_handler(codec, infoCallback, nullptr);
    opj_set_warning_handler(codec, warnCallback, nullptr);
    opj_set_error_handler(codec, errorCallback, nullptr);

    opj_image_t* image = nullptr;
    opj_stream_t* stream = nullptr;
    MemStream mem{data, size, 0};
    bool ok = false;

    do {
        if (!opj_setup_decoder(codec, &params)) {
            error = "opj_setup_decoder failed";
            break;
        }

        stream = opj_stream_create(4096, OPJ_TRUE);
        if (!stream) {
            error = "opj_stream_create failed";
            break;
        }
        opj_stream_set_user_data(stream, &mem, nullptr);
        opj_stream_set_user_data_length(stream, static_cast<OPJ_UINT64>(size));
        opj_stream_set_read_function(stream, readFn);
        opj_stream_set_skip_function(stream, skipFn);
        opj_stream_set_seek_function(stream, seekFn);

        if (!opj_read_header(stream, codec, &image)) {
            error = gLastError.empty() ? "codestream header rejected" : gLastError;
            break;
        }
        if (!opj_decode(codec, stream, image) || !opj_end_decompress(codec, stream)) {
            error = gLastError.empty() ? "codestream decode failed" : gLastError;
            break;
        }

        const uint32_t w = image->x1 > image->x0 ? image->x1 - image->x0 : 0;
        const uint32_t h = image->y1 > image->y0 ? image->y1 - image->y0 : 0;
        if (w == 0 || h == 0 || image->numcomps == 0) {
            error = "decoded image has no pixels";
            break;
        }
        if (image->numcomps > 4) {
            error = "unsupported component count (" + std::to_string(image->numcomps) + ")";
            break;
        }
        // A two-component image is grey + alpha in JP2 terms; PDF has no such
        // colour space, so take the grey plane and ignore the alpha.
        const int use = image->numcomps >= 3 ? static_cast<int>(image->numcomps) : 1;

        // Refuse rather than abort the module: 4 GiB of address space is shared
        // with the render target and the document itself.
        const size_t bytes = static_cast<size_t>(w) * h * static_cast<size_t>(use);
        if (bytes > (size_t)512 * 1024 * 1024) {
            error = "JPEG 2000 image too large to decode inline";
            break;
        }

        out.resize(bytes);
        uint8_t* dst = out.data();
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                for (int c = 0; c < use; ++c) {
                    *dst++ = static_cast<uint8_t>(sample8(image->comps[c], x, y));
                }
            }
        }
        width = w;
        height = h;
        ncomp = use;
        ok = true;
    } while (false);

    if (image) opj_image_destroy(image);
    opj_stream_destroy(stream);
    opj_destroy_codec(codec);

    if (!ok && error.empty()) error = "JPEG 2000 decode failed";
    return ok;
}

}  // namespace jpx
}  // namespace nanopdf
