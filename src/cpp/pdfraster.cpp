#include "pdfraster.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdio>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "pdf_glyphs.h"
#include "fonts_data.h"
#include "iccp.hpp"   // metric-compatible document faces (Liberation/Carlito/Caladea)

namespace pdf {
namespace {

// ─── geometry ───────────────────────────────────────────────────────────────
struct Pt { float x, y; };
typedef std::vector<Pt> Poly;
typedef std::vector<Poly> Polys;

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

// ─── blend modes (PDF 32000-1 §11.3.5) ─────────────────────────────────────
enum BlendMode {
    BM_NORMAL = 0, BM_MULTIPLY, BM_SCREEN, BM_OVERLAY, BM_DARKEN, BM_LIGHTEN,
    BM_COLOR_DODGE, BM_COLOR_BURN, BM_HARD_LIGHT, BM_SOFT_LIGHT, BM_DIFFERENCE,
    BM_EXCLUSION, BM_HUE, BM_SATURATION, BM_COLOR, BM_LUMINOSITY
};

static inline int blendModeFromName(const std::string& n) {
    if (n == "Multiply") return BM_MULTIPLY;
    if (n == "Screen") return BM_SCREEN;
    if (n == "Overlay") return BM_OVERLAY;
    if (n == "Darken") return BM_DARKEN;
    if (n == "Lighten") return BM_LIGHTEN;
    if (n == "ColorDodge") return BM_COLOR_DODGE;
    if (n == "ColorBurn") return BM_COLOR_BURN;
    if (n == "HardLight") return BM_HARD_LIGHT;
    if (n == "SoftLight") return BM_SOFT_LIGHT;
    if (n == "Difference") return BM_DIFFERENCE;
    if (n == "Exclusion") return BM_EXCLUSION;
    if (n == "Hue") return BM_HUE;
    if (n == "Saturation") return BM_SATURATION;
    if (n == "Color") return BM_COLOR;
    if (n == "Luminosity") return BM_LUMINOSITY;
    return BM_NORMAL;   // Normal, Compatible, and anything unknown
}

static inline float bmClamp(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

static void rgbToHsl(float r, float g, float b, float& h, float& s, float& l) {
    float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
    l = (mx + mn) * 0.5f;
    float d = mx - mn;
    if (d < 1e-6f) { h = 0.f; s = 0.f; return; }
    s = l > 0.5f ? d / (2.f - mx - mn) : d / (mx + mn);
    if (mx == r) h = (g - b) / d + (g < b ? 6.f : 0.f);
    else if (mx == g) h = (b - r) / d + 2.f;
    else h = (r - g) / d + 4.f;
    h *= 1.f / 6.f;
}

static float hslHue(float m1, float m2, float h) {
    if (h < 0.f) h += 1.f;
    if (h > 1.f) h -= 1.f;
    if (h * 6.f < 1.f) return m1 + (m2 - m1) * h * 6.f;
    if (h * 2.f < 1.f) return m2;
    if (h * 3.f < 2.f) return m1 + (m2 - m1) * (2.f / 3.f - h) * 6.f;
    return m1;
}

static void hslToRgb(float h, float s, float l, float& r, float& g, float& b) {
    if (s < 1e-6f) { r = g = b = l; return; }
    float m2 = l <= 0.5f ? l * (1.f + s) : l + s - l * s;
    float m1 = 2.f * l - m2;
    r = hslHue(m1, m2, h + 1.f / 3.f);
    g = hslHue(m1, m2, h);
    b = hslHue(m1, m2, h - 1.f / 3.f);
}

// Blend function B(backdrop, source); rgb values 0..1, result in source.
static inline void blendRGB(int mode, float br, float bg, float bb,
                            float& sr, float& sg, float& sb) {
    switch (mode) {
        case BM_MULTIPLY:  sr = br * sr; sg = bg * sg; sb = bb * sb; break;
        case BM_SCREEN:    sr = br + sr - br * sr; sg = bg + sg - bg * sg; sb = bb + sb - bb * sb; break;
        case BM_OVERLAY:   // hard light with operands swapped
            sr = br <= 0.5f ? 2.f * sr * br : 1.f - 2.f * (1.f - sr) * (1.f - br);
            sg = bg <= 0.5f ? 2.f * sg * bg : 1.f - 2.f * (1.f - sg) * (1.f - bg);
            sb = bb <= 0.5f ? 2.f * sb * bb : 1.f - 2.f * (1.f - sb) * (1.f - bb);
            break;
        case BM_DARKEN:    sr = std::min(sr, br); sg = std::min(sg, bg); sb = std::min(sb, bb); break;
        case BM_LIGHTEN:   sr = std::max(sr, br); sg = std::max(sg, bg); sb = std::max(sb, bb); break;
        case BM_COLOR_DODGE:
            sr = br <= 0.f ? 0.f : (sr >= 1.f ? 1.f : std::min(1.f, br / (1.f - sr)));
            sg = bg <= 0.f ? 0.f : (sg >= 1.f ? 1.f : std::min(1.f, bg / (1.f - sg)));
            sb = bb <= 0.f ? 0.f : (sb >= 1.f ? 1.f : std::min(1.f, bb / (1.f - sb)));
            break;
        case BM_COLOR_BURN:
            sr = br >= 1.f ? 1.f : (sr <= 0.f ? 0.f : 1.f - std::min(1.f, (1.f - br) / sr));
            sg = bg >= 1.f ? 1.f : (sg <= 0.f ? 0.f : 1.f - std::min(1.f, (1.f - bg) / sg));
            sb = bb >= 1.f ? 1.f : (sb <= 0.f ? 0.f : 1.f - std::min(1.f, (1.f - bb) / sb));
            break;
        case BM_HARD_LIGHT:
            sr = sr <= 0.5f ? 2.f * sr * br : 1.f - 2.f * (1.f - sr) * (1.f - br);
            sg = sg <= 0.5f ? 2.f * sg * bg : 1.f - 2.f * (1.f - sg) * (1.f - bg);
            sb = sb <= 0.5f ? 2.f * sb * bb : 1.f - 2.f * (1.f - sb) * (1.f - bb);
            break;
        case BM_SOFT_LIGHT: {
            auto sl = [](float b, float s) {
                if (s <= 0.5f) return b - (1.f - 2.f * s) * b * (1.f - b);
                float d = b <= 0.25f ? ((16.f * b - 12.f) * b + 4.f) * b : std::sqrt(b);
                return b + (2.f * s - 1.f) * (d - b);
            };
            sr = sl(br, sr); sg = sl(bg, sg); sb = sl(bb, sb);
            break;
        }
        case BM_DIFFERENCE: sr = fabs(sr - br); sg = fabs(sg - bg); sb = fabs(sb - bb); break;
        case BM_EXCLUSION:  sr = sr + br - 2.f * sr * br; sg = sg + bg - 2.f * sg * bg; sb = sb + bb - 2.f * sb * bb; break;
        case BM_HUE: case BM_SATURATION: case BM_COLOR: case BM_LUMINOSITY: {
            float bh, bs, bl, sh, ss, sl;
            rgbToHsl(br, bg, bb, bh, bs, bl);
            rgbToHsl(sr, sg, sb, sh, ss, sl);
            float h = bh, s = bs, l = bl;
            switch (mode) {
                case BM_HUE:        h = sh; s = ss; l = bl; break;
                case BM_SATURATION: h = bh; s = ss; l = bl; break;
                case BM_COLOR:      h = sh; s = ss; l = bl; break;
                case BM_LUMINOSITY: h = bh; s = bs; l = sl; break;
            }
            hslToRgb(h, s, l, sr, sg, sb);
            break;
        }
        default: break;   // Normal
    }
    sr = bmClamp(sr); sg = bmClamp(sg); sb = bmClamp(sb);
}

// ─── canvas ─────────────────────────────────────────────────────────────────
typedef std::shared_ptr<std::vector<uint8_t>> ClipMask;

struct Canvas {
    int w = 0, h = 0;
    bool transparent = false;     // scratch/group buffers keep real alpha
    std::vector<uint8_t> px;      // RGBA, straight alpha, background opaque white

    void init(int width, int height) {
        w = width; h = height;
        transparent = false;
        px.assign((size_t)w * h * 4, 255);
    }
    void initTransparent(int width, int height) {
        w = width; h = height;
        transparent = true;
        px.assign((size_t)w * h * 4, 0);
    }
    inline void blend(int x, int y, float r, float g, float b, float a, int bm = 0) {
        if (a <= 0.f) return;
        if (a > 1.f) a = 1.f;
        uint8_t* p = &px[((size_t)y * w + x) * 4];
        if (!transparent) {
            float dr = p[0] * (1.f / 255.f), dg = p[1] * (1.f / 255.f), db = p[2] * (1.f / 255.f);
            if (bm) { float br = r, bg2 = g, bb = b; blendRGB(bm, dr, dg, db, br, bg2, bb); r = br; g = bg2; b = bb; }
            p[0] = (uint8_t)((dr * (1.f - a) + r * a) * 255.f + 0.5f);
            p[1] = (uint8_t)((dg * (1.f - a) + g * a) * 255.f + 0.5f);
            p[2] = (uint8_t)((db * (1.f - a) + b * a) * 255.f + 0.5f);
            p[3] = 255;
            return;
        }
        // straight alpha source-over on a transparent buffer
        float da = p[3] * (1.f / 255.f);
        float oa = a + da * (1.f - a);
        if (oa <= 0.f) { p[0] = p[1] = p[2] = p[3] = 0; return; }
        float dr = p[0] * (1.f / 255.f), dg = p[1] * (1.f / 255.f), db = p[2] * (1.f / 255.f);
        if (bm) { float br = r, bg2 = g, bb = b; blendRGB(bm, dr, dg, db, br, bg2, bb); r = br; g = bg2; b = bb; }
        p[0] = (uint8_t)(((r * a + dr * da * (1.f - a)) / oa) * 255.f + 0.5f);
        p[1] = (uint8_t)(((g * a + dg * da * (1.f - a)) / oa) * 255.f + 0.5f);
        p[2] = (uint8_t)(((b * a + db * da * (1.f - a)) / oa) * 255.f + 0.5f);
        p[3] = (uint8_t)(oa * 255.f + 0.5f);
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
                      const std::vector<uint8_t>* clip = nullptr, int bm = 0) {
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
            if (al > 0.f) cv.blend(x0 + i, y, r, g, b, al, bm);
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
    const unsigned char* src = nullptr;   // cache key: the caller's buffer
    const unsigned char* data = nullptr;
    unsigned int len = 0;
    std::vector<unsigned char> owned;   // patched copy when the file needs one
    bool tried = false, ok = false;
};

// Many embedded CID TrueType subsets ship without a `cmap` table (the CID
// mapping is the caller's job), and stb_truetype refuses to open a face
// without one. Splice in an identity cmap so the glyph outlines become
// reachable; selection already goes by glyph id, so the mapping itself is only
// there to satisfy the loader.
static std::vector<unsigned char> addIdentityCmap(const unsigned char* d, unsigned len) {
    auto be16 = [&](size_t p) -> unsigned { return (d[p] << 8) | d[p + 1]; };
    auto be32 = [&](size_t p) -> unsigned {
        return ((unsigned)d[p] << 24) | ((unsigned)d[p + 1] << 16) |
               ((unsigned)d[p + 2] << 8) | d[p + 3];
    };
    if (len < 12) return {};
    int n = (int)be16(4);
    if (n <= 0 || 12 + (size_t)n * 16 > len) return {};
    struct Tab { char tag[5] = {0}; unsigned off = 0, len = 0; };
    std::vector<Tab> tabs;
    unsigned numGlyphs = 0;
    for (int i = 0; i < n; i++) {
        const unsigned char* r = d + 12 + (size_t)i * 16;
        Tab t;
        memcpy(t.tag, r, 4);
        t.off = be32((size_t)(r - d) + 8);
        t.len = be32((size_t)(r - d) + 12);
        if (t.off > len || t.off + t.len > len) return {};
        if (memcmp(t.tag, "cmap", 4) == 0) return {};
        if (memcmp(t.tag, "maxp", 4) == 0 && t.len >= 6) numGlyphs = be16(t.off + 4);
        tabs.push_back(t);
    }
    if (numGlyphs == 0 || numGlyphs > 65535) return {};

    size_t cmapLen = 12 + 10 + (size_t)numGlyphs * 2;
    int newN = n + 1;
    int es = 0;
    while ((1 << (es + 1)) <= newN) es++;
    unsigned searchRange = (unsigned)(16 * (1 << es));
    unsigned rangeShift = (unsigned)(newN * 16) - searchRange;

    std::vector<unsigned char> out(12 + (size_t)newN * 16, 0);
    memcpy(out.data(), d, 12);
    out[4] = (unsigned char)(newN >> 8); out[5] = (unsigned char)(newN & 0xFF);
    out[6] = (unsigned char)(searchRange >> 8); out[7] = (unsigned char)(searchRange & 0xFF);
    out[8] = (unsigned char)(es >> 8); out[9] = (unsigned char)(es & 0xFF);
    out[10] = (unsigned char)(rangeShift >> 8); out[11] = (unsigned char)(rangeShift & 0xFF);

    size_t dirPos = 12;
    for (auto& t : tabs) {
        size_t at = (out.size() + 3) & ~(size_t)3;
        out.resize(at + t.len);
        memcpy(out.data() + at, d + t.off, t.len);
        // table record: tag, checkSum, offset, length
        memcpy(out.data() + dirPos, t.tag, 4);
        out[dirPos + 8] = (unsigned char)(at >> 24); out[dirPos + 9] = (unsigned char)(at >> 16);
        out[dirPos + 10] = (unsigned char)(at >> 8); out[dirPos + 11] = (unsigned char)at;
        out[dirPos + 12] = (unsigned char)(t.len >> 24); out[dirPos + 13] = (unsigned char)(t.len >> 16);
        out[dirPos + 14] = (unsigned char)(t.len >> 8); out[dirPos + 15] = (unsigned char)t.len;
        dirPos += 16;
    }
    size_t cmapAt = (out.size() + 3) & ~(size_t)3;
    out.resize(cmapAt + cmapLen, 0);
    unsigned char* cm = out.data() + cmapAt;
    memcpy(cm, "cmap", 0);
    cm[2] = 0; cm[3] = 1;            // one subtable
    cm[4] = 0; cm[5] = 3; cm[6] = 0; cm[7] = 1;   // (3,1) Windows Unicode BMP
    cm[8] = 0; cm[9] = 0; cm[10] = 0; cm[11] = 12;
    unsigned char* sub = cm + 12;
    sub[0] = 0; sub[1] = 6;                                  // format 6
    unsigned subLen = 10 + numGlyphs * 2;
    sub[2] = (unsigned char)(subLen >> 8); sub[3] = (unsigned char)(subLen & 0xFF);
    sub[4] = 0; sub[5] = 0;                                  // language
    sub[6] = 0; sub[7] = 0;                                  // firstCode
    sub[8] = (unsigned char)(numGlyphs >> 8); sub[9] = (unsigned char)(numGlyphs & 0xFF);
    for (unsigned g = 0; g < numGlyphs; g++) {
        sub[10 + g * 2] = (unsigned char)(g >> 8);
        sub[10 + g * 2 + 1] = (unsigned char)(g & 0xFF);
    }
    memcpy(out.data() + dirPos, "cmap", 4);
    out[dirPos + 8] = (unsigned char)(cmapAt >> 24); out[dirPos + 9] = (unsigned char)(cmapAt >> 16);
    out[dirPos + 10] = (unsigned char)(cmapAt >> 8); out[dirPos + 11] = (unsigned char)cmapAt;
    out[dirPos + 12] = (unsigned char)(cmapLen >> 24); out[dirPos + 13] = (unsigned char)(cmapLen >> 16);
    out[dirPos + 14] = (unsigned char)(cmapLen >> 8); out[dirPos + 15] = (unsigned char)cmapLen;
    return out;
}

static bool openFace(SubFace& f, const unsigned char* data, unsigned int len) {
    f.src = data;
    if (stbtt_InitFont(&f.info, data, 0)) { f.data = data; f.len = len; return true; }
    std::vector<unsigned char> patched = addIdentityCmap(data, len);
    if (patched.empty()) return false;
    f.owned = std::move(patched);
    if (!stbtt_InitFont(&f.info, f.owned.data(), 0)) return false;
    f.data = f.owned.data();
    f.len = (unsigned int)f.owned.size();
    return true;
}

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
            f.ok = openFace(f, spec->data, spec->len);
        }
    }
    return f.ok ? &f : nullptr;
}

static SubFace* faceFromData(const unsigned char* data, unsigned int len) {
    static SubFace slots[24];
    static int count = 0;
    // Cache by the *source* pointer: a patched face rewrites data/len to its
    // own buffer, and keying on that made every call miss, burn a slot and
    // eventually fall back to a Latin substitute mid-paragraph.
    for (int i = 0; i < count; i++)
        if (slots[i].src == data) return slots[i].ok ? &slots[i] : nullptr;
    if (count >= 24) return nullptr;
    SubFace& f = slots[count++];
    f.src = data;
    f.ok = openFace(f, data, len);
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
    int   bm = BM_NORMAL;          // /BM blend mode
    ObjP  fillPat, strokePat;      // active tiling/shading pattern (null = solid)
    ClipMask clip;
    // soft mask: per-pixel 0..255 luminance applied to every paint
    ClipMask smask;
    double smaskOffsetX = 0, smaskOffsetY = 0;   // device-space origin of the mask
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
    ObjP curRes;        // resources of the stream currently executing
    struct TileEntry { ObjP pat; double scale; Canvas cv; };
    std::vector<TileEntry> tiles;   // rendered tiling-pattern cells

    void exec(const std::string& data, ObjP resources, GState gs);
    void doXObject(ObjP ref, ObjP resources, GState& gs, const std::string& name);
    void drawImage(ObjP io, GState& gs);
    void showText(const std::string& bytes, GState& gs, Mat& tmBase,
                  double fs, double tc, double tw, double th, int renderMode, double rise,
                  const Font* font, const std::string& embedded);
    void paintPath(PathBuilder& pb, bool strokeIt, bool fillIt, bool evenOdd, GState& gs);
    void paintShading(ObjP shadingName, ObjP resources, GState& gs);
    void paintShadingObj(ObjP sh, GState& gs);
    void paintMeshShading(ObjP sh, int type, int ncomp, GState& gs);
    void paintPattern(const Polys& polys, bool evenOdd, ObjP pat, GState& gs, float alpha);
    void paintTilingPattern(const Polys& polys, bool evenOdd, ObjP pat, GState& gs, float alpha);
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
        if (gs.fillPat) {
            paintPattern(polys, evenOdd, gs.fillPat, gs, gs.fillA);
        } else {
            FillPolys(*cv, polys, evenOdd, gs.fill[0], gs.fill[1], gs.fill[2], gs.fillA,
                      gs.clip ? gs.clip.get() : nullptr, gs.bm);
            st->fills++;
        }
    }
    if (strokeIt) {
        Polys outline;
        std::vector<std::pair<Poly, bool>> dashed;
        double sc = ctmScale(gs.ctm);
        std::vector<double> dash = gs.dash;
        for (double& d : dash) d *= sc;
        ApplyDash(polys, dash, gs.dashPhase * sc, false, dashed);
        for (auto& dp : dashed) StrokePolys(outline, dp.first, (float)(gs.lw * sc), gs.cap, dp.second);
        if (gs.strokePat) {
            GState sgs = gs;
            sgs.fillA = gs.strokeA;
            paintPattern(outline, false, gs.strokePat, sgs, gs.strokeA);
        } else {
            FillPolys(*cv, outline, false, gs.stroke[0], gs.stroke[1], gs.stroke[2], gs.strokeA,
                      gs.clip ? gs.clip.get() : nullptr, gs.bm);
            st->strokes++;
        }
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
        // CID fonts index glyphs by CID: use the explicit map, the CFF
        // charset, or Identity (CID == GID), in that order.
        if (font->isCid) {
            int g = -1;
            auto it = font->cidToGid.find(code);
            if (it != font->cidToGid.end()) g = it->second;
            else if (font->cff) g = font->cff->gidForCid(code);
            else g = code;
            if (g < 0) return 0;
            if (font->cff) { chosen = face; return g < font->cff->numGlyphs() ? g : 0; }
            if (fp.primary && g < fp.primary->info.numGlyphs) { chosen = fp.primary; return g; }
            if (font->embedded.empty()) return 0;
            chosen = face;
            return g;
        }
        // Simple CFF font: the code -> GID table was built from /Encoding.
        if (!font->codeToGid.empty()) {
            auto it = font->codeToGid.find(code);
            chosen = face;
            return it == font->codeToGid.end() ? 0 : it->second;
        }
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
    const CMap* cmap = font->cmap.get();
    // matrix from glyph units (font units, 1 em = unitsPerEm) to device:
    //   glyph -> em: scale by fs/upem  (we use stbtt scale on glyph space)
    //   then text matrix, then ctm, with Tz (horizontal) and Ts (rise)
    for (size_t i = 0; i < bytes.size(); ) {
        // With a predefined or embedded /Encoding CMap the code width varies
        // (1..4 bytes), so walk the string by the codespace instead of stepping
        // a fixed 2. Without one, a Type0 font is still two bytes per code.
        int step = (font->subtype == "Type0") ? 2 : 1;
        if (cmap) {
            int n = cmap->codeLength((const unsigned char*)bytes.data() + i, bytes.size() - i);
            if (n <= 0) { i++; continue; }     // byte outside every codespace: resync
            step = n;
        }
        if (i + step > bytes.size()) break;
        int code = 0;
        for (int k = 0; k < step; k++) code = (code << 8) | (unsigned char)bytes[i + k];
        int cid = code;
        if (cmap) {
            cid = cmap->lookup((unsigned int)code, step);
            if (!cid) cid = 0;                 // .notdef, but keep the pen moving
        }
        int uni = font->unicodeFor(code);
        int gid = glyphFor(cid, uni, face);
        // CID fonts often omit /W for a subset and leave /DW at 1000, which
        // spaces every glyph a full em apart and turns the line into a jumble.
        // The embedded face knows the real advance, so ask it.
        double adv1000 = font->widthFor(cid);
        if (gid && face && !font->widthKnown(cid)) {
            int adv = 0, lsb = 0;
            stbtt_GetGlyphHMetrics(&face->info, gid, &adv, &lsb);
            if (adv > 0) {
                float sc = stbtt_ScaleForMappingEmToPixels(&face->info, 1.0f);
                // scale = 1 / unitsPerEm, so em fraction = adv * scale
                if (sc > 0) adv1000 = adv * sc * 1000.0;   // 1 em == 1000
            }
        }

        // text rendering matrix: [fs*th 0 0 fs 0 rise] x Tm x CTM
        Mat trm = Mat(fs * th, 0, 0, fs, 0, rise).mul(tmBase).mul(gs.ctm);
        Mat toPage = trm.mul(devToPage);

        float cbx0 = 0, cby0 = 0, cbx1 = 0, cby1 = 0;
        bool haveCffBox = false;
        if (gid && font->cff) {
            // CFF / Type2 outlines: our interpreter hands back em units, which
            // the text rendering matrix maps straight to device space.
            std::vector<CffSeg> segs;
            if (font->cff->glyphPath(gid, segs) && !segs.empty()) {
                PathBuilder gpb(trm);
                float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
                for (const CffSeg& sg : segs) {
                    if (sg.type == 0) gpb.moveTo(sg.x, sg.y);
                    else if (sg.type == 1) gpb.lineTo(sg.x, sg.y);
                    else gpb.curveTo(sg.x1, sg.y1, sg.x2, sg.y2, sg.x, sg.y);
                    float px[3] = {(float)sg.x, (float)sg.x1, (float)sg.x2};
                    float py[3] = {(float)sg.y, (float)sg.y1, (float)sg.y2};
                    for (int k = 0; k < 3; k++) {
                        if (sg.type == 0 && k > 0) break;
                        if (sg.type == 1 && k > 0) break;
                        minx = std::min(minx, px[k]); maxx = std::max(maxx, px[k]);
                        miny = std::min(miny, py[k]); maxy = std::max(maxy, py[k]);
                    }
                }
                gpb.close();
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
                cbx0 = minx; cby0 = miny; cbx1 = maxx; cby1 = maxy;
                haveCffBox = (maxx > minx && maxy > miny);
            }
        } else if (gid) {
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
            float gx0 = 0, gy0 = 0, gx1 = (float)adv1000 / 1000.f, gy1 = 0.7f;
            if (haveCffBox) {
                gx0 = cbx0; gy0 = cby0; gx1 = cbx1; gy1 = cby1;
            } else if (gid && !font->cff) {
                int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
                if (stbtt_GetGlyphBox(&face->info, gid, &bx0, &by0, &bx1, &by1)) {
                    // font units -> em: 1/unitsPerEm == ScaleForMappingEmToPixels(1.0)
                    float up = stbtt_ScaleForMappingEmToPixels(&face->info, 1.0f);
                    gx0 = bx0 * up; gy0 = by0 * up;
                    gx1 = bx1 * up; gy1 = by1 * up;
                    if (gx1 - gx0 < 0.02f) gx1 = gx0 + (float)adv1000 / 1000.f;
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
                double advUser = ((font->widthKnown(cid) ? font->widthFor(cid) : adv1000) / 1000.0 * fs + tc) * th;
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
        double w0 = (font->widthKnown(cid) ? font->widthFor(cid) : adv1000) / 1000.0;
        double adv = (w0 * fs + tc) * th;
        if (step == 1 && code == 32) adv += tw * th;
        // Tm moves by adv in text space
        // tmBase is [..] with translation = current pen; translate x by adv
        // (Tj advances the text matrix: Td(adv,0) after each glyph)
        // We rebuild tmBase for the next glyph:
        tmBase = Mat(1, 0, 0, 1, adv, 0).mul(tmBase);
        i += step;
    }
}

void Interp::paintShading(ObjP shadingName, ObjP resources, GState& gs) {
    if (!resources || !shadingName || shadingName->t != O_NAME) return;
    ObjP shadings = doc->resolve(resources->get("Shading"));
    if (!shadings) return;
    ObjP sh = doc->resolve(shadings->get(shadingName->str.c_str()));
    if (!sh) { st->note("shading missing"); return; }
    paintShadingObj(sh, gs);
}

// ─── mesh shadings (types 4-7) and the type 1 function shading ────────────
// A mesh is a stream of vertices: coordinates and colour components packed at
// /BitsPerCoordinate and /BitsPerComponent, each mapped through /Decode. Type 4
// and 5 are Gouraud triangles, which is what producers actually emit for
// gradients and soft backgrounds; type 6/7 patch meshes stay unsupported and are
// reported instead of being approximated into a wrong picture.

struct BitReader {
    const std::string& d;
    size_t bit = 0;
    explicit BitReader(const std::string& data) : d(data) {}
    unsigned get(int n) {
        unsigned v = 0;
        for (int i = 0; i < n; i++) {
            size_t byte = bit >> 3;
            if (byte >= d.size()) return 0;
            v = (v << 1) | ((unsigned char)d[byte] >> (7 - (bit & 7)) & 1u);
            bit++;
        }
        return v;
    }
    size_t usedBits() const { return bit; }
    size_t totalBits() const { return d.size() * 8; }
};

static double decodeComponent(unsigned raw, int bits, double lo, double hi) {
    if (bits <= 0) return lo;
    double maxv = (double)((1u << bits) - 1);
    return lo + (hi - lo) * ((double)raw / maxv);
}

void Interp::paintMeshShading(ObjP sh, int type, int ncomp, GState& gs) {
    if (type == 6 || type == 7) { st->note("mesh shading type " + std::to_string(type)); return; }

    int bpc = (int)num(doc->resolve(sh->get("BitsPerCoordinate")), 0);
    int bcc = (int)num(doc->resolve(sh->get("BitsPerComponent")), 8);
    if (bpc < 1 || bpc > 32 || bcc < 1 || bcc > 16) { st->note("mesh bit depth"); return; }

    ObjP dec = doc->resolve(sh->get("Decode"));
    if (!dec || (int)dec->arr.size() < 4 + 2 * ncomp) { st->note("mesh decode"); return; }
    double dx0 = num(doc->resolve(dec->arr[0]), 0), dx1 = num(doc->resolve(dec->arr[1]), 1);
    double dy0 = num(doc->resolve(dec->arr[2]), 0), dy1 = num(doc->resolve(dec->arr[3]), 1);
    std::vector<double> clo(ncomp), chi(ncomp);
    for (int c = 0; c < ncomp; c++) {
        clo[c] = num(doc->resolve(dec->arr[4 + 2 * c]), 0);
        chi[c] = num(doc->resolve(dec->arr[5 + 2 * c]), 1);
    }

    std::string data;
    if (!doc->decodeStream(sh, data)) { st->note("mesh stream"); return; }
    BitReader br(data);

    struct V { double x, y; float c[4]; };
    auto readVertex = [&](V& v, bool withFlag, unsigned& flag) {
        if (withFlag) flag = br.get(1);
        double ux = decodeComponent(br.get(bpc), bpc, dx0, dx1);
        double uy = decodeComponent(br.get(bpc), bpc, dy0, dy1);
        gs.ctm.apply(ux, uy, v.x, v.y);
        for (int c = 0; c < ncomp; c++)
            v.c[c] = (float)decodeComponent(br.get(bcc), bcc, clo[c], chi[c]);
    };

    static const double sub[4][2] = {{0.25, 0.25}, {0.75, 0.25}, {0.25, 0.75}, {0.75, 0.75}};
    int drawn = 0;

    auto fillTriangle = [&](const V& a, const V& b, const V& c) {
        double minx = std::min(a.x, std::min(b.x, c.x)), maxx = std::max(a.x, std::max(b.x, c.x));
        double miny = std::min(a.y, std::min(b.y, c.y)), maxy = std::max(a.y, std::max(b.y, c.y));
        int x0 = std::max(0, (int)std::floor(minx)), x1 = std::min(cv->w, (int)std::ceil(maxx));
        int y0 = std::max(0, (int)std::floor(miny)), y1 = std::min(cv->h, (int)std::ceil(maxy));
        double det = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
        if (fabs(det) < 1e-9) return;
        for (int y = y0; y < y1; y++) {
            for (int x = x0; x < x1; x++) {
                float cov = 0.f, acc[4] = {0, 0, 0, 0};
                for (auto& o : sub) {
                    double px = x + o[0], py = y + o[1];
                    double l1 = ((b.y - c.y) * (px - c.x) + (c.x - b.x) * (py - c.y)) / det;
                    double l2 = ((c.y - a.y) * (px - c.x) + (a.x - c.x) * (py - c.y)) / det;
                    double l3 = 1.0 - l1 - l2;
                    if (l1 < -1e-6 || l2 < -1e-6 || l3 < -1e-6) continue;
                    cov += 0.25f;
                    for (int k = 0; k < 4; k++)
                        acc[k] += (float)(l1 * a.c[k] + l2 * b.c[k] + l3 * c.c[k]) * 0.25f;
                }
                if (cov <= 0.002f) continue;
                float a2 = cov * gs.fillA;
                if (gs.clip) a2 *= (*gs.clip)[(size_t)y * cv->w + x] * (1.f / 255.f);
                if (a2 <= 0.f) continue;
                float r, g, b2;
                if (ncomp == 1) { r = g = b2 = acc[0]; }
                else if (ncomp == 4) { float rgb3[3]; cmykToRgb(acc[0], acc[1], acc[2], acc[3], rgb3); r = rgb3[0]; g = rgb3[1]; b2 = rgb3[2]; }
                else { r = acc[0]; g = ncomp > 1 ? acc[1] : acc[0]; b2 = ncomp > 2 ? acc[2] : acc[0]; }
                cv->blend(x, y, r, g, b2, a2, gs.bm);
            }
        }
        drawn++;
    };

    if (type == 4) {
        while (br.usedBits() + (size_t)(8 + 3 * (bpc * 2 + bcc * ncomp)) <= br.totalBits()) {
            V tri[3];
            for (int i = 0; i < 3; i++) { unsigned flag = 0; readVertex(tri[i], false, flag); }
            fillTriangle(tri[0], tri[1], tri[2]);
        }
    } else if (type == 5) {
        int perRow = (int)num(doc->resolve(sh->get("VerticesPerRow")), 0);
        if (perRow < 2) { st->note("mesh lattice"); return; }
        std::vector<V> verts;
        while (br.usedBits() + (size_t)(1 + bpc * 2 + bcc * ncomp) <= br.totalBits()) {
            V v; unsigned flag = 0; readVertex(v, true, flag);
            verts.push_back(v);
        }
        int rows = (int)verts.size() / perRow;
        for (int r = 0; r + 1 < rows; r++)
            for (int c = 0; c + 1 < perRow; c++) {
                const V& v00 = verts[r * perRow + c];
                const V& v01 = verts[r * perRow + c + 1];
                const V& v10 = verts[(r + 1) * perRow + c];
                const V& v11 = verts[(r + 1) * perRow + c + 1];
                fillTriangle(v00, v01, v11);
                fillTriangle(v00, v11, v10);
            }
    }
    if (drawn) st->fills++;
}

void Interp::paintShadingObj(ObjP sh, GState& gs) {
    int type = (int)num(doc->resolve(sh->get("ShadingType")), 0);
    if (type != 2 && type != 3) {
        // mesh and function shadings need their own colour decoding path, so
        // they are dispatched before the axial/radial setup below.
        int ncomp = 3;
        ObjP cs0 = doc->resolve(sh->get("ColorSpace"));
        if (cs0) {
            if (cs0->t == O_NAME) {
                if (cs0->str == "DeviceGray") ncomp = 1;
                else if (cs0->str == "DeviceCMYK") ncomp = 4;
            } else if (cs0->t == O_ARR && !cs0->arr.empty()) {
                std::string fam = objName(doc->resolve(cs0->arr[0]));
                if (fam == "ICCBased" && cs0->arr.size() > 1) {
                    ObjP stm = doc->resolve(cs0->arr[1]);
                    if (stm) ncomp = (int)num(doc->resolve(stm->get("N")), 3);
                }
            }
        }
        if (type >= 4 && type <= 7) { paintMeshShading(sh, type, ncomp, gs); return; }
        st->note("shading type " + std::to_string(type));
        return;
    }

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
            cv->blend(x, y, r, g, b, a, gs.bm);
        }
    }
    st->fills++;
}

void Interp::paintPattern(const Polys& polys, bool evenOdd, ObjP pat, GState& gs, float alpha) {
    if (!pat || pat->t != O_DICT) return;
    int ptype = (int)num(doc->resolve(pat->get("PatternType")), 0);
    if (ptype == 2) {
        // shading pattern: paint the shading clipped to the current path
        ObjP sh = doc->resolve(pat->get("Shading"));
        if (!sh) { st->note("shading pattern"); return; }
        GState sub = gs;
        sub.clip = ClipPolys(*cv, gs.clip, polys, evenOdd);
        paintShadingObj(sh, sub);
        return;
    }
    if (ptype != 1) { st->note("pattern type " + std::to_string(ptype)); return; }
    paintTilingPattern(polys, evenOdd, pat, gs, alpha);
}

// A tiling pattern is a small content stream stamped on a lattice. The cell is
// rendered once per (pattern, scale) into a transparent buffer, then every pixel
// inside the painted path samples that buffer through the inverse of
// /Matrix x CTM — so rotation and skew need no special case.
void Interp::paintTilingPattern(const Polys& polys, bool evenOdd, ObjP pat, GState& gs, float alpha) {
    ObjP bboxO = doc->resolve(pat->get("BBox"));
    if (!bboxO || bboxO->t != O_ARR || bboxO->arr.size() < 4) { st->note("pattern bbox"); return; }
    double bx0 = num(doc->resolve(bboxO->arr[0]), 0), by0 = num(doc->resolve(bboxO->arr[1]), 0);
    double bx1 = num(doc->resolve(bboxO->arr[2]), 0), by1 = num(doc->resolve(bboxO->arr[3]), 0);
    if (bx1 < bx0) std::swap(bx0, bx1);
    if (by1 < by0) std::swap(by0, by1);
    if (bx1 - bx0 < 0.01 || by1 - by0 < 0.01) { st->note("pattern bbox empty"); return; }

    double xs = num(doc->resolve(pat->get("XStep")), bx1 - bx0);
    double ys = num(doc->resolve(pat->get("YStep")), by1 - by0);
    if (xs <= 0.01 || ys <= 0.01) { st->note("pattern step"); return; }

    Mat pm;   // /Matrix: pattern space -> default user space
    ObjP mx = doc->resolve(pat->get("Matrix"));
    if (mx && mx->t == O_ARR && mx->arr.size() == 6) {
        pm = Mat(num(doc->resolve(mx->arr[0]), 1), num(doc->resolve(mx->arr[1]), 0),
                 num(doc->resolve(mx->arr[2]), 0), num(doc->resolve(mx->arr[3]), 1),
                 num(doc->resolve(mx->arr[4]), 0), num(doc->resolve(mx->arr[5]), 0));
    }
    Mat dev = pm.mul(gs.ctm);            // pattern space -> device pixels
    double scale = ctmScale(dev);
    if (!(scale > 1e-4)) return;

    // Device-space bounds of one cell. The pattern matrix carries the page flip
    // and offset, so the cell does not start at the tile's origin: compute where
    // the four corners land and shift the tile matrix by that minimum, otherwise
    // the cell is painted outside its own buffer (blank tiles).
    double cw = bx1 - bx0, ch = by1 - by0;
    double minX = 1e30, minY = 1e30, maxX = -1e30, maxY = -1e30;
    const double corners[4][2] = {{0, 0}, {cw, 0}, {cw, ch}, {0, ch}};
    for (auto& c : corners) {
        double dx, dy;
        dev.apply(c[0], c[1], dx, dy);
        minX = std::min(minX, dx); maxX = std::max(maxX, dx);
        minY = std::min(minY, dy); maxY = std::max(maxY, dy);
    }
    // Mat::mul(X) applies "this" first: cell-local -> device -> tile pixel.
    // The cell is rendered at kTileSS x device resolution: a rotated checker
    // sampled at 1:1 has staircased edges and a mean diff an order of magnitude
    // above the gate, while the supersampled cell matches the reference.
    const int kTileSS = 3;
    Mat toTile = Mat(1, 0, 0, 1, -bx0, -by0).mul(dev)
                    .mul(Mat(1, 0, 0, 1, -minX, -minY))
                    .mul(Mat(kTileSS, 0, 0, kTileSS, 0, 0));

    // exactly one cell: a spare pixel column would be empty and show up as a
    // seam at every tile boundary
    int tw = (int)std::ceil((maxX - minX) * kTileSS);
    int th = (int)std::ceil((maxY - minY) * kTileSS);
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    if (tw > 2048) tw = 2048;            // clamp like a page: a hostile /BBox must not allocate GBs
    if (th > 2048) th = 2048;

    Canvas* tile = nullptr;
    for (auto& e : tiles)
        if (e.pat.get() == pat.get() && fabs(e.scale - scale) < 1e-3) { tile = &e.cv; break; }
    if (!tile) {
        if (tiles.size() > 24) tiles.clear();      // keep the footprint bounded
        std::string data;
        if (!doc->decodeStream(pat, data)) { st->note("pattern stream"); return; }
        ObjP pres = doc->resolve(pat->get("Resources"));
        if (!pres) pres = curRes;

        TileEntry e;
        e.pat = pat;
        e.scale = scale;
        e.cv.initTransparent(tw, th);
        tiles.push_back(std::move(e));
        tile = &tiles.back().cv;

        // paint the cell into the tile buffer with the same alignment the
        // sampling loop below assumes
        GState sub;
        sub.ctm = toTile;
        Canvas* savedCv = cv;
        ObjP savedRes = curRes;
        cv = tile;
        curRes = pres;
        exec(data, pres, sub);
        cv = savedCv;
        curRes = savedRes;
    }

    // path coverage + clip, sampled once and reused for every tile pixel
    ClipMask mask = ClipPolys(*cv, gs.clip, polys, evenOdd);
    if (!mask) return;

    double det = dev.a * dev.d - dev.b * dev.c;
    if (fabs(det) < 1e-12) return;
    double inv = 1.0 / det;
    // inverse of dev, for device pixel -> pattern space
    double ia = dev.d * inv, ib = -dev.b * inv, ic = -dev.c * inv, id = dev.a * inv;
    double ie = (dev.c * dev.f - dev.d * dev.e) * inv;
    double iff = (dev.b * dev.e - dev.a * dev.f) * inv;

    // 2x2 supersampling per pixel: a rotated cell lands on fractional tile
    // coordinates, and picking one texel turns every diagonal edge into a
    // staircase that the reference renderer does not have.
    auto sampleAt = [&](double sx, double sy, float* out) {
        double px = ia * sx + ic * sy + ie;
        double py = ib * sx + id * sy + iff;
        double cx = px - xs * std::floor((px - bx0) / xs);
        double cy = py - ys * std::floor((py - by0) / ys);
        double tx, ty;
        toTile.apply(cx, cy, tx, ty);
        int ix = (int)std::floor(tx), iy = (int)std::floor(ty);
        if (ix < 0) ix = 0;
        if (iy < 0) iy = 0;
        if (ix >= tile->w) ix = tile->w - 1;
        if (iy >= tile->h) iy = tile->h - 1;
        const uint8_t* s = &tile->px[((size_t)iy * tile->w + ix) * 4];
        // premultiplied: transparent texels carry no colour, and averaging raw
        // colour would drag every edge towards black
        float a = s[3] * (1.f / 255.f);
        out[0] = s[0] * (1.f / 255.f) * a;
        out[1] = s[1] * (1.f / 255.f) * a;
        out[2] = s[2] * (1.f / 255.f) * a;
        out[3] = a;
    };

    double offs[9][2];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            offs[i * 3 + j][0] = (j + 0.5) / 3.0;
            offs[i * 3 + j][1] = (i + 0.5) / 3.0;
        }
    for (int y = 0; y < cv->h; y++) {
        for (int x = 0; x < cv->w; x++) {
            float cov = (*mask)[(size_t)y * cv->w + x] * (1.f / 255.f);
            if (cov <= 0.002f) continue;
            float acc[4] = {0, 0, 0, 0};
            for (auto& o : offs) {
                float sm[4];
                sampleAt(x + o[0], y + o[1], sm);
                for (int k = 0; k < 4; k++) acc[k] += sm[k] * (1.f / 9.f);
            }
            if (acc[3] <= 1e-4f) continue;
            float cr = acc[0] / acc[3], cg = acc[1] / acc[3], cb2 = acc[2] / acc[3];
            float sa = acc[3] * cov * alpha * gs.fillA;
            if (sa <= 0.0005f) continue;
            cv->blend(x, y, cr, cg, cb2, sa, gs.bm);
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

    // A form is clipped to /BBox. Producers rely on that: this figure's form
    // opens with a white rectangle of 1152x2199 units to clear its background,
    // and without the clip that rectangle paints over the rest of the page.
    ObjP bb = doc->resolve(xo->get("BBox"));
    if (bb && bb->t == O_ARR && bb->arr.size() == 4) {
        double v[4];
        for (int i = 0; i < 4; i++) v[i] = num(doc->resolve(bb->arr[i]), i < 2 ? 0 : 1);
        double x0 = std::min(v[0], v[2]), x1 = std::max(v[0], v[2]);
        double y0 = std::min(v[1], v[3]), y1 = std::max(v[1], v[3]);
        if (x1 - x0 > 0.01 && y1 - y0 > 0.01) {
            const double px[4] = {x0, x1, x1, x0}, py[4] = {y0, y0, y1, y1};
            Poly box;
            for (int i = 0; i < 4; i++) {
                double dx, dy;
                sub.ctm.apply(px[i], py[i], dx, dy);
                box.push_back(Pt{(float)dx, (float)dy});
            }
            Polys polys;
            polys.push_back(box);
            sub.clip = ClipPolys(*cv, gs.clip, polys, false);
        }
    }

    depth++;
    // A form with /Group /Transparency is a group: it is composited as a whole,
    // not painted straight into the page. Rendering it into its own buffer is
    // what makes Multiply/Screen logos and /ca groups behave like the producer
    // drew them (otherwise the blend applies per object and the result is far
    // too dark).
    ObjP group = doc->resolve(xo->get("Group"));
    std::string gsub = group ? objName(doc->resolve(group->get("S"))) : std::string();
    bool isolatedGroup = (gsub == "Transparency");
    if (isolatedGroup && depth <= 6) {
        Canvas off;
        off.initTransparent(cv->w, cv->h);
        // Inside an isolated group the backdrop is transparent, so the group's
        // own /BM and /ca must not leak into its contents: a Multiply set for
        // the group would otherwise multiply every object against black.
        sub.bm = BM_NORMAL;
        sub.fillA = 1.f;
        sub.strokeA = 1.f;
        Canvas* savedCv = cv;
        cv = &off;
        exec(data, res, sub);
        cv = savedCv;
        float ga = gs.fillA;
        for (int y = 0; y < cv->h; y++) {
            for (int x = 0; x < cv->w; x++) {
                const uint8_t* s = &off.px[((size_t)y * off.w + x) * 4];
                if (s[3] == 0) continue;
                cv->blend(x, y, s[0] * (1.f / 255.f), s[1] * (1.f / 255.f),
                          s[2] * (1.f / 255.f), s[3] * (1.f / 255.f) * ga, gs.bm);
            }
        }
        depth--;
        (void)name;
        return;
    }
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
    // An /ICCBased image carries the producer's real profile; run it instead of
    // the naive component guess so CMYK and wide-gamut images match the source.
    IccModel icc;
    bool haveIcc = false;
    if (csObj && csObj->t == O_ARR && csObj->arr.size() > 1 &&
        objName(doc->resolve(csObj->arr[0])) == "ICCBased") {
        ObjP stm = doc->resolve(csObj->arr[1]);
        std::string prof;
        if (stm && doc->decodeStream(stm, prof)) haveIcc = iccParse(prof, icc);
        if (haveIcc && icc.n > 0) ncomp = icc.n;
    }

    std::string raw;
    if (doc->hasFilter(io, "JBIG2Decode")) {
        // Bi-level scans: PDFium's decoder, compiled into this module.
        std::vector<uint8_t> pix;
        int jw = 0, jh = 0;
        if (!doc->decodeJbig2(io, pix, jw, jh)) { st->note("image filter (JBIG2)"); return; }
        iw = jw; ih = jh;
        ncomp = 1; bpc = 1;
        raw.assign((const char*)pix.data(), pix.size());
    } else if (doc->hasFilter(io, "JPXDecode")) {
        // JPEG 2000 carries its own dimensions and component count.
        std::vector<uint8_t> pix;
        int jw = 0, jh = 0, jc = 0;
        if (!doc->decodeJpx(io, pix, jw, jh, jc)) { st->note("image filter (JPX)"); return; }
        iw = jw; ih = jh;
        if (jc > 0) ncomp = jc;
        bpc = 8;
        raw.assign((const char*)pix.data(), pix.size());
    } else if (!doc->decodeStream(io, raw)) {
        st->note("image filter (DCT/CCITT/JBIG2)"); return;
    }

    size_t expected = (size_t)iw * ih * ncomp * bpc / 8;
    if (raw.size() < expected) { st->note("image data short"); return; }

    // /SMask: a grey soft mask where 0 = fully transparent. Without it, logos
    // and signature stamps paint as solid black rectangles. /Mask is the
    // stencil variant (1 bpc, 1 = opaque) used by watermarks: drawing those
    // without their mask paints a solid bar over the page.
    std::vector<uint8_t> mask;
    int mw = 0, mh = 0;
    bool maskIsStencil = false;
    {
        ObjP mk = doc->resolve(io->get("Mask"));
        if (mk && mk->isStream) {
            int mbpc = (int)num(doc->resolve(mk->get("BitsPerComponent")), 1);
            mw = (int)num(doc->resolve(mk->get("Width")), 0);
            mh = (int)num(doc->resolve(mk->get("Height")), 0);
            std::string mraw;
            // A stencil mask can itself be a JBIG2 scan (common in FaxGroup
            // scans, which carry a JBIG2 /Mask next to the page image).
            bool mok = false;
            if (mbpc == 1 && mw > 0 && mh > 0) {
                if (doc->hasFilter(mk, "JBIG2Decode")) {
                    std::vector<uint8_t> pix;
                    int jw = 0, jh = 0;
                    if (doc->decodeJbig2(mk, pix, jw, jh)) {
                        mraw.assign((const char*)pix.data(), pix.size());
                        mw = jw; mh = jh;
                        mok = true;
                    }
                } else {
                    mok = doc->decodeStream(mk, mraw);
                }
            }
            if (mok) {
                size_t stride = ((size_t)mw + 7) / 8;
                if (mraw.size() >= stride * (size_t)mh) {
                    mask.resize((size_t)mw * mh);
                    for (int y = 0; y < mh; y++) {
                        const unsigned char* row = (const unsigned char*)mraw.data() + (size_t)y * stride;
                        for (int x = 0; x < mw; x++)
                            mask[(size_t)y * mw + x] =
                                ((row[x >> 3] >> (7 - (x & 7))) & 1) ? 255 : 0;
                    }
                    maskIsStencil = true;
                } else { mw = mh = 0; }
            } else { mw = mh = 0; }
        }
    }
    {
        ObjP sm = maskIsStencil ? nullptr : doc->resolve(io->get("SMask"));
        if (sm && sm->isStream) {
            std::string mraw;
            int mbpc = (int)num(doc->resolve(sm->get("BitsPerComponent")), 8);
            mw = (int)num(doc->resolve(sm->get("Width")), 0);
            mh = (int)num(doc->resolve(sm->get("Height")), 0);
            if (mbpc == 8 && mw > 0 && mh > 0 && doc->decodeStream(sm, mraw) &&
                mraw.size() >= (size_t)mw * mh) {
                mask.assign((const uint8_t*)mraw.data(), (const uint8_t*)mraw.data() + (size_t)mw * mh);
            } else {
                mw = mh = 0;
            }
        }
    }

    auto sample = [&](int x, int y, int c) -> float {
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= iw) x = iw - 1;
        if (y >= ih) y = ih - 1;
        if (bpc == 8) {
            size_t idx = ((size_t)(ih - 1 - y) * iw + x) * ncomp + c;
            return (unsigned char)raw[idx] / 255.f;
        }
        if (bpc == 1) {
            // 1-bpc rows are padded to a byte boundary: index with the row
            // stride, not with the pixel width, or every row drifts by the
            // padding bits and the image shears.
            size_t rowBytes = ((size_t)iw + 7) / 8;
            size_t bit = (size_t)(ih - 1 - y) * rowBytes * 8 + (size_t)x * ncomp + c;
            if ((bit >> 3) >= raw.size()) return 1.f;
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
            // How much source area one device pixel covers. Scans are often
            // thousands of pixels wide and get drawn at a fraction of that;
            // picking a single source pixel drops thin strokes and shears
            // rows, so average a small grid over the footprint instead.
            double spanU = std::fabs(fy / det) * iw;
            double spanV = std::fabs(ex / det) * ih;
            int nx = (int)(spanU + 0.5), ny = (int)(spanV + 0.5);
            if (nx < 1) nx = 1; if (nx > 4) nx = 4;
            if (ny < 1) ny = 1; if (ny > 4) ny = 4;
            auto avg = [&](int x0, int y0, int c) -> float {
                if (nx == 1 && ny == 1) return sample(x0, y0, c);
                float sum = 0.f;
                int ox = (int)(spanU / nx), oy = (int)(spanV / ny);
                if (ox < 1) ox = 1; if (oy < 1) oy = 1;
                for (int j = 0; j < ny; j++)
                    for (int i = 0; i < nx; i++)
                        sum += sample(x0 + i * ox, y0 + j * oy, c);
                return sum / (float)(nx * ny);
            };
            float r, g, b;
            if (haveIcc) {
                float in[4] = {0, 0, 0, 0};
                for (int c = 0; c < ncomp && c < 4; c++) in[c] = avg(sx, sy, c);
                float rgb[3];
                iccToRgb(icc, in, rgb);
                r = rgb[0]; g = rgb[1]; b = rgb[2];
            } else if (ncomp == 1) { r = g = b = avg(sx, sy, 0); }
            else if (ncomp == 4) {
                float c = avg(sx, sy, 0), m = avg(sx, sy, 1), yy = avg(sx, sy, 2), k = avg(sx, sy, 3);
                r = (1 - c) * (1 - k); g = (1 - m) * (1 - k); b = (1 - yy) * (1 - k);
            } else { r = avg(sx, sy, 0); g = avg(sx, sy, 1); b = avg(sx, sy, 2); }
            float a = 1.f;
            if (!mask.empty() && mw > 0 && mh > 0) {
                int mx = (int)((double)sx * mw / std::max(1, iw));
                int my = (int)((double)sy * mh / std::max(1, ih));
                if (mx < 0) mx = 0; if (mx >= mw) mx = mw - 1;
                if (my < 0) my = 0; if (my >= mh) my = mh - 1;
                unsigned mv = mask[(size_t)(mh - 1 - my) * mw + mx];
                // A stencil mask paints where the bit is 0 unless /Decode flips
                // it; our decode above kept 1 = opaque, so honour that.
                a *= (maskIsStencil ? (mv ? 1.f : 0.f) : mv * (1.f / 255.f));
            }
            if (gs.clip) a *= (*gs.clip)[(size_t)y * cv->w + x] * (1.f / 255.f);
            if (a > 0) cv->blend(x, y, r, g, b, a, gs.bm);
        }
    }
}

void Interp::exec(const std::string& data, ObjP resources, GState gs) {
    std::vector<ObjP> stack;
    stack.reserve(64);
    PathBuilder pb(gs.ctm);
    bool pathHasSegs = false;

    ObjP savedRes = curRes;
    curRes = resources;
    ObjP fonts = nullptr, xobjs = nullptr, extg = nullptr, colorspaces = nullptr, patterns = nullptr;
    if (resources) {
        fonts = doc->resolve(resources->get("Font"));
        xobjs = doc->resolve(resources->get("XObject"));
        extg = doc->resolve(resources->get("ExtGState"));
        colorspaces = doc->resolve(resources->get("ColorSpace"));
        patterns = doc->resolve(resources->get("Pattern"));
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
            // operands are pushed in order, so the array is the second-from-top
            // object and the phase is the last; reading stack.back() found the
            // phase number and silently dropped every dash pattern.
            gs.dash.clear();
            if (stack.size() >= 2) {
                ObjP arr = stack[stack.size() - 2];
                if (arr && arr->t == O_ARR)
                    for (auto& e : arr->arr) gs.dash.push_back(num(doc->resolve(e), 0));
            }
            gs.dashPhase = numArg(1, 0);
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
                    v = doc->resolve(g->get("BM"));
                    if (v && v->t == O_NAME) gs.bm = blendModeFromName(v->str);
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
            // BT resets the text matrix only. Font, size, char/word spacing,
            // horizontal scale, leading, rise and render mode belong to the
            // graphics state and persist until changed — producers rely on that
            // and emit the font once per style run, not once per text object.
            Tm = Mat(); Tlm = Mat();
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
            gs.strokePat = nullptr;
        } else if (op == "g") {
            float v = (float)numArg(1, 0);
            gs.fill[0] = gs.fill[1] = gs.fill[2] = v;
            gs.fillPat = nullptr;
        } else if (op == "RG") {
            gs.stroke[0] = (float)numArg(3, 0);
            gs.stroke[1] = (float)numArg(2, 0);
            gs.stroke[2] = (float)numArg(1, 0);
            gs.strokePat = nullptr;
        } else if (op == "rg") {
            gs.fill[0] = (float)numArg(3, 0);
            gs.fill[1] = (float)numArg(2, 0);
            gs.fill[2] = (float)numArg(1, 0);
            gs.fillPat = nullptr;
        } else if (op == "K") {
            cmykToRgb(numArg(4, 0), numArg(3, 0), numArg(2, 0), numArg(1, 0), gs.stroke);
            gs.strokePat = nullptr;
        } else if (op == "k") {
            cmykToRgb(numArg(4, 0), numArg(3, 0), numArg(2, 0), numArg(1, 0), gs.fill);
            gs.fillPat = nullptr;
        } else if (op == "SC" || op == "SCN" || op == "sc" || op == "scn") {
            bool strokeOp = (op == "SC" || op == "SCN");
            if (!stack.empty() && stack.back()->t == O_NAME) {
                // pattern colour: "scn /P1" selects a tiling or shading pattern
                ObjP pat = nullptr;
                if (patterns) pat = doc->resolve(patterns->get(stack.back()->str.c_str()));
                if (pat && pat->t == O_DICT) {
                    if (strokeOp) gs.strokePat = pat; else gs.fillPat = pat;
                } else {
                    st->note("pattern colour");
                }
            } else {
                float* dst = strokeOp ? gs.stroke : gs.fill;
                if (stack.size() == 1) {
                    float v = (float)numArg(1, 0);
                    dst[0] = dst[1] = dst[2] = v;
                } else if (stack.size() == 3) {
                    dst[0] = (float)numArg(3, 0); dst[1] = (float)numArg(2, 0); dst[2] = (float)numArg(1, 0);
                } else if (stack.size() >= 4) {
                    cmykToRgb(numArg(4, 0), numArg(3, 0), numArg(2, 0), numArg(1, 0), dst);
                }
                if (strokeOp) gs.strokePat = nullptr; else gs.fillPat = nullptr;
            }
        } else {
            // unknown operator: ignore quietly (d0/d1/BX/EX/MP/DP...)
        }
        stack.clear();
    }
    (void)pathHasSegs;
    (void)colorspaces;
    (void)lastPathName;
    curRes = savedRes;
}

// ─── annotation fallbacks ───────────────────────────────────────────────────
// Not every producer writes an appearance stream where one is allowed:
// highlight/underline/strikeout/squiggly marks usually arrive as /QuadPoints
// plus /C only, and a /Text note may have no icon appearance at all. Both are
// synthesised here, straight into the device canvas, after the page content.

struct AnnotQuad { double x[4], y[4]; };

static bool annotQuads(Document* doc, const ObjP& an, std::vector<AnnotQuad>& out) {
    ObjP q = doc->resolve(an->get("QuadPoints"));
    if (q && q->t == O_ARR && q->arr.size() >= 8) {
        for (size_t i = 0; i + 7 < q->arr.size(); i += 8) {
            AnnotQuad a;
            for (int k = 0; k < 4; k++) {
                a.x[k] = num(doc->resolve(q->arr[i + k * 2]), 0);
                a.y[k] = num(doc->resolve(q->arr[i + k * 2 + 1]), 0);
            }
            double x0 = a.x[0], x1 = a.x[0], y0 = a.y[0], y1 = a.y[0];
            for (int k = 1; k < 4; k++) {
                x0 = std::min(x0, a.x[k]); x1 = std::max(x1, a.x[k]);
                y0 = std::min(y0, a.y[k]); y1 = std::max(y1, a.y[k]);
            }
            if (x1 - x0 > 0.2 && y1 - y0 > 0.2) out.push_back(a);
        }
    }
    if (out.empty()) {   // no /QuadPoints: the /Rect is the whole mark
        ObjP r = doc->resolve(an->get("Rect"));
        if (r && r->t == O_ARR && r->arr.size() >= 4) {
            double v[4];
            for (int i = 0; i < 4; i++) v[i] = num(doc->resolve(r->arr[i]), 0);
            double x0 = std::min(v[0], v[2]), x1 = std::max(v[0], v[2]);
            double y0 = std::min(v[1], v[3]), y1 = std::max(v[1], v[3]);
            if (x1 - x0 > 0.2 && y1 - y0 > 0.2) {
                AnnotQuad a;
                a.x[0] = x0; a.y[0] = y1; a.x[1] = x1; a.y[1] = y1;
                a.x[2] = x0; a.y[2] = y0; a.x[3] = x1; a.y[3] = y0;
                out.push_back(a);
            }
        }
    }
    return !out.empty();
}

// /C is grey (1 component), RGB (3) or CMYK (4), each 0..1. Producers write the
// corners of a quad in either orientation, so sort them around the centroid
// instead of trusting the spec order.
static void quadPoly(const AnnotQuad& a, Poly& out) {
    double cx = 0, cy = 0;
    for (int k = 0; k < 4; k++) { cx += a.x[k]; cy += a.y[k]; }
    cx /= 4; cy /= 4;
    int idx[4] = {0, 1, 2, 3};
    std::sort(idx, idx + 4, [&](int p, int q) {
        return std::atan2(a.y[p] - cy, a.x[p] - cx) < std::atan2(a.y[q] - cy, a.x[q] - cx);
    });
    out.clear();
    for (int k = 0; k < 4; k++) out.push_back({(float)a.x[idx[k]], (float)a.y[idx[k]]});
}

// The bottom edge of a quad, its inward normal and the text height it covers.
static void quadFrame(const Poly& q, Pt& p0, Pt& p1, Pt& n, double& h) {
    double cx = 0, cy = 0, miny = 1e30, maxy = -1e30;
    for (const Pt& p : q) {
        cx += p.x; cy += p.y;
        miny = std::min(miny, (double)p.y); maxy = std::max(maxy, (double)p.y);
    }
    cx /= 4; cy /= 4;
    h = maxy - miny > 0.2 ? maxy - miny : 1.0;
    double best = 1e30;
    p0 = q[0]; p1 = q[1];
    for (size_t i = 0; i < q.size(); i++) {
        const Pt& a = q[i];
        const Pt& b = q[(i + 1) % q.size()];
        double mid = (a.y + b.y) * 0.5;
        if (mid < best) { best = mid; p0 = a; p1 = b; }
    }
    double dx = p1.x - p0.x, dy = p1.y - p0.y;
    double len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-6) { n = {0, 1}; return; }
    n = {(float)(-dy / len), (float)(dx / len)};
    // point it into the quad
    double mx = (p0.x + p1.x) * 0.5, my = (p0.y + p1.y) * 0.5;
    if (n.x * (cx - mx) + n.y * (cy - my) < 0) { n.x = -n.x; n.y = -n.y; }
}

