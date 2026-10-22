#include "cfffont.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

#include "cff-parser.hh"   // standard strings / standard encoding tables

namespace pdf {

namespace {

using nanopdf::cff::get_standard_string;
using nanopdf::cff::standard_encoding_name;

inline uint16_t rd16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
inline uint32_t rd32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// DICT numeric operand reader (CFF spec 5.1).
bool dictNumber(const uint8_t* p, size_t n, size_t& i, double& v) {
    if (i >= n) return false;
    uint8_t b0 = p[i];
    if (b0 >= 32 && b0 <= 246) { v = (int)b0 - 139; i++; return true; }
    if (b0 >= 247 && b0 <= 250) {
        if (i + 1 >= n) return false;
        v = (b0 - 247) * 256 + p[i + 1] + 108; i += 2; return true;
    }
    if (b0 >= 251 && b0 <= 254) {
        if (i + 1 >= n) return false;
        v = -(b0 - 251) * 256 - (int)p[i + 1] - 108; i += 2; return true;
    }
    if (b0 == 28) {
        if (i + 2 >= n) return false;
        v = (int16_t)((p[i + 1] << 8) | p[i + 2]); i += 3; return true;
    }
    if (b0 == 29) {
        if (i + 4 >= n) return false;
        v = (int32_t)rd32(p + i + 1); i += 5; return true;
    }
    if (b0 == 30) {   // real, BCD nibbles
        i++;
        std::string s;
        bool done = false;
        while (i < n && !done) {
            uint8_t byte = p[i++];
            for (int half = 0; half < 2 && !done; half++) {
                uint8_t nib = half == 0 ? (byte >> 4) : (byte & 0xF);
                switch (nib) {
                    case 0xA: s += '.'; break;
                    case 0xB: s += 'E'; break;
                    case 0xC: s += "E-"; break;
                    case 0xE: s += '-'; break;
                    case 0xF: done = true; break;
                    default: s += (char)('0' + nib); break;
                }
            }
        }
        v = s.empty() ? 0 : strtod(s.c_str(), nullptr);
        return true;
    }
    return false;
}

bool dictGetInt(const std::map<int, std::vector<double>>& d, int op, double& out) {
    auto it = d.find(op);
    if (it == d.end() || it->second.empty()) return false;
    out = it->second[0];
    return true;
}

// ─── Type2 charstring interpreter ───────────────────────────────────────────
struct T2 {
    const CffFont* font;
    const uint8_t* data;
    size_t size;
    const std::vector<std::pair<size_t, size_t>>* gsubrs;
    const std::vector<std::pair<size_t, size_t>>* lsubrs;
    size_t gbias = 0, lbias = 0;
    std::vector<CffSeg>* out;
    double sx = 0.001, sy = 0.001;   // em scale (FontMatrix)

    double st[48];
    int sp = 0;
    double tr[32];
    double x = 0, y = 0;
    bool open = false;
    int  nStems = 0;
    bool widthSet = false;
    double width = 0;
    int  depth = 0;

