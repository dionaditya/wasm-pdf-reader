// wpdf_api.cpp — the C ABI that the JavaScript wrapper talks to.
//
// Everything above this file is a plain C++ PDF reader (pdfdoc.cpp) and
// rasterizer (pdfraster.cpp) that know nothing about the web. This shim is the
// only place that knows about wasm: it owns documents, keeps the last rendered
// bitmap in memory so JS can copy it out without an allocation per page, and
// serialises the page text layer as JSON.
//
// Rules for the ABI:
//   * every pointer returned is owned by the module; JS must copy, never free;
//   * strings are UTF-8, NUL terminated, and stay valid until the next call that
//     returns a string;
//   * functions return 1/0 (or a count) and report the reason in wpdf_last_error().

#include "pdfdoc.hpp"
#include "pdfraster.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define WPDF_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define WPDF_EXPORT
#endif

namespace {

constexpr int kAbiVersion = 1;
constexpr double kTextRenderScale = 0.06;   // tiny canvas, real text layer

struct DocHandle {
    pdf::Document doc;
    std::vector<pdf::TextItem> text;
    std::vector<pdf::RenderStats> stats;
    std::string textJson;
    std::string error;
    int textPage = -1;
    std::string imageBytesCache;

    void reset(int pageCount) {
        text.clear();
        textPage = -1;
        stats.assign(pageCount > 0 ? pageCount : 0, pdf::RenderStats());
    }
    int count() const { return doc.pageCount(); }
};

std::string gLastError;
std::string gEngineInfo;
pdf::Image gBitmap;

std::string JsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            case '\b': o += "\\b";  break;
            case '\f': o += "\\f";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    o += buf;
                } else {
                    o.push_back((char)c);
                }
        }
    }
    return o;
}

std::string Num(float v) {
    if (!std::isfinite(v)) return "0";
    char buf[40];
    snprintf(buf, sizeof(buf), "%.3f", (double)v);
    // trim trailing zeros so the JSON stays readable in a devtools console
    std::string s(buf);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s.empty() || s == "-" ? "0" : s;
}

DocHandle* H(void* p) { return static_cast<DocHandle*>(p); }

// Extract the page text layer without allocating a full-size canvas: the
// rasterizer only needs a canvas big enough to exist, and the item boxes it
// reports are in page space, so the scale does not change them.
bool ExtractText(DocHandle* h, int page) {
    if (!h) return false;
    pdf::Renderer r(&h->doc);
    pdf::Image tiny;
    pdf::RenderStats st;
    if (!r.renderPage(page, kTextRenderScale, tiny, &st)) return false;
    h->text = r.textItems();
    h->textPage = page;
    if ((int)h->stats.size() <= page) h->stats.resize(page + 1);
    h->stats[page] = st;
    return true;
}

}  // namespace

extern "C" {

WPDF_EXPORT int wpdf_abi_version() { return kAbiVersion; }

// Small JSON blob describing the build, so a page can print what it loaded.
WPDF_EXPORT const char* wpdf_engine_info() {
    if (gEngineInfo.empty()) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "{\"name\":\"wasm-pdf-reader\",\"abi\":%d,\"bytes\":%d}",
                 kAbiVersion, (int)sizeof(void*));
        gEngineInfo = buf;
    }
    return gEngineInfo.c_str();
}

WPDF_EXPORT const char* wpdf_last_error() { return gLastError.c_str(); }

WPDF_EXPORT void* wpdf_open_pw(const uint8_t* data, int len, const char* pw, int pwLen);

// Parse a PDF held in wasm memory. Returns a handle, or nullptr on failure.
WPDF_EXPORT void* wpdf_open(const uint8_t* data, int len) {
    return wpdf_open_pw(data, len, nullptr, 0);
}