static void addBar(Poly& out, const Pt& p0, const Pt& p1, const Pt& n,
                   double up, double thick) {
    out.clear();
    out.push_back({(float)(p0.x + n.x * up), (float)(p0.y + n.y * up)});
    out.push_back({(float)(p1.x + n.x * up), (float)(p1.y + n.y * up)});
    out.push_back({(float)(p1.x + n.x * (up + thick)), (float)(p1.y + n.y * (up + thick))});
    out.push_back({(float)(p0.x + n.x * (up + thick)), (float)(p0.y + n.y * (up + thick))});
}

static void annotColor(Document* doc, const ObjP& an, bool markup, float* rgb) {
    rgb[0] = markup ? 1.f : 0.98f;
    rgb[1] = markup ? 0.90f : 0.86f;
    rgb[2] = markup ? 0.f : 0.32f;
    ObjP c = doc->resolve(an->get("C"));
    if (!c || c->t != O_ARR || c->arr.empty()) return;
    std::vector<double> v;
    for (auto& e : c->arr) v.push_back(num(doc->resolve(e), 0));
    if (v.size() == 1) { rgb[0] = rgb[1] = rgb[2] = (float)v[0]; }
    else if (v.size() == 3) { for (int i = 0; i < 3; i++) rgb[i] = (float)v[i]; }
    else if (v.size() >= 4) cmykToRgb(v[0], v[1], v[2], v[3], rgb);
}

