#include "pdfraster.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "pdf_glyphs.h"
#include "fonts_data.h"   // metric-compatible document faces (Liberation/Carlito/Caladea)

namespace pdf {
namespace {

// ─── geometry ───────────────────────────────────────────────────────────────
struct Pt { float x, y; };
typedef std::vector<Pt> Poly;
typedef std::vector<Poly> Polys;

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

// ─── canvas ─────────────────────────────────────────────────────────────────
typedef std::shared_ptr<std::vector<uint8_t>> ClipMask;

struct Canvas {
    int w = 0, h = 0;
    std::vector<uint8_t> px;      // RGBA, straight alpha, background opaque white

    void init(int width, int height) {
        w = width; h = height;
        px.assign((size_t)w * h * 4, 255);
    }
    inline void blend(int x, int y, float r, float g, float b, float a) {
        if (a <= 0.f) return;
        if (a > 1.f) a = 1.f;
        uint8_t* p = &px[((size_t)y * w + x) * 4];
        p[0] = (uint8_t)(p[0] * (1.f - a) + r * 255.f * a + 0.5f);
        p[1] = (uint8_t)(p[1] * (1.f - a) + g * 255.f * a + 0.5f);
        p[2] = (uint8_t)(p[2] * (1.f - a) + b * 255.f * a + 0.5f);
        p[3] = 255;
    }
};

// ─── scanline coverage ──────────────────────────────────────────────────────
// 4 vertical subsamples per row, analytic coverage in x.
static const int kSub = 4;

struct Edge { float x0, y0, x1, y1; int dir; };

static void addSpanCov(float* cov, int base, float xa, float xb, int n) {
    if (xb <= xa) return;
    float l = xa - base, r = xb - base;
    if (r <= 0 || l >= n) return;
    if (l < 0) l = 0;
    if (r > n) r = (float)n;
    int ia = (int)l, ib = (int)r;
    if (ia == ib) { cov[ia] += (r - l); return; }
    cov[ia] += (ia + 1 - l);
    for (int k = ia + 1; k < ib; k++) cov[k] += 1.f;
    if (ib < n) cov[ib] += (r - ib);
}

static void FillPolys(Canvas& cv, const Polys& polys, bool evenOdd,
                      float r, float g, float b, float alpha,
                      const std::vector<uint8_t>* clip = nullptr) {
    if (alpha <= 0.f) return;
    // collect edges inline (duplicated from ScanPolys for speed/clarity)
    std::vector<Edge> edges;
    float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    for (const Poly& p : polys) {
        if (p.size() < 2) continue;
        for (size_t i = 0; i < p.size(); i++) {
            const Pt& a = p[i];
            const Pt& b = p[(i + 1) % p.size()];
            minx = std::min(minx, a.x); maxx = std::max(maxx, a.x);
            miny = std::min(miny, a.y); maxy = std::max(maxy, a.y);
            if (a.y == b.y) continue;
            if (a.y < b.y) edges.push_back({a.x, a.y, b.x, b.y, 1});
            else           edges.push_back({b.x, b.y, a.x, a.y, -1});
        }
    }
    if (edges.empty()) return;
    int y0 = (int)std::floor(miny), y1 = (int)std::ceil(maxy);
    int x0 = (int)std::floor(minx), x1 = (int)std::ceil(maxx);
    if (y0 < 0) y0 = 0;
    if (y1 > cv.h) y1 = cv.h;
    if (x0 < 0) x0 = 0;
    if (x1 > cv.w) x1 = cv.w;
    if (y1 <= y0 || x1 <= x0) return;

    int n = x1 - x0;
    std::vector<float> cov(n, 0.f);
    std::vector<std::pair<float, int>> xs;
    for (int y = y0; y < y1; y++) {
        std::fill(cov.begin(), cov.end(), 0.f);
        bool any = false;
        for (int s = 0; s < kSub; s++) {
            float yc = (float)y + ((float)s + 0.5f) / kSub;
            xs.clear();
            for (const Edge& e : edges)
                if (yc >= e.y0 && yc < e.y1)
                    xs.push_back({e.x0 + (yc - e.y0) * (e.x1 - e.x0) / (e.y1 - e.y0), e.dir});
            if (xs.empty()) continue;
            std::sort(xs.begin(), xs.end());
            int wind = 0;
            float start = 0;
            bool inside = false;
            for (auto& c : xs) {
                wind += c.second;
                bool on = evenOdd ? ((wind & 1) != 0) : (wind != 0);
                if (on && !inside) { start = c.first; inside = true; }
                else if (!on && inside) { addSpanCov(cov.data(), x0, start, c.first, n); inside = false; any = true; }
            }
            if (inside) { addSpanCov(cov.data(), x0, start, (float)(x0 + n), n); any = true; }
        }
        if (!any) continue;
        for (int i = 0; i < n; i++) {
            float c = cov[i] * (1.f / kSub);
            if (c <= 0.002f) continue;
            float al = c * alpha;
            if (clip) al *= (*clip)[(size_t)y * cv.w + x0 + i] * (1.f / 255.f);
            if (al > 0.f) cv.blend(x0 + i, y, r, g, b, al);
        }
    }
}

// Intersect the current clip (possibly null) with a path; the caller owns the result.
static ClipMask ClipPolys(const Canvas& cv, const ClipMask& cur, const Polys& polys, bool evenOdd) {
    ClipMask out = std::make_shared<std::vector<uint8_t>>((size_t)cv.w * cv.h, 0);
    std::vector<uint8_t>& mask = *out;
    std::vector<Edge> edges;
    float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    for (const Poly& p : polys) {
        if (p.size() < 2) continue;
        for (size_t i = 0; i < p.size(); i++) {
            const Pt& a = p[i];
            const Pt& b = p[(i + 1) % p.size()];
            minx = std::min(minx, a.x); maxx = std::max(maxx, a.x);
            miny = std::min(miny, a.y); maxy = std::max(maxy, a.y);
            if (a.y == b.y) continue;
            if (a.y < b.y) edges.push_back({a.x, a.y, b.x, b.y, 1});
            else           edges.push_back({b.x, b.y, a.x, a.y, -1});
        }
    }
    if (edges.empty()) return cur;
    int y0 = std::max(0, (int)std::floor(miny)), y1 = std::min(cv.h, (int)std::ceil(maxy));
    int x0 = std::max(0, (int)std::floor(minx)), x1 = std::min(cv.w, (int)std::ceil(maxx));
    if (y1 <= y0 || x1 <= x0) return cur;
    int n = x1 - x0;
    std::vector<float> cov(n, 0.f);
    std::vector<std::pair<float, int>> xs;
    for (int y = y0; y < y1; y++) {
        std::fill(cov.begin(), cov.end(), 0.f);
        for (int s = 0; s < kSub; s++) {
            float yc = (float)y + ((float)s + 0.5f) / kSub;
            xs.clear();
            for (const Edge& e : edges)
                if (yc >= e.y0 && yc < e.y1)
                    xs.push_back({e.x0 + (yc - e.y0) * (e.x1 - e.x0) / (e.y1 - e.y0), e.dir});
            if (xs.empty()) continue;
            std::sort(xs.begin(), xs.end());
            int wind = 0; float start = 0; bool inside = false;
            for (auto& c : xs) {
                wind += c.second;
                bool on = evenOdd ? ((wind & 1) != 0) : (wind != 0);
                if (on && !inside) { start = c.first; inside = true; }
                else if (!on && inside) { addSpanCov(cov.data(), x0, start, c.first, n); inside = false; }
            }
            if (inside) addSpanCov(cov.data(), x0, start, (float)(x0 + n), n);
        }
        for (int i = 0; i < n; i++) {
            float c = cov[i] * (1.f / kSub);
            uint8_t v = (uint8_t)std::min(255.f, c * 255.f + 0.5f);
            mask[(size_t)y * cv.w + x0 + i] = v;
        }
    }
    if (cur)
        for (size_t i = 0; i < cur->size(); i++) mask[i] = (uint8_t)(((*cur)[i] * mask[i]) / 255);
    return out;
}

// ─── path building / flattening ─────────────────────────────────────────────
struct PathBuilder {
    Mat ctm;
    Polys polys;
    Poly cur;
    bool open = false;
    double lastUx = 0, lastUy = 0;      // current point in *user* space