// Same, but tries `pw` against the document's encryption handler (user first,
// then owner). An empty password is the same as wpdf_open.
WPDF_EXPORT void* wpdf_open_pw(const uint8_t* data, int len, const char* pw, int pwLen) {
    if (!data || len <= 0) {
        gLastError = "empty document";
        return nullptr;
    }
    DocHandle* h = new DocHandle();
    if (pw && pwLen > 0) h->doc.password.assign(pw, (size_t)pwLen);
    if (!h->doc.load(data, (size_t)len)) {
        gLastError = h->doc.error.empty() ? "cannot parse PDF" : h->doc.error;
        delete h;
        return nullptr;
    }
    h->reset(h->doc.pageCount());
    gLastError.clear();
    return h;
}

WPDF_EXPORT void wpdf_close(void* handle) { delete H(handle); }

WPDF_EXPORT int wpdf_page_count(void* handle) {
    DocHandle* h = H(handle);
    return h ? h->count() : 0;
}

// out: [widthPt, heightPt, rotationDegrees, boxX0, boxY0]. Returns 1.
WPDF_EXPORT int wpdf_page_size(void* handle, int page, double* out) {
    DocHandle* h = H(handle);
    if (!h || page < 0 || page >= h->count() || !out) return 0;
    double w = 612, hh = 792;
    int rot = 0;
    h->doc.pageSize(page, w, hh, rot);
    out[0] = w;
    out[1] = hh;
    out[2] = (double)rot;
    out[3] = 0;
    out[4] = 0;
    // box origin, so a text overlay can be positioned in CSS pixels
    pdf::ObjP p = h->doc.pages[page];
    pdf::ObjP box = h->doc.resolve(p->get("CropBox"));
    if (!box) box = h->doc.resolve(p->get("MediaBox"));
    if (box && box->t == pdf::O_ARR && box->arr.size() == 4) {
        out[3] = pdf::objNum(h->doc.resolve(box->arr[0]), 0);
        out[4] = pdf::objNum(h->doc.resolve(box->arr[1]), 0);
    }
    return 1;
}

// Rasterize `page` at `scale` device pixels per PDF point (72 dpi == 1.0).
// The bitmap is available through wpdf_bitmap_ptr() until the next render.
WPDF_EXPORT int wpdf_render_page(void* handle, int page, double scale) {
    DocHandle* h = H(handle);
    if (!h) {
        gLastError = "no document";
        return 0;
    }
    if (page < 0 || page >= h->count()) {
        gLastError = "page out of range";
        return 0;
    }
    if (!(scale > 0)) scale = 1.0;
    pdf::Renderer r(&h->doc);
    pdf::RenderStats st;
    pdf::Image img;
    if (!r.renderPage(page, scale, img, &st)) {
        gLastError = "render failed (page too large at this scale?)";
        return 0;
    }
    gBitmap.rgba.swap(img.rgba);
    gBitmap.w = img.w;
    gBitmap.h = img.h;
    h->text = r.textItems();
    h->textPage = page;
    if ((int)h->stats.size() <= page) h->stats.resize(page + 1);
    h->stats[page] = st;
    gLastError.clear();
    return 1;
}

WPDF_EXPORT uint8_t* wpdf_bitmap_ptr() { return gBitmap.rgba.empty() ? nullptr : gBitmap.rgba.data(); }
WPDF_EXPORT int wpdf_bitmap_width() { return gBitmap.w; }
WPDF_EXPORT int wpdf_bitmap_height() { return gBitmap.h; }