static void annotFill(Canvas& cv, const Mat& ctm, const Polys& pagePolys,
                      const float* rgb, float alpha) {
    Polys dev;
    for (const Poly& p : pagePolys) {
        if (p.size() < 3) continue;
        Poly d;
        for (const Pt& pt : p) {
            double x, y;
            ctm.apply(pt.x, pt.y, x, y);
            d.push_back({(float)x, (float)y});
        }
        dev.push_back(std::move(d));
    }
    if (!dev.empty()) FillPolys(cv, dev, false, rgb[0], rgb[1], rgb[2], alpha, nullptr);
}

// A paper icon with a folded corner and three ruled lines, sized from /Rect.
static void paintNoteIcon(Document* doc, Canvas& cv, const Mat& ctm, const ObjP& an) {
    ObjP r = doc->resolve(an->get("Rect"));
    if (!r || r->t != O_ARR || r->arr.size() < 4) return;
    double v[4];
    for (int i = 0; i < 4; i++) v[i] = num(doc->resolve(r->arr[i]), 0);
    double x0 = std::min(v[0], v[2]), x1 = std::max(v[0], v[2]);
    double y0 = std::min(v[1], v[3]), y1 = std::max(v[1], v[3]);
    double w = x1 - x0, h = y1 - y0;
    if (w < 2 || h < 2) return;
    if (w > 4 * h) w = h;            // a /Rect spanning the page is not an icon
    if (h > 4 * w) h = w;

    float base[3];
    annotColor(doc, an, false, base);
    float body[3] = {base[0] * 0.25f + 0.75f, base[1] * 0.25f + 0.75f, base[2] * 0.25f + 0.75f};
    float ink[3] = {0.25f, 0.22f, 0.12f};

    double f = std::min(w, h) * 0.34;     // folded corner
    double bw = std::max(0.6, std::min(w, h) * 0.06);   // border width

    Polys page;
    Poly outer = {{(float)x0, (float)y0}, {(float)x1, (float)y0},
                  {(float)x1, (float)(y1 - f)}, {(float)(x1 - f), (float)y1},
                  {(float)x0, (float)y1}};
    page.push_back(outer);
    annotFill(cv, ctm, page, ink, 1.f);

    page.clear();
    Poly inner = {{(float)(x0 + bw), (float)(y0 + bw)}, {(float)(x1 - bw), (float)(y0 + bw)},
                  {(float)(x1 - bw), (float)(y1 - f + bw * 0.5)},
                  {(float)(x1 - f + bw * 0.5), (float)(y1 - bw)},
                  {(float)(x0 + bw), (float)(y1 - bw)}};
    page.push_back(inner);
    annotFill(cv, ctm, page, body, 1.f);

    // fold: a darker triangle in the cut corner
    page.clear();
    page.push_back({{(float)(x1 - f), (float)(y1 - bw)},
                    {(float)(x1 - bw), (float)(y1 - f)},
                    {(float)(x1 - f), (float)(y1 - f + bw * 0.5)}});
    annotFill(cv, ctm, page, ink, 1.f);

    // ruled lines
    double lx0 = x0 + w * 0.22, lx1 = x1 - w * 0.22;
    double lh = std::max(0.5, h * 0.07);
    for (int i = 0; i < 3; i++) {
        double ly = y0 + h * (0.26 + i * 0.18);
        page.clear();
        page.push_back({{(float)lx0, (float)ly}, {(float)lx1, (float)ly},
                        {(float)lx1, (float)(ly + lh)}, {(float)lx0, (float)(ly + lh)}});
        annotFill(cv, ctm, page, ink, 0.8f);
    }
}