    explicit PathBuilder(const Mat& m) : ctm(m) {}

    void dev(double x, double y, float& ox, float& oy) const {
        double dx, dy;
        ctm.apply(x, y, dx, dy);
        ox = (float)dx; oy = (float)dy;
    }
    void moveTo(double x, double y) {
        flush();
        float dx, dy; dev(x, y, dx, dy);
        cur.push_back({dx, dy});
        open = true;
        lastUx = x; lastUy = y;
    }
    void lineTo(double x, double y) {
        if (!open) { moveTo(x, y); return; }
        float dx, dy; dev(x, y, dx, dy);
        cur.push_back({dx, dy});
        lastUx = x; lastUy = y;
    }
    void curveTo(double x1, double y1, double x2, double y2, double x3, double y3) {
        if (!open) { moveTo(x1, y1); }
        lastUx = x3; lastUy = y3;
        float ax, ay, bx, by, cx, cy;
        dev(x1, y1, ax, ay); dev(x2, y2, bx, by); dev(x3, y3, cx, cy);
        Pt p0 = cur.back();
        flattenCubic(p0.x, p0.y, ax, ay, bx, by, cx, cy, 0);
    }
    void close() {
        if (cur.size() > 1) {
            // explicit closing segment so stroking sees the real last edge
            float dx, dy;
            if (cur.front().x != cur.back().x || cur.front().y != cur.back().y) {
                dx = cur.front().x; dy = cur.front().y;
                cur.push_back({dx, dy});
            }
        }
        flush();
    }
    void flush() {
        if (cur.size() >= 2) polys.push_back(cur);
        cur.clear();
        open = false;
    }
    Polys take() { flush(); Polys out; out.swap(polys); return out; }