// The text layer of `page` as a JSON array of glyph runs:
//   {text, x0, y0, x1, y1, ox, oy, adv, fs}   (page space, points, y up)
WPDF_EXPORT const char* wpdf_page_text_json(void* handle, int page) {
    DocHandle* h = H(handle);
    if (!h || page < 0 || page >= h->count()) {
        gLastError = "page out of range";
        return nullptr;
    }
    if (h->textPage != page || h->text.empty()) {
        if (!ExtractText(h, page)) {
            gLastError = "text extraction failed";
            return nullptr;
        }
    }
    if (h->textPage != page) {
        // renderPage() in ExtractText() already filled h->text; guard anyway
        h->textPage = page;
    }
    h->textJson.clear();
    h->textJson.reserve(h->text.size() * 96 + 2);
    h->textJson += '[';
    for (size_t i = 0; i < h->text.size(); ++i) {
        const pdf::TextItem& it = h->text[i];
        if (i) h->textJson += ',';
        h->textJson += "{\"text\":\"";
        h->textJson += JsonEscape(it.text);
        h->textJson += "\",\"x0\":" + Num(it.x0) + ",\"y0\":" + Num(it.y0) +
                       ",\"x1\":" + Num(it.x1) + ",\"y1\":" + Num(it.y1) +
                       ",\"ox\":" + Num(it.ox) + ",\"oy\":" + Num(it.oy) +
                       ",\"adv\":" + Num(it.adv) + ",\"fs\":" + Num(it.fs) + "}";
    }
    h->textJson += ']';
    gLastError.clear();
    return h->textJson.c_str();
}

// Render statistics for a page, as [ops, texts, glyphs, paths, fills, strokes,
// unsupported]. Handy for a debug badge; not needed for normal viewing.
WPDF_EXPORT int wpdf_page_stats(void* handle, int page, int* out, int n) {
    DocHandle* h = H(handle);
    if (!h || page < 0 || page >= (int)h->stats.size() || !out) return 0;
    const pdf::RenderStats& s = h->stats[page];
    int v[7] = {s.ops, s.texts, s.glyphs, s.paths, s.fills, s.strokes, s.unsupported};
    int m = n < 7 ? n : 7;
    for (int i = 0; i < m; ++i) out[i] = v[i];
    return m;
}

// ─── document information dictionary ────────────────────────────────────────
// PDF strings are byte strings; a leading FE FF BOM means UTF-16BE.
static std::string PdfText(const std::string& s) {
    if (s.size() >= 2 && (unsigned char)s[0] == 0xFE && (unsigned char)s[1] == 0xFF) {
        std::string out;
        for (size_t i = 2; i + 1 < s.size(); i += 2) {
            unsigned int u = ((unsigned char)s[i] << 8) | (unsigned char)s[i + 1];
            if (u < 0x80) {
                out.push_back((char)u);
            } else if (u < 0x800) {
                out.push_back((char)(0xC0 | (u >> 6)));
                out.push_back((char)(0x80 | (u & 0x3F)));
            } else {
                out.push_back((char)(0xE0 | (u >> 12)));
                out.push_back((char)(0x80 | ((u >> 6) & 0x3F)));
                out.push_back((char)(0x80 | (u & 0x3F)));
            }
        }
        return out;
    }
    return s;
}

WPDF_EXPORT const char* wpdf_info_json(void* handle) {
    static std::string out;
    DocHandle* h = H(handle);
    if (!h) {
        out = "{}";
        return out.c_str();
    }
    pdf::ObjP info;
    if (h->doc.trailer) info = h->doc.resolve(h->doc.trailer->get("Info"));
    const char* keys[] = {"Title", "Author", "Subject", "Keywords", "Creator", "Producer", "CreationDate"};
    out = "{";
    bool first = true;
    for (const char* key : keys) {
        pdf::ObjP v = info ? h->doc.resolve(info->get(key)) : nullptr;
        if (!v || v->t != pdf::O_STR) continue;
        if (!first) out += ',';
        first = false;
        out += '"';
        out += key;
        out += "\":\"";
        out += JsonEscape(PdfText(v->str));
        out += '"';
    }
    out += '}';
    return out.c_str();
}

// Parse warnings collected while reading the file, as JSON. Empty array for a
// clean document; a viewer can surface these instead of failing silently.
WPDF_EXPORT const char* wpdf_warnings_json(void* handle) {
    static std::string out;
    DocHandle* h = H(handle);
    out = "[";
    if (h) {
        for (size_t i = 0; i < h->doc.warnings.size(); ++i) {
            if (i) out += ',';
            out += '"';
            out += JsonEscape(h->doc.warnings[i]);
            out += '"';
        }
    }
    out += ']';
    return out.c_str();
}

