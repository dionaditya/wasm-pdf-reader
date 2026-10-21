#include "pdfdoc.hpp"
#include "pdf_widths.h"   // standard-14 AFM advances for fonts without /Widths

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <zlib.h>

namespace pdf {

// ─── small object constructors ───────────────────────────────────────────────
ObjP mkNull()            { static ObjP n = std::make_shared<Obj>(); return n; }
ObjP mkInt(long long v)  { auto o = std::make_shared<Obj>(); o->t = O_INT;  o->num = (double)v; return o; }
ObjP mkReal(double v)    { auto o = std::make_shared<Obj>(); o->t = O_REAL; o->num = v; return o; }
ObjP mkName(const std::string& n) { auto o = std::make_shared<Obj>(); o->t = O_NAME; o->str = n; return o; }
ObjP mkStr(const std::string& s)  { auto o = std::make_shared<Obj>(); o->t = O_STR;  o->str = s; return o; }
ObjP mkArr()             { auto o = std::make_shared<Obj>(); o->t = O_ARR;  return o; }
ObjP mkDict()            { auto o = std::make_shared<Obj>(); o->t = O_DICT; return o; }

ObjP Obj::get(const char* k) const {
    for (auto& kv : dict) if (kv.first == k) return kv.second;
    return nullptr;
}
bool Obj::has(const char* k) const {
    for (auto& kv : dict) if (kv.first == k) return true;
    return false;
}

static bool isWS(int c) {
    return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32;
}
static bool isDelim(int c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' ||
           c == '{' || c == '}' || c == '/' || c == '%';
}
static bool isRegular(int c) { return !isWS(c) && !isDelim(c); }

// ─── lexer / parser ─────────────────────────────────────────────────────────
struct Document::Lex {
    Document* doc;
    const char* p;
    const char* end;

    Lex(Document* d, const char* b, const char* e) : doc(d), p(b), end(e) {}

    void skipWS() {
        while (p < end) {
            if (isWS((unsigned char)*p)) { ++p; continue; }
            if (*p == '%') { while (p < end && *p != '\n' && *p != '\r') ++p; continue; }
            break;
        }
    }
    bool eof() { skipWS(); return p >= end; }
    bool wordIs(const char* w) {
        size_t n = strlen(w);
        if ((size_t)(end - p) < n) return false;
        if (memcmp(p, w, n) != 0) return false;
        if (p + n < end && isRegular((unsigned char)p[n])) return false;
        return true;
    }
    bool eatWord(const char* w) {
        skipWS();
        if (!wordIs(w)) return false;
        p += strlen(w);
        return true;
    }