    void flattenCubic(float x0, float y0, float x1, float y1, float x2, float y2,
                      float x3, float y3, int depth) {
        auto flat = [](float a, float b, float c, float d) {
            return std::fabs(a - 2 * b + c) + std::fabs(b - 2 * c + d);
        };
        if (depth > 10 ||
            (flat(x0, x1, x2, x3) < 0.20f && flat(y0, y1, y2, y3) < 0.20f)) {
            cur.push_back({x3, y3});
            return;
        }
        float x01 = (x0 + x1) * 0.5f, y01 = (y0 + y1) * 0.5f;
        float x12 = (x1 + x2) * 0.5f, y12 = (y1 + y2) * 0.5f;
        float x23 = (x2 + x3) * 0.5f, y23 = (y2 + y3) * 0.5f;
        float xa = (x01 + x12) * 0.5f, ya = (y01 + y12) * 0.5f;
        float xb = (x12 + x23) * 0.5f, yb = (y12 + y23) * 0.5f;
        float xm = (xa + xb) * 0.5f, ym = (ya + yb) * 0.5f;
        flattenCubic(x0, y0, x01, y01, xa, ya, xm, ym, depth + 1);
        flattenCubic(xm, ym, xb, yb, x23, y23, x3, y3, depth + 1);
    }
};

// ─── stroking: segments as quads, round joins/caps as small circles ─────────
static void addCircle(Poly& out, float cx, float cy, float r) {
    const int N = 8;
    if (r <= 0.05f) return;
    Poly c;
    for (int i = 0; i < N; i++) {
        float t = (float)i / N * 6.2831853f;
        c.push_back({cx + cosf(t) * r, cy + sinf(t) * r});
    }
    // append as separate loop
    out.insert(out.end(), c.begin(), c.end());
}

static void StrokePolys(Polys& out, const Poly& pts, float w, int cap, bool closed) {
    if (pts.size() < 2) return;
    float hw = w * 0.5f;
    if (hw < 0.05f) hw = 0.05f;
    size_t n = pts.size();
    for (size_t i = 0; i + 1 < n; i++) {
        Pt a = pts[i], b = pts[i + 1];
        float dx = b.x - a.x, dy = b.y - a.y;
        float len = sqrtf(dx * dx + dy * dy);
        if (len < 1e-6f) continue;
        float nx = -dy / len * hw, ny = dx / len * hw;
        Poly q;
        q.push_back({a.x + nx, a.y + ny});
        q.push_back({b.x + nx, b.y + ny});
        q.push_back({b.x - nx, b.y - ny});
        q.push_back({a.x - nx, a.y - ny});
        out.push_back(q);
    }
    // joins
    size_t jStart = closed ? 0 : 1;
    size_t jEnd = closed ? n : n - 1;
    for (size_t i = jStart; i < jEnd; i++) {
        Poly c;
        addCircle(c, pts[i].x, pts[i].y, hw);
        if (!c.empty()) out.push_back(c);
    }
    if (!closed) {
        if (cap == 1) {   // round
            Poly c; addCircle(c, pts.front().x, pts.front().y, hw);
            if (!c.empty()) out.push_back(c);
            Poly c2; addCircle(c2, pts.back().x, pts.back().y, hw);
            if (!c2.empty()) out.push_back(c2);
        } else if (cap == 2) {   // square
            Pt a = pts[0], b = pts[1];
            float dx = a.x - b.x, dy = a.y - b.y, len = sqrtf(dx * dx + dy * dy);
            if (len > 1e-6f) {
                dx /= len; dy /= len;
                float nx = -dy * hw, ny = dx * hw;
                Poly q;
                q.push_back({a.x + nx, a.y + ny});
                q.push_back({a.x - nx, a.y - ny});
                q.push_back({a.x - nx + dx * hw, a.y - ny + dy * hw});
                q.push_back({a.x + nx + dx * hw, a.y + ny + dy * hw});
                out.push_back(q);
            }
            Pt c2 = pts[n - 1], d2 = pts[n - 2];
            float ex = c2.x - d2.x, ey = c2.y - d2.y, el = sqrtf(ex * ex + ey * ey);
            if (el > 1e-6f) {
                ex /= el; ey /= el;
                float nx = -ey * hw, ny = ex * hw;
                Poly q;
                q.push_back({c2.x + nx, c2.y + ny});
                q.push_back({c2.x - nx, c2.y - ny});
                q.push_back({c2.x - nx + ex * hw, c2.y - ny + ey * hw});
                q.push_back({c2.x + nx + ex * hw, c2.y + ny + ey * hw});
                out.push_back(q);
            }
        }
    }
}

// Dashed polylines: split by pattern; each dash becomes its own open polyline
static void ApplyDash(const Polys& in, const std::vector<double>& dash, double phase,
                      bool closed, std::vector<std::pair<Poly, bool>>& out) {
    double total = 0;
    for (double d : dash) total += d;
    if (dash.empty() || total <= 0.0001) {
        for (const Poly& p : in) out.push_back({p, closed});
        return;
    }
    size_t di = 0;
    bool on = true;
    double remain = dash[0];
    if (phase > 0) {
        double ph = fmod(phase, total);
        while (ph >= dash[di]) { ph -= dash[di]; di = (di + 1) % dash.size(); on = !on; }
        remain = dash[di] - ph;
    }
    for (const Poly& p : in) {
        size_t cnt = closed ? p.size() : p.size() - 1;
        Poly cur;
        for (size_t i = 0; i < cnt; i++) {
            Pt a = p[i], b = p[(i + 1) % p.size()];
            float dx = b.x - a.x, dy = b.y - a.y;
            float segLen = sqrtf(dx * dx + dy * dy);
            if (segLen < 1e-7f) continue;
            float ux = dx / segLen, uy = dy / segLen;
            float travelled = 0;
            while (travelled < segLen) {
                float step = (float)std::min((double)(segLen - travelled), remain);
                Pt p1{travelled, 0}, p2{travelled + step, 0};
                if (on) {
                    if (cur.empty()) cur.push_back({a.x + ux * p1.x, a.y + uy * p1.x});
                    cur.push_back({a.x + ux * p2.x, a.y + uy * p2.x});
                }
                travelled += step;
                remain -= step;
                if (remain <= 1e-4) {
                    if (on && cur.size() > 1) { out.push_back({cur, false}); cur.clear(); }
                    di = (di + 1) % dash.size();
                    on = !on;
                    remain = std::max(0.01, dash[di]);
                }
            }
        }
        if (on && cur.size() > 1) out.push_back({cur, false});
    }
}

// ─── substitute fonts ───────────────────────────────────────────────────────
struct SubFace {
    stbtt_fontinfo info;
    const unsigned char* data = nullptr;
    unsigned int len = 0;
    bool tried = false, ok = false;
};

static SubFace gFaces[8];

static const PdfSubFont* faceSpec(const char* key) {
    for (int i = 0; i < kPdfSubFontCount; i++)
        if (strcmp(kPdfSubFonts[i].key, key) == 0) return &kPdfSubFonts[i];
    return nullptr;
}

static SubFace* faceByKey(const char* key) {
    static const char* keys[8] = {nullptr};
    static int nkeys = 0;
    int idx = -1;
    for (int i = 0; i < nkeys; i++) if (keys[i] == key || (keys[i] && strcmp(keys[i], key) == 0)) { idx = i; break; }
    if (idx < 0) {
        if (nkeys >= 8) return nullptr;
        idx = nkeys;
        keys[nkeys++] = key;
    }
    SubFace& f = gFaces[idx];
    if (!f.tried) {
        f.tried = true;
        const PdfSubFont* spec = faceSpec(key);
        if (spec) {
            f.data = spec->data;
            f.len = spec->len;
            f.ok = stbtt_InitFont(&f.info, f.data, 0) != 0;
        }
    }
    return f.ok ? &f : nullptr;
}

static SubFace* faceFromData(const unsigned char* data, unsigned int len) {
    static SubFace slots[24];
    static int count = 0;
    for (int i = 0; i < count; i++)
        if (slots[i].data == data) return slots[i].ok ? &slots[i] : nullptr;
    if (count >= 24) return nullptr;
    SubFace& f = slots[count++];
    f.data = data;
    f.len = len;
    f.ok = stbtt_InitFont(&f.info, data, 0) != 0;
    return f.ok ? &f : nullptr;
}

struct FacePair { SubFace* primary = nullptr; SubFace* fallback = nullptr; };

// Metric-compatible face first — those glyph widths match the /Widths the PDF
// lays text out with, so letters neither collide nor drift. The wide-coverage
// DejaVu subset is only consulted for characters the document face lacks
// (greek, math operators, ligatures).
static FacePair facePairFor(const Font* font) {
    FacePair fp;
    if (font && !font->embedded.empty()) {
        fp.primary = faceFromData((const unsigned char*)font->embedded.data(),
                                  (unsigned int)font->embedded.size());
        if (fp.primary) return fp;
    }
    int cls = font ? font->faceClass() : 1;
    bool bold = font ? font->faceBold() : false;
    bool ital = font ? font->faceItalic() : false;
    int fam = cls == 2 ? 5 : (cls == 0 ? 3 : 4);          // Liberation Mono / Sans / Serif
    int style = bold ? (ital ? 3 : 1) : (ital ? 2 : 0);
    fp.primary = faceFromData(kFontFaces[fam][style].data, kFontFaces[fam][style].len);
    if (!fp.primary) fp.primary = faceFromData(kFontFaces[fam][0].data, kFontFaces[fam][0].len);
    const char* key = cls == 2 ? "mono_regular"
                    : (cls == 0 ? "sans_regular"
                                : (bold ? "serif_bold" : (ital ? "serif_italic" : "serif_regular")));
    if (const PdfSubFont* spec = faceSpec(key)) fp.fallback = faceFromData(spec->data, spec->len);
    return fp;
}

static SubFace* faceFor(const Font* font, const std::string& embedded) {
    // Embedded TrueType is the best source when the PDF carries one.
    if (!embedded.empty()) {
        static SubFace emb;
        static const void* lastData = nullptr;
        if (lastData != (const void*)embedded.data()) {
            lastData = embedded.data();
            emb.data = (const unsigned char*)embedded.data();
            emb.len = (unsigned int)embedded.size();
            emb.ok = stbtt_InitFont(&emb.info, emb.data, 0) != 0;
        }
        if (emb.ok) return &emb;
    }
    int cls = font ? font->faceClass() : 1;
    bool bold = font ? font->faceBold() : false;
    bool ital = font ? font->faceItalic() : false;
    if (cls == 2) return faceByKey("mono_regular");
    if (cls == 0) return faceByKey("sans_regular");
    if (bold) return faceByKey("serif_bold");
    if (ital) return faceByKey("serif_italic");
    return faceByKey("serif_regular");
}

// ─── graphics state ─────────────────────────────────────────────────────────
struct GState {
    Mat ctm;
    double lw = 1.0, miter = 10.0;
    int cap = 0, join = 0;
    std::vector<double> dash;
    double dashPhase = 0;
    float fill[3] = {0, 0, 0};
    float stroke[3] = {0, 0, 0};
    float fillA = 1.f, strokeA = 1.f;
    ClipMask clip;
};

// Uniform scale of a CTM: line widths and dash lengths are user-space values
// and must be brought into device space before stroking flattened geometry.
static double ctmScale(const Mat& m) {
    double det = fabs(m.a * m.d - m.b * m.c);
    return det > 1e-12 ? sqrt(det) : 1.0;
}

static void cmykToRgb(double c, double m, double y, double k, float* out) {
    out[0] = (float)((1 - c) * (1 - k));
    out[1] = (float)((1 - m) * (1 - k));
    out[2] = (float)((1 - y) * (1 - k));
}

static double num(ObjP o, double def) { return o ? objNum(o, def) : def; }

// ─── content interpreter ────────────────────────────────────────────────────
// ─── shading functions ──────────────────────────────────────────────────────
struct ShadingFn {
    Document* doc;
    // evaluate at t in [0,1] -> ncomp components
    bool eval(ObjP fn, double t, double* out, int ncomp);
};

static double sampleComponent(const std::string& data, size_t idx, int bits) {
    if (bits == 8) return idx < data.size() ? (unsigned char)data[idx] / 255.0 : 0.0;
    if (bits == 16) {
        size_t b0 = idx * 2;
        if (b0 + 1 >= data.size()) return 0.0;
        return ((unsigned char)data[b0] * 256 + (unsigned char)data[b0 + 1]) / 65535.0;
    }
    return 0.0;
}

bool ShadingFn::eval(ObjP fn, double t, double* out, int ncomp) {
    if (!fn) return false;
    fn = doc->resolve(fn);
    if (!fn) return false;
    int type = (int)num(doc->resolve(fn->get("FunctionType")), -1);
    if (type == 2) {   // exponential interpolation
        ObjP c0 = doc->resolve(fn->get("C0"));
        ObjP c1 = doc->resolve(fn->get("C1"));
        double N = num(doc->resolve(fn->get("N")), 1);
        double tt = pow(std::max(0.0, std::min(1.0, t)), N);
        for (int i = 0; i < ncomp; i++) {
            double a = c0 && (int)c0->arr.size() > i ? num(doc->resolve(c0->arr[i]), 0) : 0.0;
            double b = c1 && (int)c1->arr.size() > i ? num(doc->resolve(c1->arr[i]), 1) : 1.0;
            out[i] = a + (b - a) * tt;
        }
        return true;
    }
    if (type == 3) {   // stitching: pick a sub-function from Bounds/Encode
        ObjP fns = doc->resolve(fn->get("Functions"));
        ObjP bounds = doc->resolve(fn->get("Bounds"));
        ObjP encode = doc->resolve(fn->get("Encode"));
        if (!fns || !bounds) return false;
        int k = 0, n = (int)fns->arr.size();
        while (k < n - 1 && k < (int)bounds->arr.size() && t > num(doc->resolve(bounds->arr[k]), 1)) k++;
        double lo = 0, hi = 1;
        if (encode && (int)encode->arr.size() >= 2 * (k + 1)) {
            lo = num(doc->resolve(encode->arr[2 * k]), 0);
            hi = num(doc->resolve(encode->arr[2 * k + 1]), 1);
        }
        double sub = lo + (hi - lo) * t;
        return eval(fns->arr[k], sub, out, ncomp);
    }
    if (type == 0) {   // sampled
        if (!fn->isStream) return false;
        std::string data;
        if (!doc->decodeStream(fn, data)) return false;
        ObjP size = doc->resolve(fn->get("Size"));
        int m = size && !size->arr.empty() ? (int)num(doc->resolve(size->arr[0]), 2) : 2;
        int bits = (int)num(doc->resolve(fn->get("BitsPerSample")), 8);
        ObjP range = doc->resolve(fn->get("Range"));
        ObjP domain = doc->resolve(fn->get("Domain"));
        double t0 = domain && (int)domain->arr.size() >= 2 ? num(doc->resolve(domain->arr[0]), 0) : 0;
        double t1 = domain && (int)domain->arr.size() >= 2 ? num(doc->resolve(domain->arr[1]), 1) : 1;
        double u = (t - t0) / (t1 - t0 > 1e-9 ? (t1 - t0) : 1);
        u = std::max(0.0, std::min(1.0, u));
        double pos = u * (m - 1);
        int i0 = (int)pos, i1 = std::min(m - 1, i0 + 1);
        double frac = pos - i0;
        int bytesPerSample = bits / 8;
        for (int c = 0; c < ncomp; c++) {
            size_t base0 = ((size_t)i0 * ncomp + c) * bytesPerSample;
            size_t base1 = ((size_t)i1 * ncomp + c) * bytesPerSample;
            double v0 = sampleComponent(data, base0, bits);
            double v1 = sampleComponent(data, base1, bits);
            double v = v0 + (v1 - v0) * frac;
            if (range && (int)range->arr.size() >= 2 * (c + 1)) {
                double lo = num(doc->resolve(range->arr[2 * c]), 0);
                double hi = num(doc->resolve(range->arr[2 * c + 1]), 1);
                v = lo + (hi - lo) * v;
            }
            out[c] = v;
        }
        return true;
    }
    return false;   // type 4 (PostScript calculator) not supported
}

struct Interp {
    Document* doc;
    Canvas* cv;
    RenderStats* st;
    int depth = 0;
    std::vector<GState> gstack;
    std::vector<TextItem>* textSink = nullptr;
    Mat devToPage;      // device pixels -> page points (y up)

