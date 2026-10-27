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
#include <set>
#include <string>
#include <vector>

#include "cmap.hpp"
#include "pdfsource.hpp"

#include "cfffont.hpp"

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
    int objNum = 0, genNum = 0;            // own object number when parsed indirect
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
    std::string embedded;                  // /FontFile2 TrueType or /FontFile3 OpenType bytes
    std::shared_ptr<CffFont> cff;          // /FontFile3 Type1C or CIDFontType0C
    bool isCid = false;                    // Type0 (2-byte codes, CID space)
    // Type3: glyphs are content streams in /CharProcs, mapped by /FontMatrix.
    bool type3 = false;
    ObjP charProcs, res3;
    double fm[6] = {0.001, 0, 0, 0.001, 0, 0};
    // /Encoding CMap for a Type0 font when it is not /Identity-H: predefined
    // (cmapFind) or parsed from an embedded stream. Codes have their own byte
    // width and must go through it before any CID -> GID step.
    std::shared_ptr<const CMap> cmap;
    std::map<int, int> codeToGid;          // simple fonts: byte code -> glyph id
    std::map<int, int> cidToGid;           // Type0: /CIDToGIDMap stream (CID -> GID)

    int  unicodeFor(int code) const;       // 0 when unmapped
    double widthFor(int code) const;       // text space (1 em == 1000)
    bool   widthKnown(int code) const;     // /W or /Widths actually covers it
    // Substitute face selectors derived from the BaseFont name.
    int  faceClass() const;                // 0 sans, 1 serif, 2 mono
    bool faceBold() const;
    bool faceItalic() const;
};

struct Document {
    // ── byte access (M9.2) ───────────────────────────────────────────────
    // The file is read through a ByteSource. When the source is wholly
    // resident (the in-memory path every other test uses) `direct()` gives one
    // pointer to all of it and nothing is ever copied. Otherwise the parser
    // works inside `win`, a window that slides as it walks forward, and only
    // `windowBytes` of the file is in wasm memory at a time.
    std::unique_ptr<ByteSource> src;
    std::vector<uint8_t> win;
    uint64_t winOff = 0;
    size_t windowBytes = 16u << 10;     // current readahead; grows on sequential access

    uint64_t fileSize() const { return src ? src->size() : 0; }
    /// Whole-file pointer when the source is resident, else nullptr.
    const uint8_t* directBase() const { return src ? src->direct() : nullptr; }
    const uint8_t* winBase() const { return (const uint8_t*)win.data(); }
    /// Make [off, off+need) contiguous and resident. False when past EOF.
    bool cover(uint64_t off, size_t need);
    /// One byte at an absolute offset (fetches a window if needed), or -1.
    int byteAt(uint64_t off);
    /// Copy [off, off+len) out of the source.
    std::string readBytes(uint64_t off, size_t len);
    const uint8_t* residentBase() const;
    uint64_t residentOff() const;
    size_t residentSize() const;
    /// Offset of `p` inside the resident map.
    uint64_t absOff(const uint8_t* p) const;
    /// First occurrence of `needle` at or after `from`, or std::string::npos.
    /// Streams past the file in chunks, so it works on a windowed source.
    /// `maxBytes` caps the scan (0 = whole file): a ranged scan is one
    /// synchronous request per 64 KB, so an unbounded miss on a 4 GiB file is
    /// 65 000 round trips, which reads as a hang rather than an error.
    uint64_t findForward(uint64_t from, const char* needle, size_t nl, uint64_t maxBytes = 0);
    bool loadSource(std::unique_ptr<ByteSource> s);

    std::map<int, int> objNumToStm;        // object streams: num -> container obj num
    std::map<int, int64_t> xref;           // num -> byte offset (>=0) or -1 (in object stream)
    std::map<int, ObjP> cache;
    ObjP trailer;
    std::vector<ObjP> pages;               // page dicts, in document order
    std::vector<std::string> warnings;
    std::string error;
    bool  loaded = false;
    bool  windowForced = false;         // tests: force sliding even when resident

    // Password to try against the Standard security handler (user, else owner).
    std::string password;

    // Standard security handler: RC4 40/128, AES-128, AES-256 (R5/R6).
    bool  encrypted = false;
    bool  aesStreams = false;
    std::string encKey;                    // file key
    int   encN = 0;                        // key length in bytes
    bool  encDirectKey = false;            // V5: per-object key is the file key
    std::string decryptStream(const ObjP& stm) const;
    int   depth = 0;

    bool load(const uint8_t* data, size_t len);
    bool loadWindowed(const uint8_t* data, size_t len, size_t windowBytes);
    bool setupEncryption(const ObjP& enc);
    ObjP resolve(const ObjP& o);
    ObjP obj(int num);
    const std::string& warn(const std::string& w) {
        if (warnings.size() < 40) warnings.push_back(w);
        return w;
    }
    int  pageCount() const { return (int)pages.size(); }
    void pageSize(int idx, double& w, double& h, int& rotate);

    // ── link annotations ─────────────────────────────────────────────────
    struct Link {
        double x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // page space, y up
        std::string uri;                          // /URI action
        int page = -1;                            // /GoTo target, -1 when none
    };
    const std::vector<Link>& links(int idx);
    int pageIndex(const ObjP& pageRef);            // ref -> index, -1 if unknown

    // ── stream decoding ──────────────────────────────────────────────────
    // Optional content (layers): /OCProperties decides which OCGs are visible
    // in the default configuration. Rendering consults this for /OC marked
    // content and for XObjects that carry an /OC entry.
    bool ocgVisible(const ObjP& ocg);
    void loadOcg();
    std::map<const Obj*, bool> ocgState;
    bool ocgLoaded = false;
    // Document outline (bookmarks) as JSON, resolved against the page list.
    std::string outlineJson();

    // AcroForm: terminal fields with their widget rectangle and page, plus a
    // setter that updates /V in place so the next render shows the new value.
    std::string formJson();
    bool setFormField(const std::string& name, const std::string& value);

    bool decodeStream(const ObjP& stm, std::string& out);
    bool hasFilter(const ObjP& stm, const char* name);
    // JPEG 2000 images carry their own geometry and component count, so they
    // bypass decodeStream (which cannot report dims back to the caller).
    // JBIG2: decodes to 1 bpc, rows packed MSB-first (see cpp/jbig2native.cpp).
    bool decodeJbig2(const ObjP& stm, std::vector<uint8_t>& pixels, int& w, int& h);

    bool decodeJpx(const ObjP& stm, std::vector<uint8_t>& pixels,
                   int& w, int& h, int& comps);

    // ── externally decoded images (JBIG2 comes from the JS decoder) ──────
    // 1 bpc DeviceGray bits, one bit per pixel, 1 = white, rows padded to a byte.

    // ── fonts ────────────────────────────────────────────────────────────
    Font* font(const ObjP& fontDict);

    // internals
    struct Lex;
    ObjP parseObjectAt(int64_t off, bool* ok);
    ObjP objFromObjectStream(int stmNum, int wantNum);
    int64_t scanObjectOffset(int num);
    bool  parseXref(int64_t off, int depth);
    void  reconstructXref();
    std::map<std::string, std::shared_ptr<Font>> fontCache;
    std::map<int, std::vector<Link>> linkCache;
    std::map<int, int> pageIndexByObj;
    ObjP resolveDest(const ObjP& dest, int depth);
    ObjP lookupNamedDest(const std::string& name);
    std::map<int, std::string> stmCache;   // decoded object streams, by obj num
    size_t stmCacheBytes = 0;
    std::set<int> xrefFree;                // objects deleted by a later update
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
