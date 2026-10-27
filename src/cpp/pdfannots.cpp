// Link annotations: /Annots /Subtype /Link, with /URI actions and internal
// /GoTo destinations (direct or named). Rects stay in PDF page space (y up);
// the JS layer flips them for CSS.

#include "pdfdoc.hpp"

namespace pdf {

namespace {

bool numOf(const ObjP& o, double& out) {
    if (!o) return false;
    if (o->t == O_INT || o->t == O_REAL) { out = o->num; return true; }
    return false;
}

}  // namespace

int Document::pageIndex(const ObjP& pageRef) {
    if (!pageRef) return -1;
    if (pageIndexByObj.empty()) {
        for (size_t i = 0; i < pages.size(); i++)
            if (pages[i]) pageIndexByObj[pages[i]->objNum] = (int)i;
    }
    if (pageRef->t == O_REF) {
        auto it = pageIndexByObj.find(pageRef->ref);
        if (it != pageIndexByObj.end()) return it->second;
        // the reference may be indirect twice
        ObjP r = resolve(pageRef);
        if (r && r->t == O_DICT) {
            auto it2 = pageIndexByObj.find(r->objNum);
            if (it2 != pageIndexByObj.end()) return it2->second;
        }
        return -1;
    }
    if (pageRef->t == O_DICT) {
        auto it = pageIndexByObj.find(pageRef->objNum);
        if (it != pageIndexByObj.end()) return it->second;
    }
    return -1;
}

// /Names /Dests name tree, and the older /Dests dictionary on the catalog.
ObjP Document::lookupNamedDest(const std::string& name) {
    ObjP root = trailer ? resolve(trailer->get("Root")) : nullptr;
    if (!root || root->t != O_DICT) return nullptr;

    // Old style: /Dests is a dictionary of name -> dest
    ObjP old = resolve(root->get("Dests"));
    if (old && old->t == O_DICT) {
        for (auto& kv : old->dict)
            if (kv.first == name) return resolve(kv.second);
    }

    // Name tree: /Names << /Dests << /Names [ (a) [..] (b) [..] ] /Kids [...] >> >>
    ObjP names = resolve(root->get("Names"));
    ObjP tree = names ? resolve(names->get("Dests")) : nullptr;
    if (!tree || tree->t != O_DICT) return nullptr;

    std::vector<ObjP> stack{tree};
    int guard = 0;
    while (!stack.empty() && guard++ < 512) {
        ObjP node = resolve(stack.back());
        stack.pop_back();
        if (!node || node->t != O_DICT) continue;
        ObjP arr = resolve(node->get("Names"));
        if (arr && arr->t == O_ARR) {
            for (size_t i = 0; i + 1 < arr->arr.size(); i += 2) {
                ObjP key = resolve(arr->arr[i]);
                if (!key || key->t != O_STR) continue;
                if (key->str == name) return resolve(arr->arr[i + 1]);
            }
        }
        ObjP kids = resolve(node->get("Kids"));
        if (kids && kids->t == O_ARR)
            for (auto& k : kids->arr) stack.push_back(k);
    }
    return nullptr;
}

// A destination array: [ page /XYZ x y z ] (also /Fit, /FitH ...).
ObjP Document::resolveDest(const ObjP& dest, int depth) {
    if (!dest || depth > 4) return nullptr;
    ObjP d = resolve(dest);
    if (!d) return nullptr;
    if (d->t == O_STR || d->t == O_NAME) {
        ObjP named = lookupNamedDest(d->str);
        return named ? resolveDest(named, depth + 1) : nullptr;
    }
    if (d->t == O_DICT) {
        // /D may hold the array in an action-style dictionary
        ObjP inner = resolve(d->get("D"));
        return inner ? resolveDest(inner, depth + 1) : nullptr;
    }
    return d;
}

const std::vector<Document::Link>& Document::links(int idx) {
    auto cached = linkCache.find(idx);
    if (cached != linkCache.end()) return cached->second;

    std::vector<Link> out;
    if (idx >= 0 && idx < (int)pages.size()) {
        const ObjP& page = pages[idx];
        ObjP annots = resolve(page->get("Annots"));
        if (annots && annots->t == O_ARR) {
            for (auto& entry : annots->arr) {
                ObjP an = resolve(entry);
                if (!an || an->t != O_DICT) continue;
                if (objName(resolve(an->get("Subtype"))) != "Link") continue;

                Link link;
                ObjP rect = resolve(an->get("Rect"));
                if (!rect || rect->t != O_ARR || rect->arr.size() < 4) continue;
                double v[4] = {0, 0, 0, 0};
                bool ok = true;
                for (int i = 0; i < 4; i++) ok = ok && numOf(resolve(rect->arr[i]), v[i]);
                if (!ok) continue;
                link.x0 = std::min(v[0], v[2]); link.x1 = std::max(v[0], v[2]);
                link.y0 = std::min(v[1], v[3]); link.y1 = std::max(v[1], v[3]);
                if (link.x1 - link.x0 <= 0 || link.y1 - link.y0 <= 0) continue;

                // /A << /S /URI ... >> or << /S /GoTo /D ... >>, else /Dest
                ObjP action = resolve(an->get("A"));
                std::string kind = action ? objName(resolve(action->get("S"))) : std::string();
                if (kind == "URI") {
                    ObjP uri = action ? resolve(action->get("URI")) : nullptr;
                    if (uri && uri->t == O_STR) link.uri = uri->str;
                } else if (kind == "GoTo") {
                    ObjP target = resolveDest(action->get("D"), 0);
                    if (target && target->t == O_ARR && !target->arr.empty())
                        link.page = pageIndex(target->arr[0]);
                } else if (kind == "GoToR") {
                    // Remote target: keep the file name so the UI can decide.
                    ObjP f = action ? resolve(action->get("F")) : nullptr;
                    if (f && f->t == O_STR) link.uri = f->str;
                } else if (!action) {
                    ObjP dest = an->get("Dest");
                    if (dest) {
                        ObjP target = resolveDest(dest, 0);
                        if (target && target->t == O_ARR && !target->arr.empty())
                            link.page = pageIndex(target->arr[0]);
                    }
                }

                if (link.uri.empty() && link.page < 0) continue;   // nothing to do
                out.push_back(std::move(link));
            }
        }
    }
    return linkCache[idx] = std::move(out);
}

namespace {

// PDF text strings are byte strings; a leading FE FF marks UTF-16BE. Annotations
// written by Acrobat and Preview use that form for any non-ASCII comment.
std::string annotText(const ObjP& o) {
    if (!o || o->t != O_STR) return std::string();
    const std::string& s = o->str;
    bool utf16 = s.size() >= 2 && (unsigned char)s[0] == 0xFE && (unsigned char)s[1] == 0xFF;
    if (!utf16) {
        // PDFDocEncoding matches Latin-1 for the printable range, which is what
        // every producer writes in practice.
        return s;
    }
    std::string out;
    for (size_t i = 2; i + 1 < s.size(); i += 2) {
        unsigned u = ((unsigned char)s[i] << 8) | (unsigned char)s[i + 1];
        if (u >= 0xD800 && u <= 0xDBFF && i + 3 < s.size()) {
            unsigned lo = ((unsigned char)s[i + 2] << 8) | (unsigned char)s[i + 3];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        if (u < 0x80) {
            out.push_back((char)u);
        } else if (u < 0x800) {
            out.push_back((char)(0xC0 | (u >> 6)));
            out.push_back((char)(0x80 | (u & 0x3F)));
        } else if (u < 0x10000) {
            out.push_back((char)(0xE0 | (u >> 12)));
            out.push_back((char)(0x80 | ((u >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (u & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (u >> 18)));
            out.push_back((char)(0x80 | ((u >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((u >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (u & 0x3F)));
        }
    }
    return out;
}

void readRect(Document* doc, const ObjP& an, const char* key,
              double& x0, double& y0, double& x1, double& y1) {
    x0 = y0 = x1 = y1 = 0;
    ObjP r = doc->resolve(an->get(key));
    if (!r || r->t != O_ARR || r->arr.size() < 4) return;
    double v[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; i++) v[i] = objNum(doc->resolve(r->arr[i]), 0);
    x0 = std::min(v[0], v[2]); x1 = std::max(v[0], v[2]);
    y0 = std::min(v[1], v[3]); y1 = std::max(v[1], v[3]);
}

}  // namespace

// Every annotation that is not a link. Links keep their own accessor because the
// viewer turns them into anchors; these become comment marks with a popup.
const std::vector<Document::Annot>& Document::annots(int idx) {
    auto cached = annotCache.find(idx);
    if (cached != annotCache.end()) return cached->second;

    std::vector<Annot> out;
    if (idx >= 0 && idx < (int)pages.size()) {
        const ObjP& page = pages[idx];
        ObjP annots = resolve(page->get("Annots"));
        if (annots && annots->t == O_ARR) {
            for (auto& entry : annots->arr) {
                ObjP an = resolve(entry);
                if (!an || an->t != O_DICT) continue;

                Annot a;
                a.subtype = objName(resolve(an->get("Subtype")));
                // Links have their own path; a popup is only the geometry of its
                // parent note, so painting it again would be a second window.
                if (a.subtype.empty() || a.subtype == "Link" || a.subtype == "Popup") continue;

                // /F bit 2 = Hidden, bit 6 = NoView: neither is on-screen content.
                int flags = (int)objNum(resolve(an->get("F")), 0);
                if ((flags & 2) || (flags & 32)) continue;

                readRect(this, an, "Rect", a.x0, a.y0, a.x1, a.y1);

                a.contents = annotText(resolve(an->get("Contents")));
                a.author = annotText(resolve(an->get("T")));
                a.subject = annotText(resolve(an->get("Subj")));
                a.name = objName(resolve(an->get("Name")));
                a.modified = annotText(resolve(an->get("M")));

                ObjP c = resolve(an->get("C"));
                if (c && c->t == O_ARR && !c->arr.empty()) {
                    double v[4] = {0, 0, 0, 0};
                    size_t ncomp = std::min<size_t>(4, c->arr.size());
                    for (size_t i = 0; i < ncomp; i++)
                        v[i] = objNum(resolve(c->arr[i]), 0);
                    if (ncomp == 1) {
                        a.color[0] = a.color[1] = a.color[2] = (float)v[0];
                    } else if (ncomp == 3) {
                        for (int i = 0; i < 3; i++) a.color[i] = (float)v[i];
                    } else if (ncomp >= 4) {
                        // Naive CMYK -> RGB: the colour is a UI hint here, not ink.
                        a.color[0] = (float)((1 - v[0]) * (1 - v[3]));
                        a.color[1] = (float)((1 - v[1]) * (1 - v[3]));
                        a.color[2] = (float)((1 - v[2]) * (1 - v[3]));
                    }
                    a.hasColor = true;
                }

                ObjP popup = resolve(an->get("Popup"));
                if (popup && popup->t == O_DICT) {
                    readRect(this, popup, "Rect", a.px0, a.py0, a.px1, a.py1);
                    a.hasPopup = true;
                }

                out.push_back(std::move(a));
            }
        }
    }
    return annotCache[idx] = std::move(out);
}

}  // namespace pdf
