// Native PDF rasterizer: content stream interpreter + scanline AA filler.
// Produces RGBA8 page bitmaps in C++; the app uploads them as OpenGL textures.
#pragma once

#include <string>
#include <vector>

#include "pdfdoc.hpp"

namespace pdf {

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;   // w*h*4, top-down, opaque
};

// One showable glyph (or glyph run) with its box in *page space* (points, y up).
// This is the text layer the editor uses for hit-testing, selection and copy.
struct TextItem {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // ink box
    float ox = 0, oy = 0;                  // glyph origin (baseline)
    float adv = 0;                         // advance width in page points
    float fs = 0;                          // font size in points
    std::string text;                      // UTF-8, may be empty for spaces
};

struct RenderStats {
    int    ops = 0, texts = 0, glyphs = 0, paths = 0, fills = 0, strokes = 0, images = 0;
    int    unsupported = 0;
    int    ms = 0;
    std::string notes;
    void note(const std::string& s) {
        unsupported++;
        if (notes.size() < 300) { if (!notes.empty()) notes += "; "; notes += s; }
    }
};

class Renderer {
public:
    explicit Renderer(Document* d) : doc(d) {}
    // scale = device pixels per PDF point (1.0 = 72 dpi)
    bool renderPage(int pageIndex, double scale, Image& out, RenderStats* st);
    const std::vector<TextItem>& textItems() const { return items; }
    int  lastPageW() const { return lastW; }
    int  lastPageH() const { return lastH; }
private:
    Document* doc;
    int lastW = 0, lastH = 0;
    std::vector<TextItem> items;
};

} // namespace pdf