    void exec(const std::string& data, ObjP resources, GState gs);
    void doXObject(ObjP ref, ObjP resources, GState& gs, const std::string& name);
    void drawImage(ObjP io, GState& gs);
    void showText(const std::string& bytes, GState& gs, Mat& tmBase,
                  double fs, double tc, double tw, double th, int renderMode, double rise,
                  const Font* font, const std::string& embedded);
    void paintPath(PathBuilder& pb, bool strokeIt, bool fillIt, bool evenOdd, GState& gs);
    void paintShading(ObjP shadingName, ObjP resources, GState& gs);
};

static void toDeviceRect(const Mat& m, double& x0, double& y0, double& x1, double& y1,
                         double& ux, double& uy, double& vx, double& vy) {
    m.apply(0, 0, x0, y0);
    m.apply(1, 1, x1, y1);
    m.apply(1, 0, ux, uy);
    m.apply(0, 1, vx, vy);
}

void Interp::paintPath(PathBuilder& pb, bool strokeIt, bool fillIt, bool evenOdd, GState& gs) {
    Polys polys = pb.take();
    if (polys.empty()) return;
    st->paths++;
    if (fillIt) {
        FillPolys(*cv, polys, evenOdd, gs.fill[0], gs.fill[1], gs.fill[2], gs.fillA,
                  gs.clip ? gs.clip.get() : nullptr);
        st->fills++;
    }
    if (strokeIt) {
        Polys outline;
        std::vector<std::pair<Poly, bool>> dashed;
        double sc = ctmScale(gs.ctm);
        std::vector<double> dash = gs.dash;
        for (double& d : dash) d *= sc;
        ApplyDash(polys, dash, gs.dashPhase * sc, false, dashed);
        for (auto& dp : dashed) StrokePolys(outline, dp.first, (float)(gs.lw * sc), gs.cap, dp.second);
        FillPolys(*cv, outline, false, gs.stroke[0], gs.stroke[1], gs.stroke[2], gs.strokeA,
                  gs.clip ? gs.clip.get() : nullptr);
        st->strokes++;
    }
}

void Interp::showText(const std::string& bytes, GState& gs, Mat& tmBase,
                      double fs, double tc, double tw, double th, int renderMode, double rise,
                      const Font* font, const std::string& embedded) {
    if (!font) return;
    const std::string& emb = font->embedded;
    FacePair fp = facePairFor(font);
    SubFace* face = fp.primary ? fp.primary : fp.fallback;
    if (!face) return;
    auto glyphFor = [&](int code, int uni, SubFace*& chosen) -> int {
        SubFace* cands[2] = {fp.primary, fp.fallback};
        for (int c = 0; c < 2; c++) {
            SubFace* f = cands[c];
            if (!f) continue;
            int g = 0;
            if (font->symbolic && !emb.empty()) {
                int rel = code - font->firstChar + 1;
                if (rel >= 1 && rel < f->info.numGlyphs) g = rel;
                else if (code >= 1 && code < f->info.numGlyphs) g = code;
            }
            if (!g && uni) g = stbtt_FindGlyphIndex(&f->info, uni);
            if (!g && uni == 0 && code > 32 && code < 256) g = stbtt_FindGlyphIndex(&f->info, code);
            if (g) { chosen = f; return g; }
        }
        chosen = face;
        return 0;
    };
    st->texts++;
    std::vector<TextItem>* out = textSink;
    int step = (font->subtype == "Type0") ? 2 : 1;
    // matrix from glyph units (font units, 1 em = unitsPerEm) to device:
    //   glyph -> em: scale by fs/upem  (we use stbtt scale on glyph space)
    //   then text matrix, then ctm, with Tz (horizontal) and Ts (rise)
    for (size_t i = 0; i + step <= bytes.size(); i += step) {
        int code = (unsigned char)bytes[i];
        if (step == 2) code = (code << 8) | (unsigned char)bytes[i + 1];
        int uni = font->unicodeFor(code);
        int gid = glyphFor(code, uni, face);

        // text rendering matrix: [fs*th 0 0 fs 0 rise] x Tm x CTM
        Mat trm = Mat(fs * th, 0, 0, fs, 0, rise).mul(tmBase).mul(gs.ctm);
        Mat toPage = trm.mul(devToPage);

        if (gid) {
            // glyph outline in font units -> device, flattened
            stbtt_vertex* verts = nullptr;
            int nv = stbtt_GetGlyphShape(&face->info, gid, &verts);
            if (verts && nv > 0) {
                float upem = 1.0f / std::max(1e-6f, stbtt_ScaleForMappingEmToPixels(&face->info, 1.0f));
                if (upem <= 0) upem = 1000.f;
                float k = 1.f / upem;
                PathBuilder gpb(trm);
                auto tpt = [&](float x, float y, float& ox, float& oy) {
                    double dx, dy;
                    trm.apply(x * k, y * k, dx, dy);
                    ox = (float)dx; oy = (float)dy;
                };
                bool penDown = false;
                for (int v = 0; v < nv; v++) {
                    stbtt_vertex& vv = verts[v];
                    float gx, gy;
                    tpt(vv.x, vv.y, gx, gy);
                    if (vv.type == STBTT_vmove) {
                        gpb.flush();          // previous contour
                        gpb.cur.push_back({gx, gy});
                        gpb.open = true;
                        penDown = true;
                    } else if (vv.type == STBTT_vline) {
                        if (penDown) gpb.cur.push_back({gx, gy});
                    } else if (vv.type == STBTT_vcurve) {
                        float cx, cy;
                        tpt(vv.cx, vv.cy, cx, cy);
                        if (penDown && !gpb.cur.empty()) {
                            Pt p0 = gpb.cur.back();
                            // quadratic -> cubic
                            float c1x = p0.x + 2.f / 3.f * (cx - p0.x), c1y = p0.y + 2.f / 3.f * (cy - p0.y);
                            float c2x = gx + 2.f / 3.f * (cx - gx), c2y = gy + 2.f / 3.f * (cy - gy);
                            gpb.flattenCubic(p0.x, p0.y, c1x, c1y, c2x, c2y, gx, gy, 0);
                        } else if (penDown) gpb.cur.push_back({gx, gy});
                    } else if (vv.type == STBTT_vcubic) {
                        float c1x, c1y, c2x, c2y;
                        tpt(vv.cx, vv.cy, c1x, c1y);
                        tpt(vv.cx1, vv.cy1, c2x, c2y);
                        if (penDown && !gpb.cur.empty()) {
                            Pt p0 = gpb.cur.back();
                            gpb.flattenCubic(p0.x, p0.y, c1x, c1y, c2x, c2y, gx, gy, 0);
                        } else if (penDown) gpb.cur.push_back({gx, gy});
                    }
                }
                // implicit close of each contour per TrueType semantics
                gpb.flush();
                Polys polys = gpb.take();
                if (!polys.empty()) {
                    if (renderMode == 0 || renderMode == 2 || renderMode == 4 || renderMode == 6)
                        FillPolys(*cv, polys, false, gs.fill[0], gs.fill[1], gs.fill[2], gs.fillA,
                                  gs.clip ? gs.clip.get() : nullptr);
                    if (renderMode == 1 || renderMode == 2 || renderMode == 5 || renderMode == 6) {
                        Polys outline;
                        float dlw = (float)(gs.lw * ctmScale(trm));
                        for (auto& p : polys) StrokePolys(outline, p, dlw, gs.cap, true);
                        FillPolys(*cv, outline, false, gs.stroke[0], gs.stroke[1], gs.stroke[2], gs.strokeA,
                  gs.clip ? gs.clip.get() : nullptr);
                    }
                    st->glyphs++;
                }
                stbtt_FreeShape(&face->info, verts);
            }
        }

        // ── text layer ──────────────────────────────────────────────────
        if (out && !(renderMode == 3 || renderMode == 7)) {
            float gx0 = 0, gy0 = 0, gx1 = (float)font->widthFor(code) / 1000.f, gy1 = 0.7f;
            if (gid) {
                int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
                if (stbtt_GetGlyphBox(&face->info, gid, &bx0, &by0, &bx1, &by1)) {
                    // font units -> em: 1/unitsPerEm == ScaleForMappingEmToPixels(1.0)
                    float up = stbtt_ScaleForMappingEmToPixels(&face->info, 1.0f);
                    gx0 = bx0 * up; gy0 = by0 * up;
                    gx1 = bx1 * up; gy1 = by1 * up;
                    if (gx1 - gx0 < 0.02f) gx1 = gx0 + (float)font->widthFor(code) / 1000.f;
                }
            }
            // into device pixels first (that is where the CTM chain is known),
            // then back to page points so the UI can map it without the form's
            // private coordinate system
            double ax0, ay0, ax1, ay1, bx2, by2, cx, cy;
            double dx0, dy0, dx1, dy1, dx2, dy2, dx3, dy3;
            trm.apply(gx0, gy0, dx0, dy0);
            trm.apply(gx1, gy0, dx1, dy1);
            trm.apply(gx1, gy1, dx2, dy2);
            trm.apply(gx0, gy1, dx3, dy3);
            devToPage.apply(dx0, dy0, ax0, ay0);
            devToPage.apply(dx1, dy1, ax1, ay1);
            devToPage.apply(dx2, dy2, bx2, by2);
            devToPage.apply(dx3, dy3, cx, cy);
            TextItem it;
            it.fs = (float)fs;
            {
                double px0, py0, px1, py1;
                toPage.apply(0, 0, px0, py0);
                double advUser = (font->widthFor(code) / 1000.0 * fs + tc) * th;
                if (step == 1 && code == 32) advUser += tw * th;
                // `advUser` is already in text-space points (the em advance times
                // fs), so measure it through the text matrix without scaling by
                // fs a second time: toPage includes trm, which carries fs.
                Mat toPagePts = tmBase.mul(gs.ctm).mul(devToPage);
                toPagePts.apply(advUser, 0, px1, py1);
                it.ox = (float)px0; it.oy = (float)py0;
                it.adv = (float)std::sqrt((px1 - px0) * (px1 - px0) + (py1 - py0) * (py1 - py0));
            }
            it.x0 = (float)std::min(std::min(ax0, bx2), std::min(ax1, cx));
            it.x1 = (float)std::max(std::max(ax0, bx2), std::max(ax1, cx));
            it.y0 = (float)std::min(std::min(ay0, ay1), std::min(by2, cy));
            it.y1 = (float)std::max(std::max(ay0, ay1), std::max(by2, cy));
            if (uni > 0) {
                // UTF-8 encode
                if (uni < 0x80) it.text += (char)uni;
                else if (uni < 0x800) { it.text += (char)(0xC0 | (uni >> 6)); it.text += (char)(0x80 | (uni & 0x3F)); }
                else { it.text += (char)(0xE0 | (uni >> 12)); it.text += (char)(0x80 | ((uni >> 6) & 0x3F)); it.text += (char)(0x80 | (uni & 0x3F)); }
            }
            out->push_back(it);
        }
        // advance in text space
        double w0 = font->widthFor(code) / 1000.0;
        double adv = (w0 * fs + tc) * th;
        if (step == 1 && code == 32) adv += tw * th;
        // Tm moves by adv in text space
        // tmBase is [..] with translation = current pen; translate x by adv
        // (Tj advances the text matrix: Td(adv,0) after each glyph)
        // We rebuild tmBase for the next glyph:
        tmBase = Mat(1, 0, 0, 1, adv, 0).mul(tmBase);
    }
}

void Interp::paintShading(ObjP shadingName, ObjP resources, GState& gs) {
    if (!resources || !shadingName || shadingName->t != O_NAME) return;
    ObjP shadings = doc->resolve(resources->get("Shading"));
    if (!shadings) return;
    ObjP sh = doc->resolve(shadings->get(shadingName->str.c_str()));
    if (!sh) { st->note("shading missing"); return; }
    int type = (int)num(doc->resolve(sh->get("ShadingType")), 0);
    if (type != 2 && type != 3) { st->note("shading type " + std::to_string(type)); return; }

    // colour components from the shading's colour space
    int ncomp = 3;
    ObjP cs = doc->resolve(sh->get("ColorSpace"));
    if (cs) {
        if (cs->t == O_NAME) {
            if (cs->str == "DeviceGray") ncomp = 1;
            else if (cs->str == "DeviceCMYK") ncomp = 4;
        } else if (cs->t == O_ARR && !cs->arr.empty()) {
            std::string fam = objName(doc->resolve(cs->arr[0]));
            if (fam == "ICCBased" && cs->arr.size() > 1) {
                ObjP stm = doc->resolve(cs->arr[1]);
                if (stm) ncomp = (int)num(doc->resolve(stm->get("N")), 3);
            } else if (fam == "CalGray") ncomp = 1;
        }
    }
    if (ncomp < 1 || ncomp > 4) ncomp = 3;

    ObjP coords = doc->resolve(sh->get("Coords"));
    if (!coords || coords->arr.size() < 4) { st->note("shading coords"); return; }
    double x0 = num(doc->resolve(coords->arr[0]), 0), y0 = num(doc->resolve(coords->arr[1]), 0);
    double x1 = num(doc->resolve(coords->arr[2]), 0), y1 = num(doc->resolve(coords->arr[3]), 0);
    double r0 = coords->arr.size() >= 6 ? num(doc->resolve(coords->arr[4]), 0) : 0;
    double r1 = coords->arr.size() >= 6 ? num(doc->resolve(coords->arr[5]), 0) : 1;
    ObjP fn = doc->resolve(sh->get("Function"));
    ObjP extendObj = doc->resolve(sh->get("Extend"));

    // device-space axis
    double dx0, dy0, dx1, dy1;
    gs.ctm.apply(x0, y0, dx0, dy0);
    gs.ctm.apply(x1, y1, dx1, dy1);
    double scale = ctmScale(gs.ctm);
    double ax = dx1 - dx0, ay = dy1 - dy0;
    double axisLen2 = ax * ax + ay * ay;
    if (axisLen2 < 1e-9 && type == 2) { st->note("degenerate shading"); return; }

    ShadingFn sfn{doc};
    double col[4] = {0, 0, 0, 1};

    for (int y = 0; y < cv->h; y++) {
        for (int x = 0; x < cv->w; x++) {
            float a = 1.f;
            if (gs.clip) {
                a = (*gs.clip)[(size_t)y * cv->w + x] * (1.f / 255.f);
                if (a <= 0.002f) continue;
            }
            double px = x + 0.5, py = y + 0.5;
            double t;
            if (type == 2) {
                t = ((px - dx0) * ax + (py - dy0) * ay) / axisLen2;
            } else {
                double vx = px - dx0, vy = py - dy0;
                double dist = sqrt(vx * vx + vy * vy);
                double rr = r1 * scale - r0 * scale;
                t = fabs(rr) > 1e-9 ? (dist - r0 * scale) / rr : 0.0;
            }
            if (t < 0 || t > 1) {
                bool ext0 = extendObj && extendObj->arr.size() >= 2 && num(doc->resolve(extendObj->arr[0]), 0) != 0;
                bool ext1 = extendObj && extendObj->arr.size() >= 2 && num(doc->resolve(extendObj->arr[1]), 0) != 0;
                if (t < 0 && !ext0) continue;
                if (t > 1 && !ext1) continue;
                t = t < 0 ? 0 : 1;
            }
            if (!sfn.eval(fn, t, col, ncomp)) continue;
            float r, g, b;
            if (ncomp == 1) {
                r = g = b = (float)col[0];
            } else if (ncomp == 4) {
                float rgb[3];
                cmykToRgb(col[0], col[1], col[2], col[3], rgb);
                r = rgb[0]; g = rgb[1]; b = rgb[2];
            } else {
                r = (float)col[0]; g = (float)col[1]; b = (float)col[2];
            }
            cv->blend(x, y, r, g, b, a);
        }
    }
    st->fills++;
}

void Interp::doXObject(ObjP ref, ObjP resources, GState& gs, const std::string& name) {
    ObjP xo = doc->resolve(ref);
    if (!xo || xo->t != O_DICT) return;
    std::string subtype = objName(doc->resolve(xo->get("Subtype")));
    if (subtype == "Image") { drawImage(xo, gs); return; }
    if (subtype != "Form") {
        st->note("xobject " + subtype);
        return;
    }
    if (depth > 6) return;
    ObjP res = doc->resolve(xo->get("Resources"));
    if (!res) res = resources;
    Mat m;
    ObjP mx = doc->resolve(xo->get("Matrix"));
    if (mx && mx->t == O_ARR && mx->arr.size() == 6) {
        m = Mat(num(doc->resolve(mx->arr[0]), 1), num(doc->resolve(mx->arr[1]), 0),
                num(doc->resolve(mx->arr[2]), 0), num(doc->resolve(mx->arr[3]), 1),
                num(doc->resolve(mx->arr[4]), 0), num(doc->resolve(mx->arr[5]), 0));
    }
    std::string data;
    if (!doc->decodeStream(xo, data)) { st->note("form stream"); return; }
    GState sub = gs;
    sub.ctm = m.mul(gs.ctm);
    depth++;
    exec(data, res, sub);
    depth--;
    (void)name;
}

void Interp::drawImage(ObjP io, GState& gs) {
    st->images++;
    ObjP wObj = doc->resolve(io->get("Width"));
    ObjP hObj = doc->resolve(io->get("Height"));
    int iw = (int)num(wObj, 0), ih = (int)num(hObj, 0);
    int bpc = (int)num(doc->resolve(io->get("BitsPerComponent")), 8);
    if (iw <= 0 || ih <= 0 || iw > 20000 || ih > 20000) { st->note("image size"); return; }

    ObjP csObj = doc->resolve(io->get("ColorSpace"));
    int ncomp = 3;
    if (csObj) {
        if (csObj->t == O_NAME) {
            if (csObj->str == "DeviceGray" || csObj->str == "G") ncomp = 1;
            else if (csObj->str == "DeviceCMYK") ncomp = 4;
            else ncomp = 3;
        } else if (csObj->t == O_ARR && !csObj->arr.empty()) {
            std::string fam = objName(doc->resolve(csObj->arr[0]));
            if (fam == "ICCBased" && csObj->arr.size() > 1) {
                ObjP stm = doc->resolve(csObj->arr[1]);
                if (stm) ncomp = (int)num(doc->resolve(stm->get("N")), 3);
            } else if (fam == "CalGray") ncomp = 1;
            else if (fam == "CalRGB" || fam == "Lab") ncomp = 3;
            else if (fam == "Indexed") ncomp = 1;
        }
    }
    std::string raw;
    if (!doc->decodeStream(io, raw)) { st->note("image filter (DCT/JPX)"); return; }

    size_t expected = (size_t)iw * ih * ncomp * bpc / 8;
    if (raw.size() < expected) { st->note("image data short"); return; }

    auto sample = [&](int x, int y, int c) -> float {
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= iw) x = iw - 1;
        if (y >= ih) y = ih - 1;
        if (bpc == 8) {
            size_t idx = ((size_t)y * iw + x) * ncomp + c;
            return (unsigned char)raw[idx] / 255.f;
        }
        if (bpc == 1) {
            size_t bit = ((size_t)y * iw + x) * ncomp + c;
            int byte = (unsigned char)raw[bit >> 3];
            return ((byte >> (7 - (bit & 7))) & 1) ? 1.f : 0.f;
        }
        return 1.f;
    };

