// Native PDF document layer: lexer, xref, object model, filters, page tree,
// fonts. No MuPDF, no JS — everything here compiles into the same wasm module
// as the ImGui app and runs in C++.
//
// Scope: the object model is complete enough for ordinary print/LaTeX PDFs
// (classic xref tables *and* xref streams, incremental updates, object streams,
// Flate/LZW/ASCII85/ASCIIHex/RunLength filters with PNG+TIFF predictors).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace pdf {

enum ObjType { O_NULL, O_BOOL, O_INT, O_REAL, O_STR, O_NAME, O_ARR, O_DICT, O_REF };

struct Obj;
typedef std::shared_ptr<Obj> ObjP;

struct Obj {
    ObjType t = O_NULL;
    bool    b = false;
    double  num = 0;
    std::string str;                       // O_STR bytes / O_NAME without '/'
    std::vector<ObjP> arr;
    std::vector<std::pair<std::string, ObjP>> dict;
    int ref = 0, gen = 0;                  // O_REF
    bool isStream = false;
    std::string raw;                       // stream bytes, still encoded

    ObjP get(const char* k) const;
    bool has(const char* k) const;
    int  size() const { return (int)arr.size(); }
};

ObjP mkNull();
ObjP mkInt(long long v);
ObjP mkReal(double v);
ObjP mkName(const std::string& n);
ObjP mkStr(const std::string& s);
ObjP mkArr();
ObjP mkDict();

// 2x3 matrix, PDF row-vector convention: [a b c d e f]
struct Mat {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    Mat() {}
    Mat(double a_, double b_, double c_, double d_, double e_, double f_)
        : a(a_), b(b_), c(c_), d(d_), e(e_), f(f_) {}
    Mat mul(const Mat& m) const {   // this * m
        return Mat(a * m.a + b * m.c, a * m.b + b * m.d,
                   c * m.a + d * m.c, c * m.b + d * m.d,
                   e * m.a + f * m.c + m.e, e * m.b + f * m.d + m.f);
    }
    void apply(double x, double y, double& ox, double& oy) const {
        ox = a * x + c * y + e;
        oy = b * x + d * y + f;
    }
    Mat inverse() const {
        double det = a * d - b * c;
        if (std::fabs(det) < 1e-12) return Mat();
        double ia = d / det, ib = -b / det, ic = -c / det, id = a / det;
        return Mat(ia, ib, ic, id, -(ia * e + ic * f), -(ib * e + id * f));
    }
};

struct Font {
    std::string base, subtype;
    int    firstChar = 0, lastChar = 0;
    double missingWidth = 0;
    std::vector<double> widths;            // glyph space units (1/1000 em)
    std::map<int, std::string> diff;       // code -> glyph name (/Encoding /Differences)
    std::string baseEncoding;              // WinAnsiEncoding / MacRomanEncoding / StandardEncoding
    std::map<int, int> toUni;              // code -> unicode (from /ToUnicode)
    bool hasWidths = false;
    bool symbolic = false;                 // TrueType with no encoding: codes are glyph ids
    std::string embedded;                  // /FontFile2 TrueType bytes when present

    int  unicodeFor(int code) const;       // 0 when unmapped
    double widthFor(int code) const;       // text space (1 em == 1000)
    // Substitute face selectors derived from the BaseFont name.
    int  faceClass() const;                // 0 sans, 1 serif, 2 mono
    bool faceBold() const;
    bool faceItalic() const;
};

struct Document {
    std::string buf;
    std::map<int, int> objNumToStm;        // object streams: num -> container obj num
    std::map<int, long> xref;              // num -> byte offset (>=0) or -1 (in object stream)
    std::map<int, ObjP> cache;
    ObjP trailer;
    std::vector<ObjP> pages;               // page dicts, in document order
    std::vector<std::string> warnings;
    std::string error;
    bool  loaded = false;
    int   depth = 0;

    bool load(const uint8_t* data, size_t len);
    ObjP resolve(const ObjP& o);
    ObjP obj(int num);
    const std::string& warn(const std::string& w) {
        if (warnings.size() < 40) warnings.push_back(w);
        return w;
    }
    int  pageCount() const { return (int)pages.size(); }
    void pageSize(int idx, double& w, double& h, int& rotate);

    // ── stream decoding ──────────────────────────────────────────────────
    bool decodeStream(const ObjP& stm, std::string& out);

    // ── fonts ────────────────────────────────────────────────────────────
    Font* font(const ObjP& fontDict);

    // internals
    struct Lex;
    ObjP parseObjectAt(long off, bool* ok);
    ObjP objFromObjectStream(int stmNum, int wantNum);
    bool  parseXref(long off, int depth);
    void  reconstructXref();
    std::map<std::string, std::shared_ptr<Font>> fontCache;
};

std::string objName(const ObjP& o);
double      objNum(const ObjP& o, double def);

// ── content stream tokens ──────────────────────────────────────────────────
enum TokType { T_EOF, T_NUM, T_STR, T_NAME, T_ARR_OPEN, T_ARR_CLOSE,
               T_DICT_OPEN, T_DICT_CLOSE, T_OP };

struct Tok {
    TokType type = T_EOF;
    double  num = 0;
    std::string s;     // string bytes, name (no '/'), or operator text
};

struct ContentLexer {
    const char* p = nullptr;
    const char* end = nullptr;
    ContentLexer(const char* b, size_t n) : p(b), end(b + n) {}
    void next(Tok& t);
};

// ── helpers shared with the rasterizer ─────────────────────────────────────
std::string flateInflate(const std::string& in, size_t hint, bool* ok);

} // namespace pdf