    void push(double v) { if (sp < 48) st[sp++] = v; }
    void clear() { sp = 0; }
    void moveTo(double nx, double ny) {
        out->push_back({0, 0, 0, 0, 0, nx * sx, ny * sy});
        x = nx; y = ny; open = true;
    }
    void lineTo(double nx, double ny) {
        if (!open) { moveTo(nx, ny); return; }
        out->push_back({1, 0, 0, 0, 0, nx * sx, ny * sy});
        x = nx; y = ny;
    }
    void curveTo(double c1x, double c1y, double c2x, double c2y, double nx, double ny) {
        if (!open) { moveTo(c1x, c1y); }
        out->push_back({2, c1x * sx, c1y * sy, c2x * sx, c2y * sy, nx * sx, ny * sy});
        x = nx; y = ny;
    }
    // A width argument is present only when a stem operator sees an ODD
    // argument count (the extra one is the width). Even counts are pure hints;
    // marking widthSet unconditionally makes every glyph take nominalWidthX.
    void takeWidth(int expectedEvenPairs) {
        if (widthSet) return;
        if (expectedEvenPairs && (sp % 2) == 1) {
            width = st[0];
            for (int i = 1; i < sp; i++) st[i - 1] = st[i];
            sp--;
            widthSet = true;
        }
    }
    bool run(size_t start, size_t end, int inner);
};

bool T2::run(size_t start, size_t end, int inner) {
    if (depth > 10) return false;
    depth++;
    size_t i = start;
    bool returned = false;
    while (i < end) {
        uint8_t b0 = data[i++];
        if (b0 >= 32 || b0 == 28) {
            i--;
            double v;
            size_t save = i;
            // charstring numbers share the DICT encoding, except 255 = 16.16.
            if (b0 == 255) {
                if (i + 4 >= end) break;
                v = (int32_t)rd32(data + i + 1) / 65536.0;
                i += 5;
            } else if (dictNumber(data, end, i, v)) {
            } else {
                i = save + 1;
                v = 0;
            }
            push(v);
            continue;
        }
        switch (b0) {
            case 1: case 3: case 18: case 23: {   // stems
                takeWidth(true);
                nStems += sp / 2;
                clear();
                break;
            }
            case 19: case 20: {                    // hintmask / cntrmask
                if (sp > 0) { takeWidth(true); nStems += sp / 2; clear(); }
                size_t bytes = (size_t)((nStems + 7) / 8);
                if (i + bytes > end) { depth--; return true; }
                i += bytes;
                break;
            }
            // Width is present only when the stack holds ONE argument more
            // than the operator consumes. Extra pairs before a moveto are
            // implicit vstem hints (the vstem operator is optional), not a
            // width — treating them as one mangles the outline.
            case 21: {                             // rmoveto
                if (sp > 2 && ((sp - 2) % 2) == 1) { width = st[0]; widthSet = true;
                              for (int k = 1; k < sp; k++) st[k - 1] = st[k]; sp--; }
                if (sp >= 2) moveTo(x + st[sp - 2], y + st[sp - 1]);
                clear();
                break;
            }
            case 22: {                             // hmoveto
                if (sp > 1 && ((sp - 1) % 2) == 1) { width = st[0]; widthSet = true;
                              for (int k = 1; k < sp; k++) st[k - 1] = st[k]; sp--; }
                if (sp >= 1) moveTo(x + st[sp - 1], y);
                clear();
                break;
            }
            case 4: {                               // vmoveto
                if (sp > 1 && ((sp - 1) % 2) == 1) { width = st[0]; widthSet = true;
                              for (int k = 1; k < sp; k++) st[k - 1] = st[k]; sp--; }
                if (sp >= 1) moveTo(x, y + st[sp - 1]);
                clear();
                break;
            }
            case 5: {                               // rlineto
                for (int k = 0; k + 1 < sp; k += 2) lineTo(x + st[k], y + st[k + 1]);
                clear();
                break;
            }
            case 6: case 7: {                        // hlineto / vlineto
                bool horiz = b0 == 6;
                for (int k = 0; k < sp; k++) {
                    if (horiz) lineTo(x + st[k], y); else lineTo(x, y + st[k]);
                    horiz = !horiz;
                }
                clear();
                break;
            }
            case 8: {                                // rrcurveto
                for (int k = 0; k + 5 < sp; k += 6) {
                    double c1x = x + st[k], c1y = y + st[k + 1];
                    double c2x = c1x + st[k + 2], c2y = c1y + st[k + 3];
                    curveTo(c1x, c1y, c2x, c2y, c2x + st[k + 4], c2y + st[k + 5]);
                }
                clear();
                break;
            }
            case 24: {                               // rcurveline
                int k = 0;
                for (; k + 5 < sp - 2; k += 6) {
                    double c1x = x + st[k], c1y = y + st[k + 1];
                    double c2x = c1x + st[k + 2], c2y = c1y + st[k + 3];
                    curveTo(c1x, c1y, c2x, c2y, c2x + st[k + 4], c2y + st[k + 5]);
                }
                if (k + 1 < sp) lineTo(x + st[k], y + st[k + 1]);
                clear();
                break;
            }
            case 25: {                               // rlinecurve
                int k = 0;
                for (; k + 1 < sp - 6; k += 2) lineTo(x + st[k], y + st[k + 1]);
                if (k + 5 < sp) {
                    double c1x = x + st[k], c1y = y + st[k + 1];
                    double c2x = c1x + st[k + 2], c2y = c1y + st[k + 3];
                    curveTo(c1x, c1y, c2x, c2y, c2x + st[k + 4], c2y + st[k + 5]);
                }
                clear();
                break;
            }
            case 26: case 27: {                      // vvcurveto / hhcurveto
                int k = 0;
                double d1 = 0;
                if (sp % 2 == 1) { d1 = st[0]; k = 1; }
                if (b0 == 26) {
                    for (; k + 3 < sp; k += 4) {
                        double c1x = x + d1, c1y = y + st[k];
                        double c2x = c1x + st[k + 1], c2y = c1y + st[k + 2];
                        curveTo(c1x, c1y, c2x, c2y, c2x, c2y + st[k + 3]);
                        d1 = 0;
                    }
                } else {
                    for (; k + 3 < sp; k += 4) {
                        double c1x = x + st[k], c1y = y + d1;
                        double c2x = c1x + st[k + 1], c2y = c1y + st[k + 2];
                        curveTo(c1x, c1y, c2x, c2y, c2x + st[k + 3], c2y);
                        d1 = 0;
                    }
                }
                clear();
                break;
            }
            case 30: case 31: {                      // vhcurveto / hvcurveto
                int k = 0;
                bool horiz = b0 == 31;
                while (k + 3 < sp) {
                    // hvcurveto: dx1 dx2 dy2 dy3 [dx3]
                    // vhcurveto: dy1 dx2 dy2 dx3 [dy3]
                    // The bracketed fifth value is the alternating axis of the
                    // endpoint — swapping it with the mandatory one bends every
                    // curve off-axis while leaving simple glyphs plausible.
                    bool extra = (sp - k == 5);   // the extra value belongs to the LAST curve
                    if (horiz) {
                        double c1x = x + st[k], c1y = y;
                        double c2x = c1x + st[k + 1], c2y = c1y + st[k + 2];
                        curveTo(c1x, c1y, c2x, c2y,
                                c2x + (extra ? st[k + 4] : 0), c2y + st[k + 3]);
                    } else {
                        double c1x = x, c1y = y + st[k];
                        double c2x = c1x + st[k + 1], c2y = c1y + st[k + 2];
                        curveTo(c1x, c1y, c2x, c2y,
                                c2x + st[k + 3], c2y + (extra ? st[k + 4] : 0));
                    }
                    horiz = !horiz;
                    k += 4;
                }
                clear();
                break;
            }
            case 10: case 29: {                      // callsubr / callgsubr
                if (sp < 1) break;
                double idx = st[--sp];
                const std::vector<std::pair<size_t, size_t>>* subs =
                    b0 == 10 ? lsubrs : gsubrs;
                size_t bias = b0 == 10 ? lbias : gbias;
                if (!subs || idx != idx) break;
                long n = (long)idx + (long)bias;
                if (n < 0 || (size_t)n >= subs->size()) break;
                auto& r = (*subs)[(size_t)n];
                run(r.first, r.second, 1);
                break;
            }
            case 11:                                  // return
                returned = true;
                depth--;
                return true;
            case 14: {                                // endchar
                if (!widthSet && (sp == 1 || sp == 5)) {
                    width = st[0];
                    for (int k = 1; k < sp; k++) st[k - 1] = st[k];
                    sp--;
                    widthSet = true;
                }
                if (sp >= 4) {
                    double adx = st[sp - 4], ady = st[sp - 3];
                    int bchar = (int)st[sp - 2], achar = (int)st[sp - 1];
                    clear();
                    int bg = font->seacGid(bchar), ag = font->seacGid(achar);
                    double baseX = x, baseY = y;
                    if (bg >= 0) {
                        std::vector<CffSeg> tmp;
                        if (font->glyphPathImpl(bg, tmp, depth + 1)) {
                            for (auto& s : tmp) out->push_back(s);
                        }
                    }
                    if (ag >= 0) {
                        std::vector<CffSeg> tmp;
                        if (font->glyphPathImpl(ag, tmp, depth + 1)) {
                            for (auto& s : tmp) {
                                CffSeg t = s;
                                double ox = (adx * sx) + baseX;
                                double oy = (ady * sy) + baseY;
                                t.x += ox; t.y += oy;
                                t.x1 += ox; t.y1 += oy;
                                t.x2 += ox; t.y2 += oy;
                                out->push_back(t);
                            }
                        }
                    }
                    (void)baseX; (void)baseY;
                }
                clear();
                depth--;
                return true;
            }
            case 12: {                                // escape
                if (i >= end) break;
                uint8_t b1 = data[i++];
                switch (b1) {
                    case 35: {                        // flex
                        if (sp >= 13) {
                            curveTo(x + st[0], y + st[1], x + st[0] + st[2], y + st[1] + st[3],
                                    x + st[0] + st[2] + st[4], y + st[1] + st[3] + st[5]);
                            double bx = x, by = y;
                            curveTo(bx + st[6], by + st[7], bx + st[6] + st[8], by + st[7] + st[9],
                                    bx + st[6] + st[8] + st[10], by + st[7] + st[9] + st[11]);
                        }
                        clear();
                        break;
                    }
                    case 34: {                        // hflex
                        if (sp >= 7) {
                            curveTo(x + st[0], y, x + st[0] + st[1], y + st[2],
                                    x + st[0] + st[1] + st[3], y + st[2]);
                            double bx = x, by = y;
                            curveTo(bx + st[4], by + st[2], bx + st[4] + st[5], by,
                                    bx + st[4] + st[5] + st[6], by);
                        }
                        clear();
                        break;
                    }
                    case 36: {                        // hflex1
                        if (sp >= 9) {
                            curveTo(x + st[0], y + st[1], x + st[0] + st[2], y + st[1] + st[3],
                                    x + st[0] + st[2] + st[4], y + st[1] + st[3]);
                            double bx = x, by = y;
                            double dy = -(st[1] + st[3] + st[5] + st[6]);
                            curveTo(bx + st[5], by + st[6], bx + st[5] + st[7], by + st[6] + dy,
                                    bx + st[5] + st[7] + st[8], by + st[6] + dy);
                        }
                        clear();
                        break;
                    }
                    case 37: {                        // flex1
                        if (sp >= 11) {
                            double dx = st[0] + st[2] + st[4] + st[6] + st[8];
                            double dy = st[1] + st[3] + st[5] + st[7] + st[9];
                            curveTo(x + st[0], y + st[1], x + st[0] + st[2], y + st[1] + st[3],
                                    x + st[0] + st[2] + st[4], y + st[1] + st[3] + st[5]);
                            double bx = x, by = y;
                            double ex = fabs(dx) > fabs(dy) ? st[10] : dx;
                            double ey = fabs(dx) > fabs(dy) ? dy : st[10];
                            curveTo(bx + st[6], by + st[7], bx + st[6] + st[8], by + st[7] + st[9],
                                    bx + st[6] + st[8] + ex, by + st[7] + st[9] + ey);
                        }
                        clear();
                        break;
                    }
                    case 3: if (sp >= 2) { st[sp - 2] = (st[sp - 2] && st[sp - 1]) ? 1 : 0; sp--; } break;
                    case 4: if (sp >= 2) { st[sp - 2] = (st[sp - 2] || st[sp - 1]) ? 1 : 0; sp--; } break;
                    case 5: if (sp >= 1) st[sp - 1] = st[sp - 1] ? 0 : 1; break;
                    case 9: if (sp >= 1) st[sp - 1] = fabs(st[sp - 1]); break;
                    case 10: if (sp >= 2) { st[sp - 2] += st[sp - 1]; sp--; } break;
                    case 11: if (sp >= 2) { st[sp - 2] -= st[sp - 1]; sp--; } break;
                    case 12: if (sp >= 2) { st[sp - 2] = st[sp - 1] != 0 ? st[sp - 2] / st[sp - 1] : 0; sp--; } break;
                    case 14: if (sp >= 1) st[sp - 1] = -st[sp - 1]; break;
                    case 15: if (sp >= 2) { st[sp - 2] = (st[sp - 2] == st[sp - 1]) ? 1 : 0; sp--; } break;
                    case 18: if (sp >= 1) sp--; break;
                    case 20: if (sp >= 2) { int idx2 = (int)st[--sp];
                                 if (idx2 >= 0 && idx2 < 32) tr[idx2] = st[sp - 1];
                                 sp--; } break;
                    case 21: if (sp >= 1) { int idx2 = (int)st[--sp];
                                 push(idx2 >= 0 && idx2 < 32 ? tr[idx2] : 0); } break;
                    case 22: if (sp >= 4) { double v2 = st[sp-4] <= st[sp-3] ? st[sp-2] : st[sp-1];
                                 sp -= 3; st[sp-1] = v2; } break;
                    case 23: push(0.5); break;
                    case 24: if (sp >= 2) { st[sp - 2] *= st[sp - 1]; sp--; } break;
                    case 26: if (sp >= 1) st[sp - 1] = std::sqrt(std::max(0.0, st[sp - 1])); break;
                    case 27: if (sp >= 1) push(st[sp - 1]); break;
                    case 28: if (sp >= 2) std::swap(st[sp - 1], st[sp - 2]); break;
                    case 29: if (sp >= 1) { int idx2 = (int)st[--sp];
                                 if (idx2 < 0) idx2 = 0;
                                 if (idx2 >= sp) idx2 = sp - 1;
                                 push(st[sp - 1 - idx2]); } break;
                    case 30: {                        // roll
                        if (sp >= 2) {
                            int n = (int)st[--sp], k2 = (int)st[--sp];
                            if (n > 0 && n <= sp) {
                                k2 = ((k2 % n) + n) % n;
                                for (int r = 0; r < k2; r++) {
                                    double last = st[sp - 1];
                                    for (int q = sp - 1; q > sp - n; q--) st[q] = st[q - 1];
                                    st[sp - n] = last;
                                }
                            }
                        }
                        break;
                    }
                    default: break;
                }
                break;
            }
            default: break;
        }
    }
    (void)returned;
    depth--;
    return true;
}

}  // namespace

// ─── CffFont ────────────────────────────────────────────────────────────────
CffFont::Index CffFont::readIndex(size_t pos) const {
    Index idx;
    const uint8_t* d = data_.data();
    size_t n = data_.size();
    if (pos + 2 > n) return idx;
    uint16_t count = rd16(d + pos);
    if (count == 0) { idx.end = pos + 2; return idx; }
    if (pos + 3 > n) return idx;
    uint8_t offSize = d[pos + 2];
    if (offSize < 1 || offSize > 4) return idx;
    size_t offArr = pos + 3;
    size_t offBytes = (size_t)(count + 1) * offSize;
    if (offArr + offBytes > n) return idx;
    auto readOff = [&](size_t k) -> uint32_t {
        const uint8_t* p = d + offArr + k * offSize;
        uint32_t v = 0;
        for (uint8_t i = 0; i < offSize; i++) v = (v << 8) | p[i];
        return v;
    };
    uint32_t base = readOff(0);
    size_t dataStart = offArr + offBytes;
    if (base == 0) return idx;
    dataStart += base - 1;
    for (uint16_t i = 0; i < count; i++) {
        uint32_t a = readOff(i), b = readOff(i + 1);
        if (b < a) break;
        size_t s = dataStart + (a - base), e = dataStart + (b - base);
        if (e > n) break;
        idx.items.push_back({s, e});
    }
    idx.end = dataStart + (readOff(count) - base);
    if (idx.end > n) idx.end = n;
    return idx;
}

bool CffFont::readDict(size_t start, size_t end,
                       std::map<int, std::vector<double>>& out) const {
    size_t i = start;
    const uint8_t* d = data_.data();
    std::vector<double> ops;
    while (i < end) {
        uint8_t b0 = d[i];
        if (b0 <= 21) {
            int op = b0;
            i++;
            if (b0 == 12) {
                if (i >= end) break;
                op = 1200 + d[i];
                i++;
            }
            out[op] = ops;
            ops.clear();
            continue;
        }
        double v;
        if (!dictNumber(d, end, i, v)) { i++; continue; }
        ops.push_back(v);
    }
    return true;
}

bool CffFont::parsePrivate(const std::map<int, std::vector<double>>& pd, size_t base,
                           FdInfo& fd) const {
    double subrsOff = 0;
    if (dictGetInt(pd, 19, subrsOff)) {
        Index subrs = readIndex(base + (size_t)subrsOff);
        fd.subrsFirst = 0;
        fd.nSubrs = (int)subrs.items.size();
        fd.subrsList = subrs.items;
    }
    double dw = 0, nw = 0;
    if (dictGetInt(pd, 20, dw)) fd.defaultWidth = dw;
    if (dictGetInt(pd, 21, nw)) fd.nominalWidth = nw;
    return true;
}

bool CffFont::parseCharset(int offset, bool cid) {
    const uint8_t* d = data_.data();
    size_t n = data_.size();
    if (cid) {
        cidOfGid_.assign((size_t)nGlyphs_, 0);
    } else {
        glyphNames_.assign((size_t)nGlyphs_, std::string());
    }
    if (offset == 0) {   // predefined ISOAdobe
        for (int g = 0; g < nGlyphs_; g++) {
            int sid = g;
            const char* nm = get_standard_string(sid);
            if (cid) cidOfGid_[g] = g;
            else glyphNames_[g] = nm ? nm : std::string();
        }
        return true;
    }
    if (offset <= 2) {   // predefined Expert / ExpertSubset — names unavailable
        for (int g = 0; g < nGlyphs_; g++) {
            if (cid) cidOfGid_[g] = g;
            else glyphNames_[g] = "g" + std::to_string(g);
        }
        return true;
    }
    size_t pos = (size_t)offset;
    if (pos >= n) return false;
    uint8_t fmt = d[pos++];
    int gid = 1;
    auto setGid = [&](int g, int value) {
        if (cid) { if (g < nGlyphs_) cidOfGid_[g] = value; }
        else {
            const char* nm = value < 391 ? get_standard_string(value) : nullptr;
            std::string name = nm ? nm : std::string();
            if (!nm && value >= 391 && (size_t)(value - 391) < strings_.size())
                name = strings_[value - 391];
            if (g < nGlyphs_) glyphNames_[g] = name.empty() ? ("g" + std::to_string(g)) : name;
        }
    };
    if (fmt == 0) {
        while (gid < nGlyphs_ && pos + 2 <= n) { setGid(gid++, rd16(d + pos)); pos += 2; }
    } else if (fmt == 1 || fmt == 2) {
        while (gid < nGlyphs_) {
            if (fmt == 1) {
                if (pos + 3 > n) break;
                int first = rd16(d + pos); int left = d[pos + 2]; pos += 3;
                for (int j = 0; j <= left && gid < nGlyphs_; j++) setGid(gid++, first + j);
            } else {
                if (pos + 4 > n) break;
                int first = rd16(d + pos); int left = rd16(d + pos + 2); pos += 4;
                for (int j = 0; j <= left && gid < nGlyphs_; j++) setGid(gid++, first + j);
            }
        }
    } else {
        return false;
    }
    return true;
}

bool CffFont::parseEncoding(int offset) {
    const uint8_t* d = data_.data();
    size_t n = data_.size();
    encToGid_.assign(256, 0);
    if (offset == 0) {          // StandardEncoding
        for (int c = 0; c < 256; c++) {
            const char* nm = standard_encoding_name(c);
            if (!nm) continue;
            int g = gidForName(nm);
            if (g >= 0) encToGid_[c] = g;
        }
        return true;
    }
    if (offset == 1) return true;   // Expert encoding: leave unmapped
    size_t pos = (size_t)offset;
    if (pos >= n) return false;
    uint8_t fmt = d[pos++];
    uint8_t base = 0, nCodes = 0;
    std::vector<int> codes;
    if (fmt & 0x7f) {   // supplemental encodings carry a base
        if (fmt & 0x80) {
            if (pos + 2 > n) return false;
            base = d[pos]; nCodes = d[pos + 1]; pos += 2;
        } else {
            if (pos + 1 > n) return false;
            base = d[pos++];
          }
        fmt &= 0x7f;
    }
    for (int c = 0; c < 256; c++) {
        const char* nm = base == 0 ? standard_encoding_name(c)
                                   : (base == 1 ? nullptr : nullptr);
        if (!nm) continue;
        int g = gidForName(nm);
        if (g >= 0) encToGid_[c] = g;
    }
    (void)nCodes;
    if (fmt == 0) {
        if (pos >= n) return true;
        int count = d[pos++];
        for (int i = 0; i < count && pos < n; i++) {
            int code = d[pos++];
            int sid = pos + 1 < n ? rd16(d + pos) : 0;
            pos += 2;
            const char* nm = sid < 391 ? get_standard_string(sid) : nullptr;
            if (nm) { int g = gidForName(nm); if (g >= 0) encToGid_[code] = g; }
        }
    } else if (fmt == 1) {
        if (pos >= n) return true;
        int nranges = d[pos++];
        for (int r = 0; r < nranges && pos + 3 <= n; r++) {
            int first = d[pos], left = d[pos + 1];
            int sid = rd16(d + pos + 2);
            pos += 3;
            for (int j = 0; j <= left; j++) {
                const char* nm = (sid + j) < 391 ? get_standard_string(sid + j) : nullptr;
                if (nm) { int g = gidForName(nm); if (g >= 0) encToGid_[first + j] = g; }
            }
        }
    }
    return true;
}

bool CffFont::parse(const uint8_t* data, size_t len) {
    if (!data || len < 4) return false;
    data_.assign(data, data + len);
    const uint8_t* d = data_.data();
    size_t n = data_.size();
    if (d[0] != 1) return false;                // only CFF version 1
    size_t hdrSize = d[2];
    if (hdrSize < 4 || hdrSize > n) return false;

    size_t pos = hdrSize;
    Index nameIdx = readIndex(pos);
    pos = nameIdx.end;
    Index topIdx = readIndex(pos);
    if (topIdx.items.empty()) return false;
    pos = topIdx.end;
    Index strIdx = readIndex(pos);
    for (auto& it : strIdx.items)
        strings_.emplace_back((const char*)d + it.first, it.second - it.first);
    pos = strIdx.end;
    gsubrs_ = readIndex(pos);

    std::map<int, std::vector<double>> top;
    readDict(topIdx.items[0].first, topIdx.items[0].second, top);

    double csOff = 0;
    if (!dictGetInt(top, 17, csOff) || csOff <= 0) return false;
    charStrings_ = readIndex((size_t)csOff);
    nGlyphs_ = (int)charStrings_.items.size();
    if (nGlyphs_ <= 0) return false;

    // FontMatrix decides the em scale (default 0.001).
    auto fmit = top.find(1231);
    if (fmit != top.end() && fmit->second.size() >= 4) {
        fmScale_ = fmit->second[0];
        if (fmScale_ == 0) fmScale_ = 0.001;
    }

    cid_ = top.find(1230) != top.end();     // ROS

    // Private dict (simple fonts) / FDArray + FDSelect (CID fonts).
    auto priv = top.find(18);
    if (priv != top.end() && priv->second.size() >= 2) {
        double size = priv->second[0], off = priv->second[1];
        std::map<int, std::vector<double>> pd;
        readDict((size_t)off, (size_t)(off + size), pd);
        parsePrivate(pd, (size_t)off, defFd_);
    }

    if (cid_) {
        fdOfGid_.assign((size_t)nGlyphs_, 0);
        double fdaOff = 0;
        if (dictGetInt(top, 1236, fdaOff) && fdaOff > 0) {
            Index fda = readIndex((size_t)fdaOff);
            for (auto& it : fda.items) {
                std::map<int, std::vector<double>> fdDict;
                readDict(it.first, it.second, fdDict);
                FdInfo fd;
                parsePrivate(fdDict, it.first, fd);
                fds_.push_back(fd);
            }
        }
        double fdsOff = 0;
        if (dictGetInt(top, 1237, fdsOff) && fdsOff > 0 && (size_t)fdsOff < n) {
            size_t p = (size_t)fdsOff;
            uint8_t fmt = d[p++];
            if (fmt == 0) {
                for (int g = 0; g < nGlyphs_ && p < n; g++) fdOfGid_[g] = d[p++];
            } else if (fmt == 3) {
                if (p + 2 <= n) {
                    int nr = rd16(d + p); p += 2;
                    size_t ranges = p;
                    p += (size_t)nr * 3;
                    uint16_t sentinel = (p + 2 <= n) ? rd16(d + p) : 0;
                    for (int r = 0; r < nr; r++) {
                        size_t rp = ranges + (size_t)r * 3;
                        int first = rd16(d + rp), fd = d[rp + 2];
                        int next = (r + 1 < nr) ? rd16(d + ranges + (size_t)(r + 1) * 3) : sentinel;
                        for (int g = first; g < next && g < nGlyphs_; g++) fdOfGid_[g] = fd;
                    }
                }
            }
        }
        if (fds_.empty()) fds_.push_back(defFd_);
    }

    double charsetOff = 0;
    bool hasCharset = dictGetInt(top, 15, charsetOff);
    if (hasCharset) {
        if (!parseCharset((int)charsetOff, cid_)) { /* keep going: identity names */ }
    } else {
        parseCharset(0, cid_);
    }

    double encOff = 0;
    bool hasEnc = dictGetInt(top, 16, encOff);
    if (!cid_) {
        // Name -> GID must exist before the encoding is decoded: the built-in
        // encoding resolves codes through glyph names, so filling it first
        // leaves every code unmapped (text then renders with a substitute face
        // or not at all).
        nameToGid_.clear();
        for (int g = 0; g < nGlyphs_ && g < (int)glyphNames_.size(); g++)
            if (!glyphNames_[g].empty() && !nameToGid_.count(glyphNames_[g]))
                nameToGid_[glyphNames_[g]] = g;
        if (hasEnc) parseEncoding((int)encOff);
        else parseEncoding(0);
    } else {
        cidToGid_.clear();
        for (int g = 0; g < (int)cidOfGid_.size(); g++)
            if (!cidToGid_.count(cidOfGid_[g]) || g < cidToGid_[cidOfGid_[g]])
                cidToGid_[cidOfGid_[g]] = g;
    }

    ok_ = true;
    return true;
}

int CffFont::gidForName(const std::string& name) const {
    auto it = nameToGid_.find(name);
    return it == nameToGid_.end() ? -1 : it->second;
}

int CffFont::gidForCode(int code) const {
    if (code < 0 || code > 255 || code >= (int)encToGid_.size()) return -1;
    return encToGid_[code] ? encToGid_[code] : -1;
}

int CffFont::gidForCid(int cid) const {
    auto it = cidToGid_.find(cid);
    if (it != cidToGid_.end()) return it->second;
    if (cid >= 0 && cid < nGlyphs_) return cid;   // identity
    return -1;
}

int CffFont::seacGid(int code) const {
    const char* nm = standard_encoding_name(code);
    if (!nm) return -1;
    return gidForName(nm);
}

double CffFont::advanceForGid(int gid) const {
    if (gid < 0 || gid >= nGlyphs_) return 0;
    auto it = widthCache_.find(gid);
    if (it != widthCache_.end()) return it->second;
    std::vector<CffSeg> segs;
    glyphPathImpl(gid, segs, 0);
    auto it2 = widthCache_.find(gid);
    return it2 == widthCache_.end() ? 0 : it2->second;
}

bool CffFont::glyphPath(int gid, std::vector<CffSeg>& out) const {
    out.clear();
    if (!ok_ || gid < 0 || gid >= nGlyphs_) return false;
    return glyphPathImpl(gid, out, 0);
}

bool CffFont::glyphPathImpl(int gid, std::vector<CffSeg>& out, int depth) const {
    if (gid < 0 || gid >= nGlyphs_ || depth > 4) return false;
    const FdInfo* fd = &defFd_;
    if (cid_ && gid < (int)fdOfGid_.size() && fdOfGid_[gid] >= 0 &&
        fdOfGid_[gid] < (int)fds_.size())
        fd = &fds_[fdOfGid_[gid]];

    T2 t2;
    t2.font = this;
    t2.data = data_.data();
    t2.size = data_.size();
    t2.gsubrs = &gsubrs_.items;
    t2.lsubrs = &fd->subrsList;
    t2.gbias = subrBias((int)gsubrs_.items.size());
    t2.lbias = subrBias(fd->nSubrs);
    t2.out = &out;
    t2.sx = fmScale_;
    t2.sy = fmScale_;
    t2.width = 0;

    auto& cs = charStrings_.items[(size_t)gid];
    t2.run(cs.first, cs.second, 0);
    // A charstring carries a delta from nominalWidthX; without one the glyph
    // uses defaultWidthX. Adding defaultWidth twice (or forgetting nominal)
    // shifts every advance by a constant, which is easy to miss visually.
    if (!t2.widthSet) t2.width = fd->defaultWidth;
    else t2.width = fd->nominalWidth + t2.width;
    widthCache_[gid] = t2.width * fmScale_;
    return true;
}

}  // namespace pdf