// Returns true when something was painted for an annotation that has no
// usable appearance stream.
static bool paintAnnotFallback(Document* doc, Canvas& cv, const Mat& ctm,
                               const std::string& sub, const ObjP& an) {
    if (sub == "Text" || sub == "Comment" || sub == "Note") {
        paintNoteIcon(doc, cv, ctm, an);
        return true;
    }
    if (sub != "Highlight" && sub != "Underline" && sub != "StrikeOut" &&
        sub != "Squiggly")
        return false;

    std::vector<AnnotQuad> quads;
    if (!annotQuads(doc, an, quads)) return false;

    float rgb[3];
    annotColor(doc, an, true, rgb);
    double ca = num(doc->resolve(an->get("CA")), -1);
    float alpha = ca >= 0 ? (float)std::min(1.0, std::max(0.0, ca))
                          : (sub == "Highlight" ? 0.45f : 1.f);

    Polys page;
    for (const AnnotQuad& a : quads) {
        Poly q, bar;
        quadPoly(a, q);
        if (sub == "Highlight") {
            page.push_back(q);
            continue;
        }
        Pt p0, p1, n;
        double h = 0;
        quadFrame(q, p0, p1, n, h);
        if (sub == "Underline") {
            double th = std::max(0.6, h * 0.07);
            addBar(bar, p0, p1, n, h * 0.03, th);
            page.push_back(bar);
        } else if (sub == "StrikeOut") {
            double th = std::max(0.6, h * 0.07);
            addBar(bar, p0, p1, n, h * 0.45, th);
            page.push_back(bar);
        } else {   // Squiggly
            double amp = std::max(0.6, h * 0.07), th = std::max(0.5, h * 0.05);
            double len = std::hypot(p1.x - p0.x, p1.y - p0.y);
            int segs = (int)std::max(4.0, std::min(160.0, len / std::max(0.8, h * 0.28)));
            Pt prev = {p0.x, p0.y};
            for (int i = 1; i <= segs; i++) {
                double t = (double)i / segs;
                double up = h * 0.30 + ((i & 1) ? amp : -amp);
                Pt cur = {(float)(p0.x + (p1.x - p0.x) * t + n.x * up),
                          (float)(p0.y + (p1.y - p0.y) * t + n.y * up)};
                // thin quad around the segment
                double dx = cur.x - prev.x, dy = cur.y - prev.y;
                double l = std::hypot(dx, dy);
                if (l > 1e-6) {
                    float nx = (float)(-dy / l * th * 0.5), ny = (float)(dx / l * th * 0.5);
                    Poly segp = {{(float)(prev.x + nx), (float)(prev.y + ny)},
                                 {(float)(cur.x + nx), (float)(cur.y + ny)},
                                 {(float)(cur.x - nx), (float)(cur.y - ny)},
                                 {(float)(prev.x - nx), (float)(prev.y - ny)}};
                    page.push_back(segp);
                }
                prev = cur;
            }
        }
    }
    annotFill(cv, ctm, page, rgb, alpha);
    return true;
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
    err.clear();
    const long long px = (long long)dw * dh;
    if (px > 40 * 1000 * 1000) {
        char b[160];
        snprintf(b, sizeof b,
                 "page needs %dx%d px (%.1f MP) at this scale; the renderer caps a page at 40 MP",
                 dw, dh, px / 1e6);
        err = b;
        return false;
    }
    // Probe the heap before vector::assign: with -fno-exceptions an allocation
    // failure there aborts the module instead of returning false.
    {
        size_t need = (size_t)px * 4;
        void* probe = malloc(need);
        if (!probe) {
            char b[160];
            snprintf(b, sizeof b,
                     "not enough memory for a %dx%d px page (%.0f MB); the wasm heap could not grow",
                     dw, dh, need / 1048576.0);
            err = b;
            return false;
        }
        free(probe);
    }

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

    // Annotation appearance streams. Signatures, stamps (materai) and form
    // widgets live only in /AP /N: the page content stream never draws them.
    // Markup annotations and text notes are allowed to omit the appearance;
    // paintAnnotFallback synthesises those from /QuadPoints, /C and /Rect.
    ObjP annots = doc->resolve(page->get("Annots"));
    if (annots && annots->t == O_ARR) {
        std::string adata;
        for (auto& entry : annots->arr) {
            ObjP an = doc->resolve(entry);
            if (!an || an->t != O_DICT) continue;
            int flags = (int)num(doc->resolve(an->get("F")), 0);
            if (flags & 0x02) continue;    // Hidden
            if (flags & 0x20) continue;    // NoView
            std::string sub = objName(doc->resolve(an->get("Subtype")));
            bool painted = false;

            ObjP ap = doc->resolve(an->get("AP"));
            ObjP form = (ap && ap->t == O_DICT) ? doc->resolve(ap->get("N")) : nullptr;
            if (form && form->t == O_DICT && !form->isStream) {   // keyed by /AS
                ObjP as = doc->resolve(an->get("AS"));
                form = (as && as->t == O_NAME) ? doc->resolve(form->get(as->str.c_str())) : nullptr;
            }
            ObjP rect = doc->resolve(an->get("Rect"));
            double r[4];
            bool rectOk = rect && rect->t == O_ARR && rect->arr.size() >= 4;
            if (rectOk)
                for (int i = 0; i < 4; i++) r[i] = num(doc->resolve(rect->arr[i]), 0);

            if (form && form->isStream && rectOk) {
                double rx0 = std::min(r[0], r[2]), rx1 = std::max(r[0], r[2]);
                double ry0 = std::min(r[1], r[3]), ry1 = std::max(r[1], r[3]);
                if (rx1 - rx0 >= 0.5 && ry1 - ry0 >= 0.5) {
                    double bx0 = 0, by0 = 0, bx1 = 1, by1 = 1;
                    ObjP bbox = doc->resolve(form->get("BBox"));
                    if (bbox && bbox->t == O_ARR && bbox->arr.size() == 4) {
                        // Corners may be written in any order (many producers
                        // emit [0 h w 0]); normalise or the height comes out
                        // negative and the appearance is dropped.
                        double v[4];
                        for (int i = 0; i < 4; i++) v[i] = num(doc->resolve(bbox->arr[i]), i < 2 ? 0 : 1);
                        bx0 = std::min(v[0], v[2]); bx1 = std::max(v[0], v[2]);
                        by0 = std::min(v[1], v[3]); by1 = std::max(v[1], v[3]);
                    }
                    double bw = bx1 - bx0, bh = by1 - by0;
                    if (bw > 0 && bh > 0 && doc->decodeStream(form, adata) && !adata.empty()) {
                        double sx = (rx1 - rx0) / bw, sy = (ry1 - ry0) / bh;
                        Mat place(sx, 0, 0, sy, rx0 - bx0 * sx, ry0 - by0 * sy);
                        ObjP fm = doc->resolve(form->get("Matrix"));
                        if (fm && fm->t == O_ARR && fm->arr.size() == 6) {
                            Mat fmat(num(doc->resolve(fm->arr[0]), 1), num(doc->resolve(fm->arr[1]), 0),
                                     num(doc->resolve(fm->arr[2]), 0), num(doc->resolve(fm->arr[3]), 1),
                                     num(doc->resolve(fm->arr[4]), 0), num(doc->resolve(fm->arr[5]), 0));
                            place = fmat.mul(place);
                        }
                        GState ags = gs;
                        ags.ctm = place.mul(ctm);
                        Interp ai;
                        ai.doc = doc;
                        ai.cv = &cv;
                        ai.st = st;
                        ai.textSink = nullptr;
                        ai.devToPage = ags.ctm.inverse();
                        ai.exec(adata, doc->resolve(form->get("Resources")), ags);
                        painted = true;
                    }
                }
            }

            if (!painted && sub != "Link" && sub != "Popup")
                paintAnnotFallback(doc, cv, ctm, sub, an);
        }
    }

    out.w = dw;
    out.h = dh;
    out.rgba.swap(cv.px);
    lastW = dw;
    lastH = dh;
    return true;
}

} // namespace pdf