    // unit square -> device parallelogram
    double x0, y0, x1, y1, ux, uy, vx, vy;
    toDeviceRect(gs.ctm, x0, y0, x1, y1, ux, uy, vx, vy);
    double ex = ux - x0, ey = uy - y0;      // u axis
    double fx = vx - x0, fy = vy - y0;      // v axis
    double det = ex * fy - ey * fx;
    if (fabs(det) < 1e-9) return;
    double minx = std::min(std::min(x0, x1), std::min(ux, vx));
    double maxx = std::max(std::max(x0, x1), std::max(ux, vx));
    double miny = std::min(std::min(y0, y1), std::min(uy, vy));
    double maxy = std::max(std::max(y0, y1), std::max(uy, vy));
    int px0 = std::max(0, (int)floor(minx)), px1 = std::min(cv->w, (int)ceil(maxx));
    int py0 = std::max(0, (int)floor(miny)), py1 = std::min(cv->h, (int)ceil(maxy));

    for (int y = py0; y < py1; y++) {
        for (int x = px0; x < px1; x++) {
            double cx = x + 0.5 - x0, cy = y + 0.5 - y0;
            double u = (cx * fy - cy * fx) / det;
            double v = (ex * cy - ey * cx) / det;
            if (u < 0 || u > 1 || v < 0 || v > 1) continue;
            int sx = (int)(u * iw), sy = (int)(v * ih);
            float r, g, b;
            if (ncomp == 1) { r = g = b = sample(sx, sy, 0); }
            else if (ncomp == 4) {
                float c = sample(sx, sy, 0), m = sample(sx, sy, 1), yy = sample(sx, sy, 2), k = sample(sx, sy, 3);
                r = (1 - c) * (1 - k); g = (1 - m) * (1 - k); b = (1 - yy) * (1 - k);
            } else { r = sample(sx, sy, 0); g = sample(sx, sy, 1); b = sample(sx, sy, 2); }
            float a = 1.f;
            if (gs.clip) a *= (*gs.clip)[(size_t)y * cv->w + x] * (1.f / 255.f);
            if (a > 0) cv->blend(x, y, r, g, b, a);
        }
    }
}

void Interp::exec(const std::string& data, ObjP resources, GState gs) {
    std::vector<ObjP> stack;
    stack.reserve(64);
    PathBuilder pb(gs.ctm);
    bool pathHasSegs = false;

    ObjP fonts = nullptr, xobjs = nullptr, extg = nullptr, colorspaces = nullptr;
    if (resources) {
        fonts = doc->resolve(resources->get("Font"));
        xobjs = doc->resolve(resources->get("XObject"));
        extg = doc->resolve(resources->get("ExtGState"));
        colorspaces = doc->resolve(resources->get("ColorSpace"));
    }

    // text state
    const Font* curFont = nullptr;
    std::string curFontEmbedded;
    double Tfs = 0, Ttc = 0, Ttw = 0, Tth = 1, TL = 0, Ts = 0;
    int Tr = 0;
    Mat Tm, Tlm;

    auto numArg = [&](int back, double def) -> double {
        if ((int)stack.size() < back) return def;
        return num(stack[stack.size() - back], def);
    };
    std::string lastPathName;

    ContentLexer lx(data.data(), data.size());
    Tok t;
    while (true) {
        lx.next(t);
        if (t.type == T_EOF) break;
        if (t.type == T_NUM) { stack.push_back(mkReal(t.num)); continue; }
        if (t.type == T_STR) { stack.push_back(mkStr(t.s)); continue; }
        if (t.type == T_NAME) { stack.push_back(mkName(t.s)); continue; }
        if (t.type == T_ARR_OPEN) {
            auto a = mkArr();
            while (true) {
                Tok a2;
                lx.next(a2);
                if (a2.type == T_ARR_CLOSE || a2.type == T_EOF) break;
                if (a2.type == T_NUM) a->arr.push_back(mkReal(a2.num));
                else if (a2.type == T_STR) a->arr.push_back(mkStr(a2.s));
                else if (a2.type == T_NAME) a->arr.push_back(mkName(a2.s));
            }
            stack.push_back(a);
            continue;
        }
        if (t.type != T_OP) continue;

        const std::string& op = t.s;
        st->ops++;

        if (op == "q") {
            if (gstack.size() < 48) gstack.push_back(gs);
            pb = PathBuilder(gs.ctm);
        } else if (op == "Q") {
            if (!gstack.empty()) { gs = gstack.back(); gstack.pop_back(); pb = PathBuilder(gs.ctm); }
        } else if (op == "cm") {
            Mat m(numArg(6, 1), numArg(5, 0), numArg(4, 0), numArg(3, 1), numArg(2, 0), numArg(1, 0));
            gs.ctm = m.mul(gs.ctm);
            pb.ctm = gs.ctm;
        } else if (op == "w") {
            gs.lw = numArg(1, 1);
        } else if (op == "J") {
            gs.cap = (int)numArg(1, 0);
        } else if (op == "j") {
            gs.join = (int)numArg(1, 0);
        } else if (op == "M") {
            gs.miter = numArg(1, 10);
        } else if (op == "d") {
            gs.dash.clear();
            if (!stack.empty() && stack.back()->t == O_ARR)
                for (auto& e : stack.back()->arr) gs.dash.push_back(num(doc->resolve(e), 0));
            gs.dashPhase = numArg(2, 0);
        } else if (op == "ri" || op == "i") {
            // ignore rendering intent / flatness
        } else if (op == "gs") {
            if (!stack.empty() && stack.back()->t == O_NAME && extg) {
                ObjP g = doc->resolve(extg->get(stack.back()->str.c_str()));
                if (g) {
                    ObjP v = doc->resolve(g->get("ca"));
                    if (v) gs.fillA = (float)num(v, 1);
                    v = doc->resolve(g->get("CA"));
                    if (v) gs.strokeA = (float)num(v, 1);
                    v = doc->resolve(g->get("LW"));
                    if (v) gs.lw = num(v, gs.lw);
                    v = doc->resolve(g->get("LC"));
                    if (v) gs.cap = (int)num(v, gs.cap);
                    v = doc->resolve(g->get("LJ"));
                    if (v) gs.join = (int)num(v, gs.join);
                    v = doc->resolve(g->get("D"));
                    if (v && v->t == O_ARR && v->arr.size() >= 2) {
                        gs.dash.clear();
                        ObjP arr = doc->resolve(v->arr[0]);
                        if (arr) for (auto& e : arr->arr) gs.dash.push_back(num(doc->resolve(e), 0));
                        gs.dashPhase = num(doc->resolve(v->arr[1]), 0);
                    }
                }
            }
        } else if (op == "m") {
            pb.moveTo(numArg(2, 0), numArg(1, 0)); pathHasSegs = true;
        } else if (op == "l") {
            pb.lineTo(numArg(2, 0), numArg(1, 0)); pathHasSegs = true;
        } else if (op == "c") {
            pb.curveTo(numArg(6, 0), numArg(5, 0), numArg(4, 0), numArg(3, 0), numArg(2, 0), numArg(1, 0));
            pathHasSegs = true;
        } else if (op == "v") {
            pb.curveTo(pb.lastUx, pb.lastUy, numArg(4, 0), numArg(3, 0), numArg(2, 0), numArg(1, 0));
            pathHasSegs = true;
        } else if (op == "y") {
            double ex = numArg(2, 0), ey = numArg(1, 0);
            pb.curveTo(numArg(4, 0), numArg(3, 0), ex, ey, ex, ey);
            pathHasSegs = true;
        } else if (op == "h") {
            pb.close();
        } else if (op == "re") {
            double x = numArg(4, 0), y = numArg(3, 0), w = numArg(2, 0), h = numArg(1, 0);
            pb.moveTo(x, y);
            pb.lineTo(x + w, y);
            pb.lineTo(x + w, y + h);
            pb.lineTo(x, y + h);
            pb.close();
            pathHasSegs = true;
        } else if (op == "S" || op == "s") {
            paintPath(pb, true, false, false, gs);
            pb = PathBuilder(gs.ctm);
            pathHasSegs = false;
        } else if (op == "f" || op == "F" || op == "f*") {
            paintPath(pb, false, true, op == "f*", gs);
            pb = PathBuilder(gs.ctm);
            pathHasSegs = false;
        } else if (op == "B" || op == "B*") {
            paintPath(pb, true, true, op == "B*", gs);
            pb = PathBuilder(gs.ctm);
            pathHasSegs = false;
        } else if (op == "b" || op == "b*") {
            pb.close();
            paintPath(pb, true, true, op == "b*", gs);
            pb = PathBuilder(gs.ctm);
            pathHasSegs = false;
        } else if (op == "n") {
            pb = PathBuilder(gs.ctm);
            pathHasSegs = false;
        } else if (op == "W" || op == "W*") {
            Polys polys = pb.take();
            if (!polys.empty()) gs.clip = ClipPolys(*cv, gs.clip, polys, op == "W*");
            // the clip path stays current until the painting operator consumes it
            pb = PathBuilder(gs.ctm);
            for (auto& p : polys) pb.polys.push_back(p);
            pathHasSegs = !polys.empty();
        } else if (op == "BT") {
            Tm = Mat(); Tlm = Mat();
            Tfs = 0; Ttc = 0; Ttw = 0; Tth = 1; TL = 0; Ts = 0; Tr = 0;
        } else if (op == "ET") {
        } else if (op == "Tc") {
            Ttc = numArg(1, 0);
        } else if (op == "Tw") {
            Ttw = numArg(1, 0);
        } else if (op == "Tz") {
            Tth = numArg(1, 100) / 100.0;
        } else if (op == "TL") {
            TL = numArg(1, 0);
        } else if (op == "Ts") {
            Ts = numArg(1, 0);
        } else if (op == "Tr") {
            Tr = (int)numArg(1, 0);
        } else if (op == "Tf") {
            if (stack.size() >= 2) {
                ObjP nameObj = stack[stack.size() - 2];
                Tfs = numArg(1, 0);
                if (nameObj && nameObj->t == O_NAME && fonts) {
                    ObjP fd = fonts->get(nameObj->str.c_str());
                    curFont = doc->font(fd);
                    curFontEmbedded = curFont ? curFont->embedded : std::string();
                }
            }
        } else if (op == "Td") {
            double tx = numArg(2, 0), ty = numArg(1, 0);
            Tlm = Mat(1, 0, 0, 1, tx, ty).mul(Tlm);
            Tm = Tlm;
        } else if (op == "TD") {
            double tx = numArg(2, 0), ty = numArg(1, 0);
            TL = -ty;
            Tlm = Mat(1, 0, 0, 1, tx, ty).mul(Tlm);
            Tm = Tlm;
        } else if (op == "Tm") {
            Tlm = Mat(numArg(6, 1), numArg(5, 0), numArg(4, 0), numArg(3, 1), numArg(2, 0), numArg(1, 0));
            Tm = Tlm;
        } else if (op == "T*") {
            Tlm = Mat(1, 0, 0, 1, 0, -TL).mul(Tlm);
            Tm = Tlm;
        } else if (op == "Tj") {
            if (!stack.empty() && stack.back()->t == O_STR && curFont)
                showText(stack.back()->str, gs, Tm, Tfs, Ttc, Ttw, Tth, Tr, Ts, curFont, curFontEmbedded);
        } else if (op == "'") {
            Tlm = Mat(1, 0, 0, 1, 0, -TL).mul(Tlm);
            Tm = Tlm;
            if (!stack.empty() && stack.back()->t == O_STR && curFont)
                showText(stack.back()->str, gs, Tm, Tfs, Ttc, Ttw, Tth, Tr, Ts, curFont, curFontEmbedded);
        } else if (op == "\"") {
            if (stack.size() >= 3) {
                Ttw = numArg(3, 0);
                Ttc = numArg(2, 0);
                Tlm = Mat(1, 0, 0, 1, 0, -TL).mul(Tlm);
                Tm = Tlm;
                if (stack.back()->t == O_STR && curFont)
                    showText(stack.back()->str, gs, Tm, Tfs, Ttc, Ttw, Tth, Tr, Ts, curFont, curFontEmbedded);
            }
        } else if (op == "TJ") {
            if (!stack.empty() && stack.back()->t == O_ARR && curFont) {
                // walk the array, moving the text matrix between strings
                Mat tm = Tm;
                for (auto& e : stack.back()->arr) {
                    ObjP r = doc->resolve(e);
                    if (!r) continue;
                    if (r->t == O_STR) {
                        showText(r->str, gs, tm, Tfs, Ttc, Ttw, Tth, Tr, Ts, curFont, curFontEmbedded);
                        // showText advanced `tm` in place through const_cast
                    } else if (r->t == O_INT || r->t == O_REAL) {
                        double adj = -r->num / 1000.0 * Tfs * Tth;
                        tm = Mat(1, 0, 0, 1, adj, 0).mul(tm);
                    }
                }
                Tm = tm;
            }
        } else if (op == "Do") {
            if (!stack.empty() && stack.back()->t == O_NAME && xobjs) {
                ObjP ref = xobjs->get(stack.back()->str.c_str());
                if (ref) doXObject(ref, resources, gs, stack.back()->str);
            }
        } else if (op == "sh") {
            if (!stack.empty() && stack.back()->t == O_NAME) paintShading(stack.back(), resources, gs);
            else st->note("shading");
        } else if (op == "BI") {
            // inline image: skip to EI
            st->note("inline image");
            const char* q = lx.p;
            while (q + 1 < lx.end && !(q[0] == 'E' && q[1] == 'I')) ++q;
            lx.p = (q + 2 <= lx.end) ? q + 2 : lx.end;
        } else if (op == "CS" || op == "cs") {
            if (!stack.empty() && stack.back()->t == O_NAME) lastPathName = stack.back()->str;
        } else if (op == "G") {
            float v = (float)numArg(1, 0);
            gs.stroke[0] = gs.stroke[1] = gs.stroke[2] = v;
        } else if (op == "g") {
            float v = (float)numArg(1, 0);
            gs.fill[0] = gs.fill[1] = gs.fill[2] = v;
        } else if (op == "RG") {
            gs.stroke[0] = (float)numArg(3, 0);
            gs.stroke[1] = (float)numArg(2, 0);
            gs.stroke[2] = (float)numArg(1, 0);
        } else if (op == "rg") {
            gs.fill[0] = (float)numArg(3, 0);
            gs.fill[1] = (float)numArg(2, 0);
            gs.fill[2] = (float)numArg(1, 0);
        } else if (op == "K") {
            cmykToRgb(numArg(4, 0), numArg(3, 0), numArg(2, 0), numArg(1, 0), gs.stroke);
        } else if (op == "k") {
            cmykToRgb(numArg(4, 0), numArg(3, 0), numArg(2, 0), numArg(1, 0), gs.fill);
        } else if (op == "SC" || op == "SCN" || op == "sc" || op == "scn") {
            // colour by component count (patterns are skipped)
            if (!stack.empty() && stack.back()->t == O_NAME) {
                st->note("pattern colour");
            } else {
                float* dst = (op == "SC" || op == "SCN") ? gs.stroke : gs.fill;
                if (stack.size() == 1) {
                    float v = (float)numArg(1, 0);
                    dst[0] = dst[1] = dst[2] = v;
                } else if (stack.size() == 3) {
                    dst[0] = (float)numArg(3, 0); dst[1] = (float)numArg(2, 0); dst[2] = (float)numArg(1, 0);
                } else if (stack.size() >= 4) {
                    cmykToRgb(numArg(4, 0), numArg(3, 0), numArg(2, 0), numArg(1, 0), dst);
                }
            }
        } else {
            // unknown operator: ignore quietly (d0/d1/BX/EX/MP/DP...)
        }
        stack.clear();
    }
    (void)pathHasSegs;
    (void)colorspaces;
    (void)lastPathName;
}

} // namespace