// ─── link annotations ───────────────────────────────────────────────────────
// One entry per /Link annotation with a usable target:
//   { x0, y0, x1, y1, uri }        external link, page space (y up)
//   { x0, y0, x1, y1, page }       internal /GoTo, zero-based page index
WPDF_EXPORT const char* wpdf_page_links_json(void* handle, int page) {
    static std::string out;
    DocHandle* h = H(handle);
    out = "[";
    if (h && page >= 0 && page < h->count()) {
        const std::vector<pdf::Document::Link>& links = h->doc.links(page);
        bool first = true;
        for (const auto& l : links) {
            if (!first) out += ',';
            first = false;
            out += "{\"x0\":" + Num(l.x0) + ",\"y0\":" + Num(l.y0) +
                   ",\"x1\":" + Num(l.x1) + ",\"y1\":" + Num(l.y1);
            if (!l.uri.empty()) out += ",\"uri\":\"" + JsonEscape(l.uri) + "\"";
            if (l.page >= 0) out += ",\"page\":" + std::to_string(l.page);
            out += '}';
        }
    }
    out += ']';
    return out.c_str();
}

// ─── externally decoded images (JBIG2) ──────────────────────────────────────
// The module ships no JBIG2 decoder: the JS side owns a dedicated one and hands
// the finished bitmap back. A render that meets a JBIG2 image paints a hole and
// records it here; the caller decodes each entry, uploads it, and renders again.
WPDF_EXPORT const char* wpdf_pending_images_json(void* handle) {
    static std::string out;
    DocHandle* h = H(handle);
    out = "[";
    if (h) {
        bool first = true;
        for (auto& kv : h->doc.pendingImages) {
            if (!first) out += ',';
            first = false;
            out += "{\"obj\":";
            out += std::to_string(kv.first);
            out += ",\"width\":";
            out += std::to_string(kv.second.first);
            out += ",\"height\":";
            out += std::to_string(kv.second.second);
            out += '}';
        }
    }
    out += ']';
    return out.c_str();
}

WPDF_EXPORT void wpdf_clear_pending_images(void* handle) {
    DocHandle* h = H(handle);
    if (h) h->doc.pendingImages.clear();
}

// Raw stream bytes (still JBIG2-encoded, already decrypted) for one image or
// its /JBIG2Globals stream. Valid until the next call on this handle.
WPDF_EXPORT const void* wpdf_image_bytes(void* handle, int objNum, int globals) {
    DocHandle* h = H(handle);
    if (!h) return nullptr;
    std::string buf;
    if (!h->doc.imageBytes(objNum, globals != 0, buf)) return nullptr;
    h->imageBytesCache = std::move(buf);
    return h->imageBytesCache.data();
}

WPDF_EXPORT int wpdf_image_bytes_len(void* handle) {
    DocHandle* h = H(handle);
    return h ? (int)h->imageBytesCache.size() : 0;
}

// Hand a decoded 1-bpc bitmap (1 = white, rows padded to a byte) back to the
// document; the next render of any page using that XObject will use it.
WPDF_EXPORT int wpdf_set_image(void* handle, int objNum, int width, int height,
                               const void* bits, int stride) {
    DocHandle* h = H(handle);
    if (!h || !bits || width <= 0 || height <= 0 || stride < (width + 7) / 8) return 0;
    h->doc.setExternalImage(objNum, width, height, (const uint8_t*)bits, (size_t)stride);
    return 1;
}

// Host builds (tests, node) need a free() for buffers JS allocated.
WPDF_EXPORT void* wpdf_alloc(size_t n) { return malloc(n); }
WPDF_EXPORT void wpdf_free(void* p) { free(p); }

}  // extern "C"