    std::string readName() {
        ++p;  // '/'
        std::string s;
        while (p < end && isRegular((unsigned char)*p)) {
            if (*p == '#') {
                auto hex = [&](int c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                if (p + 2 < end) {
                    int h = hex((unsigned char)p[1]), l = hex((unsigned char)p[2]);
                    if (h >= 0 && l >= 0) { s += (char)(h * 16 + l); p += 3; continue; }
                }
            }
            s += *p++;
        }
        return s;
    }

    std::string readLiteralString() {
        ++p;  // '('
        std::string s;
        int depth = 1;
        while (p < end) {
            char c = *p++;
            if (c == '(') { ++depth; s += c; continue; }
            if (c == ')') { if (--depth == 0) break; s += c; continue; }
            if (c == '\\') {
                if (p >= end) break;
                char e = *p++;
                switch (e) {
                    case 'n': s += '\n'; break;
                    case 'r': s += '\r'; break;
                    case 't': s += '\t'; break;
                    case 'b': s += '\b'; break;
                    case 'f': s += '\f'; break;
                    case '(': case ')': case '\\': s += e; break;
                    case '\r': if (p < end && *p == '\n') ++p; break;
                    case '\n': break;
                    default:
                        if (e >= '0' && e <= '7') {
                            int v = e - '0', n = 1;
                            while (n < 3 && p < end && *p >= '0' && *p <= '7') { v = v * 8 + (*p - '0'); ++p; ++n; }
                            s += (char)(v & 0xFF);
                        } else s += e;
                }
                continue;
            }
            s += c;
        }
        return s;
    }

    std::string readHexString() {
        ++p;  // '<'
        std::string s;
        int hi = -1;
        auto hex = [](int c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        while (p < end && *p != '>') {
            int v = hex((unsigned char)*p++);
            if (v < 0) continue;
            if (hi < 0) hi = v;
            else { s += (char)(hi * 16 + v); hi = -1; }
        }
        if (hi >= 0) s += (char)(hi * 16);
        if (p < end) ++p;
        return s;
    }

    bool readNumber(double& out, bool& isInt) {
        skipWS();
        const char* s = p;
        if (p < end && (*p == '+' || *p == '-')) ++p;
        bool any = false, dot = false;
        while (p < end) {
            if (*p >= '0' && *p <= '9') { any = true; ++p; }
            else if (*p == '.' && !dot) { dot = true; ++p; }
            else break;
        }
        if (!any) { p = s; return false; }
        std::string tmp(s, p);
        out = atof(tmp.c_str());
        isInt = !dot;
        return true;
    }

    ObjP parse(int depthGuard = 0) {
        if (depthGuard > 60) return mkNull();
        skipWS();
        if (p >= end) return mkNull();
        char c = *p;

        if (c == '/') return mkName(readName());
        if (c == '(') return mkStr(readLiteralString());
        if (c == '[') {
            ++p;
            auto a = mkArr();
            while (true) {
                skipWS();
                if (p >= end) break;
                if (*p == ']') { ++p; break; }
                size_t before = (size_t)(p - end);
                a->arr.push_back(parse(depthGuard + 1));
                if ((size_t)(p - end) == before) { ++p; }  // no progress: skip a byte
            }
            return a;
        }
        if (c == '<') {
            if (p + 1 < end && p[1] == '<') {
                p += 2;
                auto d = mkDict();
                while (true) {
                    skipWS();
                    if (p >= end) break;
                    if (p + 1 < end && p[0] == '>' && p[1] == '>') { p += 2; break; }
                    if (*p != '/') {  // malformed: skip token
                        double dv; bool ii;
                        if (readNumber(dv, ii)) continue;
                        if (*p == '(') { readLiteralString(); continue; }
                        ++p; continue;
                    }
                    std::string key = readName();
                    ObjP val = parse(depthGuard + 1);
                    d->dict.push_back({key, val});
                }
                return d;
            }
            return mkStr(readHexString());
        }
        if (c == ')' || c == ']' || c == '>') { ++p; return mkNull(); }

        if (wordIs("true"))  { p += 4; auto o = std::make_shared<Obj>(); o->t = O_BOOL; o->b = true;  return o; }
        if (wordIs("false")) { p += 5; auto o = std::make_shared<Obj>(); o->t = O_BOOL; o->b = false; return o; }
        if (wordIs("null"))  { p += 4; return mkNull(); }

        double v; bool isInt;
        const char* save = p;
        if (readNumber(v, isInt)) {
            // possible indirect reference: "<int> <int> R"
            if (isInt && v >= 0) {
                const char* after = p;
                double g; bool gInt;
                if (readNumber(g, gInt) && gInt && g >= 0) {
                    skipWS();
                    if (p < end && *p == 'R' && (p + 1 >= end || !isRegular((unsigned char)p[1]))) {
                        ++p;
                        auto o = std::make_shared<Obj>();
                        o->t = O_REF;
                        o->ref = (int)v;
                        o->gen = (int)g;
                        return o;
                    }
                }
                p = after;
            }
            return isInt ? mkInt((long long)v) : mkReal(v);
        }
        p = save;
        // bare keyword (operator) — consume it
        std::string kw;
        while (p < end && isRegular((unsigned char)*p)) kw += *p++;
        if (kw.empty()) { ++p; return mkNull(); }
        return mkName(kw);
    }
};

// ─── load ───────────────────────────────────────────────────────────────────
bool Document::load(const uint8_t* data, size_t len) {
    loaded = false;
    error.clear();
    warnings.clear();
    cache.clear();
    xref.clear();
    xrefFree.clear();
    pages.clear();
    fontCache.clear();
    objNumToStm.clear();
    stmCache.clear();
    stmCacheBytes = 0;
    buf.assign((const char*)data, len);
    if (buf.size() < 8) { error = "file too small"; return false; }

    size_t hdr = buf.find("%PDF-");
    if (hdr == std::string::npos) { error = "no %PDF header"; return false; }

    long startxref = -1;
    size_t sx = buf.rfind("startxref");
    if (sx != std::string::npos) {
        size_t q = sx + 9;
        while (q < buf.size() && isWS((unsigned char)buf[q])) ++q;
        startxref = atol(buf.c_str() + q);
    }
    bool okXref = false;
    if (startxref > 0 && startxref < (long)buf.size()) okXref = parseXref(startxref, 0);
    if (!okXref || !trailer || !trailer->get("Root")) {
        warn("xref damaged, reconstructing by scanning");
        reconstructXref();
    }
    if (!trailer) { error = "no trailer"; return false; }

    ObjP root = resolve(trailer->get("Root"));
    if (!root) { error = "no document catalog"; return false; }
    ObjP pagesRoot = resolve(root->get("Pages"));
    if (!pagesRoot) { error = "no page tree"; return false; }

    // walk the page tree, inheriting the attributes that matter
    struct Walker {
        Document* d;
        void walk(ObjP node, ObjP inhRes, ObjP inhBox, ObjP inhRot, int depth) {
            if (!node || depth > 40 || d->pages.size() > 5000) return;
            node = d->resolve(node);
            if (!node) return;
            std::string type = node->get("Type") ? objName(node->get("Type")) : "";
            ObjP res = node->get("Resources") ? d->resolve(node->get("Resources")) : inhRes;
            ObjP box = node->get("MediaBox")  ? node->get("MediaBox")  : inhBox;
            ObjP rot = node->get("Rotate")    ? d->resolve(node->get("Rotate")) : inhRot;
            ObjP kids = node->get("Kids");
            if (type == "Pages" || (kids && !node->get("Contents"))) {
                if (kids) {
                    kids = d->resolve(kids);
                    if (kids) for (size_t i = 0; i < kids->arr.size(); i++)
                        walk(kids->arr[i], res, box, rot, depth + 1);
                }
                return;
            }
            // a page: materialise inherited attributes into the page dict so the
            // renderer does not have to know about inheritance at all
            auto p = std::make_shared<Obj>(*node);
            auto setIfMissing = [&](const char* k, ObjP v) {
                if (!p->get(k) && v) p->dict.push_back({k, v});
            };
            setIfMissing("Resources", res);
            setIfMissing("MediaBox", box);
            setIfMissing("Rotate", rot);
            d->pages.push_back(p);
        }
    } w{this};
    w.walk(pagesRoot, nullptr, nullptr, nullptr, 0);
    if (pages.empty()) { error = "page tree is empty"; return false; }
    loaded = true;
    return true;
}

std::string objName(const ObjP& o) {
    if (!o) return "";
    return o->t == O_NAME ? o->str : (o->t == O_STR ? o->str : "");
}
double objNum(const ObjP& o, double def) {
    if (!o) return def;
    if (o->t == O_INT || o->t == O_REAL) return o->num;
    return def;
}

ObjP Document::resolve(const ObjP& o) {
    if (!o) return nullptr;
    if (o->t != O_REF) return o;
    return obj(o->ref);
}

ObjP Document::obj(int num) {
    auto it = cache.find(num);
    if (it != cache.end()) return it->second;
    cache[num] = mkNull();  // recursion guard
    long off = -1;
    auto xit = xref.find(num);
    if (xit != xref.end()) off = xit->second;
    if (xrefFree.count(num)) { cache[num] = mkNull(); return cache[num]; }  // deleted

    ObjP res;
    if (off >= 0) {
        bool ok = false;
        res = parseObjectAt(off, &ok);
        if (!ok) res = nullptr;
    } else if (off < 0) {
        // lives in an object stream
        auto sit = objNumToStm.find(num);
        if (sit != objNumToStm.end()) res = objFromObjectStream(sit->second, num);
    }
    if (!res || res->t == O_NULL) {
        // last resort: scan for "<num> <gen> obj" in the raw bytes
        long p = scanObjectOffset(num);
        if (p >= 0) {
            bool ok = false;
            ObjP scanned = parseObjectAt(p, &ok);
            if (ok && scanned && scanned->t != O_NULL) res = scanned;
        }
    }
    if (!res) res = mkNull();
    cache[num] = res;
    return res;
}

ObjP Document::objFromObjectStream(int stmNum, int wantNum) {
    ObjP stm = obj(stmNum);
    if (!stm || !stm->isStream) return nullptr;
    std::string data;
    const std::string* cached = nullptr;
    auto cit = stmCache.find(stmNum);
    if (cit != stmCache.end()) {
        cached = &cit->second;
    } else {
        if (!decodeStream(stm, data)) return nullptr;
    }
    const std::string& objData = cached ? *cached : data;

    int n = (int)objNum(resolve(stm->get("N")), 0);
    // Object data starts at /First; the (num, offset) pairs in the header are
    // relative to it. calibre, Acrobat and Ghostscript all write a non-zero
    // /First, so treating the offsets as absolute parses the header itself as
    // object data and every compressed object comes back as garbage.
    long first = (long)objNum(resolve(stm->get("First")), 0);
    if (first < 0 || (size_t)first > objData.size()) first = 0;

    std::vector<std::pair<int, long>> offs;
    Lex lex(this, objData.data(), objData.data() + objData.size());
    for (int i = 0; i < n; i++) {
        double a, b; bool ia, ib;
        if (!lex.readNumber(a, ia)) break;
        if (!lex.readNumber(b, ib)) break;
        offs.push_back({(int)a, (long)b});
    }
    if (!cached) {
        // remember the decoded stream: a book can hold thousands of objects in
        // a handful of streams, and inflating them per object is the hot path
        if (stmCacheBytes + data.size() < 32u * 1024u * 1024u) {
            stmCache[stmNum] = data;
            stmCacheBytes += data.size();
        }
    }
    for (auto& o : offs) {
        if (o.first != wantNum) continue;
        // spec position first, then the absolute offset as a fallback for
        // writers that stored offsets relative to the stream start
        long pos = first + o.second;
        if (pos >= 0 && (size_t)pos < objData.size()) {
            Lex l2(this, objData.data() + pos, objData.data() + objData.size());
            ObjP r = l2.parse();
            if (r && r->t != O_NULL) return r;
        }
        if (o.second >= 0 && (size_t)o.second < objData.size()) {
            Lex l3(this, objData.data() + o.second, objData.data() + objData.size());
            ObjP r = l3.parse();
            if (r && r->t != O_NULL) return r;
        }
        return nullptr;
    }
    return nullptr;
}

// Last resort for objects the xref does not cover: find "<num> <gen> obj" in
// the raw bytes. The number must start a token, otherwise object 37 matches
// inside "1337 0 obj"; the last match wins because incremental updates append.
long Document::scanObjectOffset(int num) const {
    const std::string pat = std::to_string(num);
    long found = -1;
    size_t p = 0;
    while ((p = buf.find(pat, p)) != std::string::npos) {
        bool leftOk = p == 0 || !(isdigit((unsigned char)buf[p - 1]) || buf[p - 1] == '.');
        size_t q = p + pat.size();
        if (leftOk && q < buf.size() && isWS((unsigned char)buf[q])) {
            while (q < buf.size() && isWS((unsigned char)buf[q])) ++q;
            size_t gs = q;
            while (q < buf.size() && isdigit((unsigned char)buf[q])) ++q;
            if (q > gs) {
                while (q < buf.size() && isWS((unsigned char)buf[q])) ++q;
                if (q + 3 <= buf.size() && buf.compare(q, 3, "obj") == 0 &&
                    (q + 3 >= buf.size() || !isRegular((unsigned char)buf[q + 3])))
                    found = (long)p;
            }
        }
        p += pat.size();
    }
    return found;
}

ObjP Document::parseObjectAt(long off, bool* ok) {
    *ok = false;
    if (off < 0 || (size_t)off >= buf.size()) return nullptr;
    Lex lex(this, buf.data() + off, buf.data() + buf.size());
    double n, g; bool ia, ib;
    if (!lex.readNumber(n, ia) || !lex.readNumber(g, ib)) return nullptr;
    if (!lex.eatWord("obj")) return nullptr;
    ObjP o = lex.parse();
    lex.skipWS();
    if (o->t == O_DICT && lex.wordIs("stream")) {
        lex.p += 6;
        // stream data starts after CRLF or LF (not CR alone)
        if (lex.p < lex.end && *lex.p == '\r') ++lex.p;
        if (lex.p < lex.end && *lex.p == '\n') ++lex.p;
        const char* start = lex.p;
        long len = -1;
        ObjP lenObj = o->get("Length");
        if (lenObj) {
            if (lenObj->t == O_REF) lenObj = obj(lenObj->ref);
            if (lenObj && (lenObj->t == O_INT || lenObj->t == O_REAL)) len = (long)lenObj->num;
        }
        long avail = (long)(lex.end - start);
        bool valid = len >= 0 && len <= avail;
        if (valid) {
            // sanity: 'endstream' must follow the declared length (allow EOL)
            const char* chk = start + len;
            const char* lim = chk + 12 < lex.end ? chk + 12 : lex.end;
            bool found = false;
            for (const char* q = chk; q < lim; q++)
                if (memcmp(q, "endstream", 9) == 0) { found = true; break; }
            if (!found) valid = false;
        }
        if (!valid) {
            // hunt for endstream
            const char* q = start;
            while (q + 9 <= lex.end && memcmp(q, "endstream", 9) != 0) ++q;
            len = (long)(q - start);
            if (q + 9 <= lex.end) len -= 0;
            if (len < 0) len = 0;
            o->isStream = true;
            o->raw.assign(start, (size_t)len);
        } else {
            o->isStream = true;
            o->raw.assign(start, (size_t)len);
        }
    }
    *ok = true;
    return o;
}

bool Document::parseXref(long off, int depth) {
    if (depth > 40 || off <= 0 || (size_t)off >= buf.size()) return false;
    Lex lex(this, buf.data() + off, buf.data() + buf.size());
    lex.skipWS();
    bool gotTrailer = false;
    ObjP trailerDict;
    if (lex.wordIs("xref")) {
        lex.p += 4;
        while (true) {
            lex.skipWS();
            if (lex.p >= lex.end) break;
            if (lex.wordIs("trailer")) { lex.p += 7; break; }
            double start, count; bool ia, ib;
            if (!lex.readNumber(start, ia) || !lex.readNumber(count, ib)) break;
            for (int i = 0; i < (int)count; i++) {
                double o, g; bool oa, ga;
                if (!lex.readNumber(o, oa) || !lex.readNumber(g, ga)) break;
                lex.skipWS();
                if (lex.p >= lex.end) break;
                char ty = *lex.p++;
                int num = (int)start + i;
                if (ty == 'n' && !xref.count(num)) xref[num] = (long)o;
            }
        }
        lex.skipWS();
        if (lex.p < lex.end && *lex.p == '<') trailerDict = lex.parse();
    } else {
        // cross-reference stream
        bool ok = false;
        ObjP stm = parseObjectAt(off, &ok);
        if (!ok || !stm || !stm->isStream) return false;
        ObjP type = resolve(stm->get("Type"));
        if (objName(type) != "XRef") {
            // maybe the offset points at "N G obj" of a normal object; bail
            return false;
        }
        std::string data;
        if (!decodeStream(stm, data)) return false;
        ObjP wObj = resolve(stm->get("W"));
        if (!wObj || wObj->arr.size() < 3) return false;
        int w[3];
        for (int i = 0; i < 3; i++) w[i] = (int)objNum(resolve(wObj->arr[i]), 0);
        ObjP idxObj = resolve(stm->get("Index"));
        std::vector<std::pair<int, int>> ranges;
        if (idxObj && idxObj->arr.size() >= 2) {
            for (size_t i = 0; i + 1 < idxObj->arr.size(); i += 2)
                ranges.push_back({(int)objNum(resolve(idxObj->arr[i]), 0),
                                  (int)objNum(resolve(idxObj->arr[i + 1]), 0)});
        } else {
            ranges.push_back({0, (int)objNum(resolve(stm->get("Size")), 0)});
        }
        size_t pos = 0;
        int rowLen = w[0] + w[1] + w[2];
        for (auto& rg : ranges) {
            for (int i = 0; i < rg.second; i++) {
                if (pos + rowLen > data.size()) break;
                auto field = [&](int k) -> long {
                    long v = 0;
                    for (int j = 0; j < w[k]; j++) v = (v << 8) | (unsigned char)data[pos++];
                    return v;
                };
                long f0 = field(0), f1 = field(1), f2 = field(2);
                int num = rg.first + i;
                if (w[0] == 0) f0 = 1;   // default type 1 when the type field is absent
                if (num == 0) continue;
                if (xref.count(num)) continue;
                if (f0 == 1) xref[num] = f1;
                else if (f0 == 2) { xref[num] = -1; objNumToStm[num] = (int)f1; }
                // type 0 = free, ignore
            }
        }
        trailerDict = stm;
        gotTrailer = true;
        (void)gotTrailer;
    }
    if (trailerDict) {
        if (!trailer) trailer = trailerDict;
        else {
            for (auto& kv : trailerDict->dict)
                if (!trailer->get(kv.first.c_str())) trailer->dict.push_back(kv);
        }
    }
    ObjP prev = trailerDict ? trailerDict->get("Prev") : nullptr;
    if (prev) {
        ObjP pv = resolve(prev);
        long poff = (long)objNum(pv, -1);
        if (poff > 0) parseXref(poff, depth + 1);
    }
    return trailer != nullptr;
}

void Document::reconstructXref() {
    xref.clear();
    objNumToStm.clear();
    const char* b = buf.data();
    const char* e = buf.data() + buf.size();
    const char* p = b;
    while (p < e) {
        // find digits followed by digits followed by "obj"
        while (p < e && !(*p >= '0' && *p <= '9')) ++p;
        const char* numStart = p;
        while (p < e && *p >= '0' && *p <= '9') ++p;
        if (p >= e) break;
        if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') continue;
        const char* q = p;
        while (q < e && isWS((unsigned char)*q)) ++q;
        const char* genStart = q;
        while (q < e && *q >= '0' && *q <= '9') ++q;
        if (q == genStart) continue;
        const char* r = q;
        while (r < e && isWS((unsigned char)*r)) ++r;
        if (r + 3 <= e && memcmp(r, "obj", 3) == 0 && (r + 3 >= e || !isRegular((unsigned char)r[3]))) {
            int num = atoi(std::string(numStart, p - numStart).c_str());
            xref[num] = (long)(numStart - b);   // later (newer) definitions win
            p = r + 3;
        }
    }
    // trailer: prefer a real one, else find the catalog
    size_t t = buf.rfind("trailer");
    if (t != std::string::npos) {
        Lex lex(this, buf.data() + t + 7, buf.data() + buf.size());
        ObjP d = lex.parse();
        if (d && d->t == O_DICT) trailer = d;
    }
    if (!trailer) trailer = mkDict();
    if (!trailer->get("Root")) {
        for (auto& kv : xref) {
            ObjP o = obj(kv.first);
            if (!o || o->t != O_DICT) continue;
            ObjP type = o->get("Type");
            if (type && objName(type) == "Catalog") {
                trailer->dict.push_back({"Root", mkInt(kv.first)});
                break;
            }
        }
    }
    if (!trailer->get("Size")) {
        int mx = 0;
        for (auto& kv : xref) mx = std::max(mx, kv.first);
        trailer->dict.push_back({"Size", mkInt(mx + 1)});
    }
}

// ─── filters ────────────────────────────────────────────────────────────────
std::string flateInflate(const std::string& in, size_t hint, bool* ok) {
    *ok = false;
    std::string out;
    out.resize(std::max<size_t>(hint, in.size() * 4 + 1024));
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    // 15 + 32: auto-detect zlib/gzip header
    if (inflateInit2(&zs, 15 + 32) != Z_OK) return out;
    zs.next_in = (Bytef*)in.data();
    zs.avail_in = (uInt)in.size();
    size_t produced = 0;
    int rc = Z_OK;
    do {
        if (zs.total_out >= out.size()) out.resize(out.size() * 2);
        zs.next_out = (Bytef*)(&out[0] + zs.total_out);
        zs.avail_out = (uInt)(out.size() - zs.total_out);
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc == Z_STREAM_END) break;
        if (rc == Z_DATA_ERROR || rc == Z_MEM_ERROR || rc == Z_NEED_DICT) break;
        if (rc == Z_BUF_ERROR && zs.total_out < out.size()) break;
        if (zs.avail_in == 0 && zs.avail_out != 0) break;
    } while (zs.total_out < (64u << 20));
    produced = zs.total_out;
    inflateEnd(&zs);
    out.resize(produced);
    *ok = produced > 0 || in.empty();
    return out;
}

static std::string asciiHexDecode(const std::string& in) {
    std::string out;
    int hi = -1;
    auto hex = [](int c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (char ch : in) {
        if (ch == '>') break;
        int v = hex((unsigned char)ch);
        if (v < 0) continue;
        if (hi < 0) hi = v;
        else { out += (char)(hi * 16 + v); hi = -1; }
    }
    if (hi >= 0) out += (char)(hi * 16);
    return out;
}

static std::string ascii85Decode(const std::string& in) {
    std::string out;
    std::vector<int> grp;
    size_t i = 0;
    if (in.size() >= 2 && in[0] == '<' && in[1] == '~') i = 2;
    for (; i < in.size(); i++) {
        char c = in[i];
        if (c == '~') break;
        if (isWS((unsigned char)c)) continue;
        if (c == 'z' && grp.empty()) { out.append(4, '\0'); continue; }
        if (c < '!' || c > 'u') continue;
        grp.push_back(c - '!');
        if (grp.size() == 5) {
            unsigned long v = 0;
            for (int k = 0; k < 5; k++) v = v * 85 + grp[k];
            for (int k = 3; k >= 0; k--) out += (char)((v >> (k * 8)) & 0xFF);
            grp.clear();
        }
    }
    if (!grp.empty()) {
        size_t n = grp.size();
        while (grp.size() < 5) grp.push_back(84);
        unsigned long v = 0;
        for (int k = 0; k < 5; k++) v = v * 85 + grp[k];
        for (int k = 3; k >= (int)(4 - (n - 1)); k--) out += (char)((v >> (k * 8)) & 0xFF);
    }
    return out;
}

static std::string runLengthDecode(const std::string& in) {
    std::string out;
    size_t i = 0;
    while (i < in.size()) {
        int len = (unsigned char)in[i++];
        if (len == 128) break;
        if (len < 128) {
            for (int k = 0; k <= len && i < in.size(); k++) out += in[i++];
        } else {
            if (i >= in.size()) break;
            char c = in[i++];
            for (int k = 0; k < 257 - len; k++) out += c;
        }
    }
    return out;
}

static std::string lzwDecode(const std::string& in, int earlyChange) {
    std::string out;
    std::vector<std::string> table;
    auto reset = [&]() {
        table.clear();
        for (int i = 0; i < 256; i++) table.push_back(std::string(1, (char)i));
        table.push_back("");   // 256 clear
        table.push_back("");   // 257 eod
    };
    reset();
    int codeLen = 9;
    unsigned long bitbuf = 0;
    int bitcnt = 0;
    std::string prev;
    size_t i = 0;
    while (i < in.size() || bitcnt >= codeLen) {
        while (bitcnt < codeLen && i < in.size()) {
            bitbuf = (bitbuf << 8) | (unsigned char)in[i++];
            bitcnt += 8;
        }
        if (bitcnt < codeLen) break;
        int code = (int)((bitbuf >> (bitcnt - codeLen)) & ((1u << codeLen) - 1));
        bitcnt -= codeLen;
        if (code == 256) { reset(); codeLen = 9; prev.clear(); continue; }
        if (code == 257) break;
        std::string entry;
        if (code < (int)table.size() && !table[code].empty()) entry = table[code];
        else if (!prev.empty()) entry = prev + prev[0];
        else break;
        out += entry;
        if (!prev.empty() && table.size() < 4096) table.push_back(prev + entry[0]);
        prev = entry;
        int next = (int)table.size() + earlyChange;
        codeLen = next >= 2048 ? 12 : next >= 1024 ? 11 : next >= 512 ? 10 : 9;
        if (codeLen > 12) codeLen = 12;
    }
    return out;
}

static std::string applyPredictor(const std::string& in, int predictor, int colors,
                                  int bpc, int columns) {
    if (predictor <= 1 || columns <= 0) return in;
    int bpp = std::max(1, (colors * bpc + 7) / 8);
    int rowLen = std::max(1, (colors * bpc * columns + 7) / 8);
    std::string out;
    if (predictor == 2) {   // TIFF
        out = in;
        for (size_t rowStart = 0; rowStart + rowLen <= out.size(); rowStart += rowLen)
            for (int i = bpp; i < rowLen; i++)
                out[rowStart + i] = (char)((unsigned char)out[rowStart + i] +
                                           (unsigned char)out[rowStart + i - bpp]);
        return out;
    }
    // PNG predictors 10..15
    size_t rows = in.size() / (rowLen + 1);
    std::vector<unsigned char> prior(rowLen, 0), cur(rowLen, 0);
    out.reserve(rows * rowLen);
    for (size_t r = 0; r < rows; r++) {
        unsigned char ft = (unsigned char)in[r * (rowLen + 1)];
        memcpy(cur.data(), in.data() + r * (rowLen + 1) + 1, rowLen);
        for (int i = 0; i < rowLen; i++) {
            int a = i >= bpp ? cur[i - bpp] : 0;
            int b = prior[i];
            int c = i >= bpp ? prior[i - bpp] : 0;
            int v = cur[i];
            switch (ft) {
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) / 2; break;
                case 4: {
                    int p = a + b - c;
                    int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
                    v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                    break;
                }
                default: break;
            }
            cur[i] = (unsigned char)(v & 0xFF);
        }
        out.append((const char*)cur.data(), rowLen);
        prior = cur;
    }
    return out;
}

bool Document::decodeStream(const ObjP& stm, std::string& out) {
    if (!stm || !stm->isStream) return false;
    out = stm->raw;

    std::vector<std::string> filters;
    ObjP f = stm->get("Filter");
    if (f) {
        f = resolve(f);
        if (f && f->t == O_NAME) filters.push_back(f->str);
        else if (f && f->t == O_ARR)
            for (auto& e : f->arr) { ObjP r = resolve(e); if (r && r->t == O_NAME) filters.push_back(r->str); }
    }
    std::vector<ObjP> parms;
    ObjP dp = stm->get("DecodeParms");
    if (!dp) dp = stm->get("DP");
    if (dp) {
        dp = resolve(dp);
        if (dp && dp->t == O_ARR) for (auto& e : dp->arr) parms.push_back(resolve(e));
        else if (dp && dp->t == O_DICT) parms.push_back(dp);
    }

    for (size_t i = 0; i < filters.size(); i++) {
        const std::string& fn = filters[i];
        ObjP prm = i < parms.size() ? parms[i] : nullptr;
        if (fn == "FlateDecode" || fn == "Fl") {
            bool ok = false;
            out = flateInflate(out, stm->raw.size() * 3 + 512, &ok);
            if (!ok) { warn("flate decode failed"); }
            if (prm) {
                int pred = (int)objNum(resolve(prm->get("Predictor")), 1);
                if (pred > 1)
                    out = applyPredictor(out, pred,
                                         (int)objNum(resolve(prm->get("Colors")), 1),
                                         (int)objNum(resolve(prm->get("BitsPerComponent")), 8),
                                         (int)objNum(resolve(prm->get("Columns")), 1));
            }
        } else if (fn == "LZWDecode" || fn == "LZW") {
            int early = prm ? (int)objNum(resolve(prm->get("EarlyChange")), 1) : 1;
            out = lzwDecode(out, early);
            if (prm) {
                int pred = (int)objNum(resolve(prm->get("Predictor")), 1);
                if (pred > 1)
                    out = applyPredictor(out, pred,
                                         (int)objNum(resolve(prm->get("Colors")), 1),
                                         (int)objNum(resolve(prm->get("BitsPerComponent")), 8),
                                         (int)objNum(resolve(prm->get("Columns")), 1));
            }
        } else if (fn == "ASCIIHexDecode" || fn == "AHx") {
            out = asciiHexDecode(out);
        } else if (fn == "ASCII85Decode" || fn == "A85") {
            out = ascii85Decode(out);
        } else if (fn == "RunLengthDecode" || fn == "RL") {
            out = runLengthDecode(out);
        } else if (fn == "DCTDecode" || fn == "JPXDecode" || fn == "JBIG2Decode" ||
                   fn == "CCITTFaxDecode") {
            warn("unsupported image filter " + fn);
            return false;
        } else {
            warn("unknown filter " + fn);
            return false;
        }
    }
    return true;
}

// ─── page geometry ──────────────────────────────────────────────────────────
void Document::pageSize(int idx, double& w, double& h, int& rotate) {
    w = 612; h = 792; rotate = 0;
    if (idx < 0 || idx >= (int)pages.size()) return;
    const ObjP& p = pages[idx];
    ObjP box = p->get("CropBox");
    if (!box) box = p->get("MediaBox");
    if (box && box->t == O_ARR && box->arr.size() == 4) {
        // boxes are plain numbers in practice; references are resolved at parse time
        double x0 = box->arr[0] && box->arr[0]->t != O_REF ? box->arr[0]->num : 0;
        double y0 = box->arr[1] && box->arr[1]->t != O_REF ? box->arr[1]->num : 0;
        double x1 = box->arr[2] && box->arr[2]->t != O_REF ? box->arr[2]->num : 612;
        double y1 = box->arr[3] && box->arr[3]->t != O_REF ? box->arr[3]->num : 792;
        w = fabs(x1 - x0);
        h = fabs(y1 - y0);
    }
    ObjP r = p->get("Rotate");
    if (r) {
        if (r->t == O_REF) r = resolve(r);
        if (r) rotate = ((int)objNum(r, 0) % 360 + 360) % 360;
    }
}

// ─── glyph names → unicode ──────────────────────────────────────────────────
struct NameUni { const char* n; int u; };
static const NameUni kAGL[] = {
    {"space", 32}, {"exclam", 33}, {"quotedbl", 34}, {"numbersign", 35}, {"dollar", 36},
    {"percent", 37}, {"ampersand", 38}, {"quotesingle", 39}, {"quoteright", 0x2019},
    {"parenleft", 40}, {"parenright", 41}, {"asterisk", 42}, {"plus", 43}, {"comma", 44},
    {"hyphen", 45}, {"period", 46}, {"slash", 47}, {"zero", 48}, {"one", 49}, {"two", 50},
    {"three", 51}, {"four", 52}, {"five", 53}, {"six", 54}, {"seven", 55}, {"eight", 56},
    {"nine", 57}, {"colon", 58}, {"semicolon", 59}, {"less", 60}, {"equal", 61},
    {"greater", 62}, {"question", 63}, {"at", 64}, {"bracketleft", 91}, {"backslash", 92},
    {"bracketright", 93}, {"asciicircum", 94}, {"underscore", 95}, {"grave", 96},
    {"quoteleft", 0x2018}, {"braceleft", 123}, {"bar", 124}, {"braceright", 125},
    {"asciitilde", 126}, {"exclamdown", 0xA1}, {"cent", 0xA2}, {"sterling", 0xA3},
    {"yen", 0xA5}, {"section", 0xA7}, {"dieresis", 0xA8}, {"copyright", 0xA9},
    {"ordfeminine", 0xAA}, {"guillemotleft", 0xAB}, {"registered", 0xAE},
    {"degree", 0xB0}, {"plusminus", 0xB1}, {"periodcentered", 0xB7},
    {"guillemotright", 0xBB}, {"questiondown", 0xBF}, {"AE", 0xC6}, {"Oslash", 0xD8},
    {"germandbls", 0xDF}, {"ae", 0xE6}, {"oslash", 0xF8}, {"dotlessi", 0x131},
    {"dotlessj", 0x237}, {"Lslash", 0x141}, {"lslash", 0x142}, {"OE", 0x152},
    {"oe", 0x153}, {"Zcaron", 0x17D}, {"zcaron", 0x17E}, {"Scaron", 0x160},
    {"scaron", 0x161}, {"Ydieresis", 0x178}, {"florin", 0x192},
    // accents / spacing marks
    {"dotaccent", 0x2D9}, {"ring", 0x2DA}, {"breve", 0x2D8}, {"caron", 0x2C7},
    {"hungarumlaut", 0x2DD}, {"ogonek", 0x2DB}, {"circumflex", 0x2C6},
    {"tilde", 0x2DC}, {"macron", 0xAF}, {"acute", 0xB4}, {"cedilla", 0xB8},
    // typographic
    {"endash", 0x2013}, {"emdash", 0x2014}, {"quotedblleft", 0x201C},
    {"quotedblright", 0x201D}, {"quotedblbase", 0x201E}, {"quotesinglbase", 0x201A},
    {"dagger", 0x2020}, {"daggerdbl", 0x2021}, {"bullet", 0x2022}, {"ellipsis", 0x2026},
    {"perthousand", 0x2030}, {"guilsinglleft", 0x2039}, {"guilsinglright", 0x203A},
    {"fraction", 0x2044}, {"Euro", 0x20AC}, {"trademark", 0x2122},
    {"circlecopyrt", 0xA9}, {"minus", 0x2212},
    // ligatures
    {"fi", 0xFB01}, {"fl", 0xFB02}, {"ff", 0xFB00}, {"ffi", 0xFB03}, {"ffl", 0xFB04},
    // math (CMSY/CMMI territory)
    {"asteriskmath", 0x2217}, {"notequal", 0x2260}, {"infinity", 0x221E},
    {"lessequal", 0x2264}, {"greaterequal", 0x2265}, {"partialdiff", 0x2202},
    {"summation", 0x2211}, {"product", 0x220F}, {"integral", 0x222B},
    {"radical", 0x221A}, {"approxequal", 0x2248}, {"equivalence", 0x2261},
    {"element", 0x2208}, {"gradient", 0x2207}, {"proportional", 0x221D},
    {"angle", 0x2220}, {"logicaland", 0x2227}, {"logicalor", 0x2228},
    {"intersection", 0x2229}, {"union", 0x222A}, {"similar", 0x223C},
    {"arrowright", 0x2192}, {"arrowleft", 0x2190}, {"arrowboth", 0x2194},
    {"arrowup", 0x2191}, {"arrowdown", 0x2193}, {"arrowdblright", 0x21D2},
    {"prime", 0x2032}, {"second", 0x2033}, {"degreesign", 0xB0},
    {"openbullet", 0x25E6}, {"circle", 0x25CB}, {"square", 0x25A1},
    {"triangle", 0x25B3}, {"periodcentered_", 0xB7},
    // greek
    {"alpha", 0x3B1}, {"beta", 0x3B2}, {"gamma", 0x3B3}, {"delta", 0x3B4},
    {"epsilon", 0x3B5}, {"zeta", 0x3B6}, {"eta", 0x3B7}, {"theta", 0x3B8},
    {"iota", 0x3B9}, {"kappa", 0x3BA}, {"lambda", 0x3BB}, {"mu", 0x3BC},
    {"nu", 0x3BD}, {"xi", 0x3BE}, {"omicron", 0x3BF}, {"pi", 0x3C0},
    {"rho", 0x3C1}, {"sigma", 0x3C3}, {"tau", 0x3C4}, {"upsilon", 0x3C5},
    {"phi", 0x3C6}, {"chi", 0x3C7}, {"psi", 0x3C8}, {"omega", 0x3C9},
    {"Gamma", 0x393}, {"Delta", 0x394}, {"Theta", 0x398}, {"Lambda", 0x39B},
    {"Xi", 0x39E}, {"Pi", 0x3A0}, {"Sigma", 0x3A3}, {"Phi", 0x3A6},
    {"Psi", 0x3A8}, {"Omega", 0x3A9},
    {"theta1", 0x3D1}, {"phi1", 0x3D5}, {"omega1", 0x3D6}, {"sigma1", 0x3C2},
    {nullptr, 0}
};

static int aglLookup(const std::string& name) {
    for (int i = 0; kAGL[i].n; i++) if (name == kAGL[i].n) return kAGL[i].u;
    // "uniXXXX" / "uXXXX"
    if (name.size() == 7 && name.compare(0, 3, "uni") == 0) {
        int v = (int)strtol(name.c_str() + 3, nullptr, 16);
        if (v > 0) return v;
    }
    if (name.size() >= 5 && name[0] == 'u') {
        int v = (int)strtol(name.c_str() + 1, nullptr, 16);
        if (v > 0) return v;
    }
    // single letter names "a".."z" "A".."Z"
    if (name.size() == 1 && isalpha((unsigned char)name[0])) return (unsigned char)name[0];
    return 0;
}

static int winAnsiToUni(int code) {
    switch (code) {
        case 0x80: return 0x20AC; case 0x82: return 0x201A; case 0x83: return 0x192;
        case 0x84: return 0x201E; case 0x85: return 0x2026; case 0x86: return 0x2020;
        case 0x87: return 0x2021; case 0x88: return 0x2C6;  case 0x89: return 0x2030;
        case 0x8A: return 0x160;  case 0x8B: return 0x2039; case 0x8C: return 0x152;
        case 0x8E: return 0x17D;  case 0x91: return 0x2018; case 0x92: return 0x2019;
        case 0x93: return 0x201C; case 0x94: return 0x201D; case 0x95: return 0x2022;
        case 0x96: return 0x2013; case 0x97: return 0x2014; case 0x98: return 0x2DC;
        case 0x99: return 0x2122; case 0x9A: return 0x161;  case 0x9B: return 0x203A;
        case 0x9C: return 0x153;  case 0x9E: return 0x17E;  case 0x9F: return 0x178;
        default: break;
    }
    if (code >= 32 && code <= 255) return code;
    return 0;
}

int Font::unicodeFor(int code) const {
    auto it = toUni.find(code);
    if (it != toUni.end()) return it->second;
    auto d = diff.find(code);
    if (d != diff.end()) {
        int u = aglLookup(d->second);
        if (u) return u;
    }
    if (baseEncoding == "WinAnsiEncoding" || baseEncoding.empty()) {
        int u = winAnsiToUni(code);
        if (u) return u;
    }
    if (code >= 32 && code <= 126) return code;
    return 0;
}

double Font::widthFor(int code) const {
    if (hasWidths) {
        int i = code - firstChar;
        if (i >= 0 && i < (int)widths.size() && widths[i] > 0) return widths[i];
    }
    if (missingWidth > 0) return missingWidth;
    // No /Widths and no /MissingWidth: in practice this is one of the standard
    // 14 faces, whose advances are fixed. Using the family's AFM table keeps the
    // pen in step with the substituted outline face (a flat 500/em fallback
    // spaces Helvetica to look like a monospace font).
    if (embedded.empty() && code >= 32 && code <= 126) {
        int fi = (faceBold() ? 1 : 0) + (faceItalic() ? 2 : 0);
        int fam = faceClass();
        if (fam < 0 || fam > 2) fam = 0;
        return (double)kPdfCharW[fam][fi][code - 32];
    }
    return 500.0;
}

static bool containsCI(const std::string& hay, const char* needle) {
    std::string h, n;
    h.reserve(hay.size());
    for (char c : hay) h += (char)tolower((unsigned char)c);
    for (const char* p = needle; *p; p++) n += (char)tolower((unsigned char)*p);
    return h.find(n) != std::string::npos;
}

int Font::faceClass() const {
    if (containsCI(base, "mono") || containsCI(base, "courier") || containsCI(base, "cmtt") ||
        containsCI(base, "typewriter") || containsCI(base, "consol"))
        return 2;
    if (containsCI(base, "sans") || containsCI(base, "helvetica") || containsCI(base, "arial") ||
        containsCI(base, "verdana") || containsCI(base, "calibri") || containsCI(base, "carlito"))
        return 0;
    return 1;
}
bool Font::faceBold() const {
    return containsCI(base, "bold") || containsCI(base, "medi") || containsCI(base, "cmbx") ||
           containsCI(base, "heavy") || containsCI(base, "black") || containsCI(base, "semibold");
}
bool Font::faceItalic() const {
    return containsCI(base, "italic") || containsCI(base, "ital") || containsCI(base, "oblique") ||
           containsCI(base, "slant") || containsCI(base, "cmmi") || containsCI(base, "-it");
}

// ─── ToUnicode CMap ─────────────────────────────────────────────────────────
static void parseToUnicode(Document* doc, const std::string& cm, std::map<int, int>& out) {
    auto hexVal = [](const std::string& s) -> int {
        int v = 0;
        for (char c : s) {
            if (c >= '0' && c <= '9') v = v * 16 + (c - '0');
            else if (c >= 'a' && c <= 'f') v = v * 16 + (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v = v * 16 + (c - 'A' + 10);
        }
        return v;
    };
    ContentLexer lx(cm.data(), cm.size());
    Tok t;
    std::vector<int> stack;        // hex codes since the last section keyword
    int mode = 0;                  // 1 = bfchar, 2 = bfrange
    std::vector<Tok> pending;
    while (true) {
        lx.next(t);
        if (t.type == T_EOF) break;
        if (t.type == T_OP) {
            if (t.s == "beginbfchar") { mode = 1; stack.clear(); continue; }
            if (t.s == "beginbfrange") { mode = 2; stack.clear(); continue; }
            if (t.s == "endbfchar" || t.s == "endbfrange") { mode = 0; stack.clear(); continue; }
            continue;
        }
        if (t.type == T_STR) {
            // a byte string holding a hex code (ContentLexer gives <..> as bytes)
            int v = 0;
            for (unsigned char c : t.s) v = (v << 8) | c;
            stack.push_back(v);
            if (mode == 1 && stack.size() >= 2) {
                out[stack[0]] = stack[1];
                stack.clear();
            } else if (mode == 2 && stack.size() >= 2) {
                // dst may be an array (<lo> <hi> [ <a> <b> ... ]) — handle the
                // simple "lo hi dst" form; arrays are handled by the parser below
                int lo = stack[0], hi = stack[1];
                int dst = stack.size() >= 3 ? stack[2] : 0;
                if (stack.size() >= 3) {
                    for (int c = lo; c <= hi && c - lo < 512; c++) out[c] = dst + (c - lo);
                    stack.clear();
                }
            }
            continue;
        }
        if (t.type == T_ARR_OPEN) {
            // collect the destination array of a bfrange row
            std::vector<int> arr;
            while (true) {
                Tok a;
                lx.next(a);
                if (a.type == T_ARR_CLOSE || a.type == T_EOF) break;
                if (a.type == T_STR) {
                    int v = 0;
                    for (unsigned char c : a.s) v = (v << 8) | c;
                    arr.push_back(v);
                }
            }
            if (mode == 2 && stack.size() >= 2) {
                int lo = stack[0];
                for (size_t i = 0; i < arr.size(); i++) out[lo + (int)i] = arr[i];
            }
            stack.clear();
        }
    }
}

Font* Document::font(const ObjP& fontDict) {
    ObjP fd = resolve(fontDict);
    if (!fd || fd->t != O_DICT) return nullptr;
    char key[64];
    snprintf(key, sizeof(key), "%p", (void*)fd.get());
    auto it = fontCache.find(key);
    if (it != fontCache.end()) return it->second.get();

    auto f = std::make_shared<Font>();
    ObjP base = resolve(fd->get("BaseFont"));
    f->base = objName(base);
    f->subtype = objName(resolve(fd->get("Subtype")));

    ObjP descFont = fd;
    if (f->subtype == "Type0") {
        ObjP df = resolve(fd->get("DescendantFonts"));
        if (df && df->t == O_ARR && !df->arr.empty()) {
            ObjP d0 = resolve(df->arr[0]);
            if (d0) {
                descFont = d0;
                ObjP b2 = resolve(d0->get("BaseFont"));
                if (b2) f->base = objName(b2);
                // /W widths for CID fonts
                ObjP w = resolve(d0->get("W"));
                if (w && w->t == O_ARR) {
                    for (size_t i = 0; i + 1 < w->arr.size();) {
                        ObjP a = resolve(w->arr[i]);
                        ObjP b = resolve(w->arr[i + 1]);
                        if (b && b->t == O_ARR) {
                            int first = (int)objNum(a, 0);
                            for (size_t k = 0; k < b->arr.size(); k++) {
                                int code = first + (int)k;
                                if (code - f->firstChar >= 0 && code - f->firstChar < (int)f->widths.size())
                                    f->widths[code - f->firstChar] = objNum(resolve(b->arr[k]), 0);
                            }
                            i += 2;
                        } else if (i + 2 < w->arr.size()) {
                            int first = (int)objNum(a, 0), last = (int)objNum(b, 0);
                            double wd = objNum(resolve(w->arr[i + 2]), 0);
                            if (last >= first && last - first < 65536) {
                                if ((int)f->widths.size() < last + 1) f->widths.resize(last + 1, 0);
                                for (int c = first; c <= last; c++) f->widths[c] = wd;
                            }
                            i += 3;
                        } else break;
                    }
                    f->firstChar = 0;
                    f->hasWidths = !f->widths.empty();
                }
            }
        }
    }

    ObjP fc = resolve(descFont->get("FirstChar"));
    ObjP lc = resolve(descFont->get("LastChar"));
    if (fc) f->firstChar = (int)objNum(fc, 0);
    if (lc) f->lastChar = (int)objNum(lc, 0);
    ObjP widths = resolve(descFont->get("Widths"));
    if (widths && widths->t == O_ARR) {
        f->widths.resize(widths->arr.size(), 0);
        for (size_t i = 0; i < widths->arr.size(); i++)
            f->widths[i] = objNum(resolve(widths->arr[i]), 0);
        f->hasWidths = true;
    }
    ObjP desc = resolve(descFont->get("FontDescriptor"));
    if (desc) {
        ObjP mw = resolve(desc->get("MissingWidth"));
        if (mw) f->missingWidth = objNum(mw, 0);
        // Embedded TrueType programs are usable directly — best fidelity available.
        ObjP ff2 = resolve(desc->get("FontFile2"));
        if (ff2 && ff2->isStream) {
            std::string prog;
            if (decodeStream(ff2, prog) && prog.size() > 100) f->embedded = prog;
        }
    }

    ObjP enc = resolve(fd->get("Encoding"));
    if (enc) {
        if (enc->t == O_NAME) f->baseEncoding = enc->str;
        else if (enc->t == O_DICT) {
            ObjP be = resolve(enc->get("BaseEncoding"));
            if (be) f->baseEncoding = objName(be);
            ObjP diffs = resolve(enc->get("Differences"));
            if (diffs && diffs->t == O_ARR) {
                int cur = 0;
                for (auto& e : diffs->arr) {
                    ObjP r = resolve(e);
                    if (!r) continue;
                    if (r->t == O_INT || r->t == O_REAL) { cur = (int)r->num; continue; }
                    if (r->t == O_NAME) { f->diff[cur] = r->str; cur++; }
                }
            }
        }
    }
    if (f->baseEncoding.empty()) f->baseEncoding = "WinAnsiEncoding";
    // A TrueType face that ships without /Encoding and /ToUnicode is symbolic:
    // its byte codes are glyph indices, not characters (Office/plot outputs do this).
    if (f->subtype == "TrueType" && !enc && !fd->get("ToUnicode")) f->symbolic = true;

    ObjP tu = resolve(fd->get("ToUnicode"));
    if (tu && tu->isStream) {
        std::string cm;
        if (decodeStream(tu, cm)) parseToUnicode(this, cm, f->toUni);
    }

    Font* raw = f.get();
    fontCache[key] = f;
    return raw;
}

// ─── content stream lexer ───────────────────────────────────────────────────
void ContentLexer::next(Tok& t) {
    t.type = T_EOF;
    t.s.clear();
    t.num = 0;
    while (p < end) {
        unsigned char c = *p;
        if (isWS(c)) { ++p; continue; }
        if (c == '%') { while (p < end && *p != '\n' && *p != '\r') ++p; continue; }
        break;
    }
    if (p >= end) return;
    unsigned char c = (unsigned char)*p;

    if (c == '(') {
        // literal string with nesting + escapes
        ++p;
        int depth = 1;
        std::string s;
        while (p < end) {
            char ch = *p++;
            if (ch == '(') { ++depth; s += ch; continue; }
            if (ch == ')') { if (--depth == 0) break; s += ch; continue; }
            if (ch == '\\' && p < end) {
                char e = *p++;
                switch (e) {
                    case 'n': s += '\n'; break;
                    case 'r': s += '\r'; break;
                    case 't': s += '\t'; break;
                    case 'b': s += '\b'; break;
                    case 'f': s += '\f'; break;
                    case '\r': if (p < end && *p == '\n') ++p; break;
                    case '\n': break;
                    default:
                        if (e >= '0' && e <= '7') {
                            int v = e - '0', n = 1;
                            while (n < 3 && p < end && *p >= '0' && *p <= '7') { v = v * 8 + (*p - '0'); ++p; ++n; }
                            s += (char)(v & 0xFF);
                        } else s += e;
                }
                continue;
            }
            s += ch;
        }
        t.type = T_STR;
        t.s = s;
        return;
    }
    if (c == '<') {
        if (p + 1 < end && p[1] == '<') { p += 2; t.type = T_DICT_OPEN; return; }
        ++p;
        std::string s;
        int hi = -1;
        auto hex = [](int x) -> int {
            if (x >= '0' && x <= '9') return x - '0';
            if (x >= 'a' && x <= 'f') return x - 'a' + 10;
            if (x >= 'A' && x <= 'F') return x - 'A' + 10;
            return -1;
        };
        while (p < end && *p != '>') {
            int v = hex((unsigned char)*p++);
            if (v < 0) continue;
            if (hi < 0) hi = v;
            else { s += (char)(hi * 16 + v); hi = -1; }
        }
        if (hi >= 0) s += (char)(hi * 16);
        if (p < end) ++p;
        t.type = T_STR;
        t.s = s;
        return;
    }
    if (c == '>') { ++p; if (p < end && *p == '>') ++p; t.type = T_DICT_CLOSE; return; }
    if (c == '[') { ++p; t.type = T_ARR_OPEN; return; }
    if (c == ']') { ++p; t.type = T_ARR_CLOSE; return; }
    if (c == '/' ) {
        ++p;
        std::string s;
        while (p < end && isRegular((unsigned char)*p)) {
            if (*p == '#') {
                auto hex = [](int x) -> int {
                    if (x >= '0' && x <= '9') return x - '0';
                    if (x >= 'a' && x <= 'f') return x - 'a' + 10;
                    if (x >= 'A' && x <= 'F') return x - 'A' + 10;
                    return -1;
                };
                if (p + 2 < end) {
                    int h = hex((unsigned char)p[1]), l = hex((unsigned char)p[2]);
                    if (h >= 0 && l >= 0) { s += (char)(h * 16 + l); p += 3; continue; }
                }
            }
            s += *p++;
        }
        t.type = T_NAME;
        t.s = s;
        return;
    }
    if (c == '{' || c == '}') { ++p; t.type = T_OP; t.s = std::string(1, (char)c); return; }

    // number?
    {
        const char* s0 = p;
        if (*p == '+' || *p == '-') ++p;
        bool any = false, dot = false;
        while (p < end) {
            if (*p >= '0' && *p <= '9') { any = true; ++p; }
            else if (*p == '.' && !dot) { dot = true; ++p; }
            else break;
        }
        if (any && (p >= end || isWS((unsigned char)*p) || isDelim((unsigned char)*p) || *p == '/' || *p == '<')) {
            t.type = T_NUM;
            t.num = atof(std::string(s0, p).c_str());
            return;
        }
        p = s0;
    }
    // operator
    {
        std::string s;
        while (p < end && isRegular((unsigned char)*p)) s += *p++;
        if (s.empty()) { ++p; return; }
        t.type = T_OP;
        t.s = s;
    }
}

} // namespace pdf