// ─── public entry point ─────────────────────────────────────────────────────
bool Renderer::renderPage(int pageIndex, double scale, Image& out, RenderStats* st) {
    static RenderStats dummy;
    if (!st) st = &dummy;
    *st = RenderStats();
    if (!doc || !doc->loaded || pageIndex < 0 || pageIndex >= doc->pageCount()) return false;
    ObjP page = doc->pages[pageIndex];

    double bx0 = 0, by0 = 0, bx1 = 612, by1 = 792;
    ObjP box = doc->resolve(page->get("CropBox"));
    if (!box) box = doc->resolve(page->get("MediaBox"));
    if (box && box->t == O_ARR && box->arr.size() == 4) {
        bx0 = num(doc->resolve(box->arr[0]), 0);
        by0 = num(doc->resolve(box->arr[1]), 0);
        bx1 = num(doc->resolve(box->arr[2]), 612);
        by1 = num(doc->resolve(box->arr[3]), 792);
    }
    double pw = fabs(bx1 - bx0), ph = fabs(by1 - by0);
    if (pw < 1) pw = 612;
    if (ph < 1) ph = 792;
    int rotate = 0;
    {
        double w, h; int r;
        doc->pageSize(pageIndex, w, h, r);
        rotate = r;
    }
    bool swap = (rotate == 90 || rotate == 270);
    int dw = (int)lround((swap ? ph : pw) * scale);
    int dh = (int)lround((swap ? pw : ph) * scale);
    if (dw <= 0 || dh <= 0) return false;
    if ((long long)dw * dh > 40 * 1000 * 1000) return false;

    Canvas cv;
    cv.init(dw, dh);

    // page space (y up, origin at box lower-left) -> device (y down)
    Mat ctm;
    switch (rotate) {
        case 90:
            ctm = Mat(0, 1, 1, 0, -by0 * scale, -bx0 * scale);
            break;
        case 180:
            ctm = Mat(-1, 0, 0, -1, bx1 * scale, -by0 * scale);
            break;
        case 270:
            ctm = Mat(0, -1, -1, 0, by1 * scale, bx1 * scale);
            break;
        default:
            ctm = Mat(scale, 0, 0, -scale, -bx0 * scale, by1 * scale);
            break;
    }

    GState gs;
    gs.ctm = ctm;

    // page contents (may be an array of streams)
    std::string content;
    ObjP contents = doc->resolve(page->get("Contents"));
    if (contents) {
        if (contents->isStream) {
            doc->decodeStream(contents, content);
        } else if (contents->t == O_ARR) {
            for (auto& e : contents->arr) {
                ObjP c = doc->resolve(e);
                std::string part;
                if (c && doc->decodeStream(c, part)) { content += part; content += "\n"; }
            }
        }
    }

    Interp in;
    in.doc = doc;
    in.cv = &cv;
    in.st = st;
    items.clear();
    in.textSink = &items;
    in.devToPage = ctm.inverse();
    in.exec(content, doc->resolve(page->get("Resources")), gs);

    out.w = dw;
    out.h = dh;
    out.rgba.swap(cv.px);
    lastW = dw;
    lastH = dh;
    return true;
}

} // namespace pdf
