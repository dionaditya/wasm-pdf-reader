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

}  // namespace pdf
