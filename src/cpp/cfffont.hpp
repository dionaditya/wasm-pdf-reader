// CFF / Type2 charstring support for embedded PDF fonts.
//
// PDF embeds PostScript-outline fonts in three shapes:
//   /FontFile3 /Subtype /Type1C        — bare CFF, name-keyed (simple fonts)
//   /FontFile3 /Subtype /CIDFontType0C — bare CFF, CID-keyed (Type0 fonts)
//   /FontFile3 /Subtype /OpenType      — sfnt wrapper (handled by stb_truetype)
//
// Bare CFF has no sfnt tables, so stb_truetype cannot open it, and its CFF
// path does not understand CID FDArray/FDSelect local subrs either. This
// interpreter produces outlines directly in em units (y up, baseline 0) so
// the rasterizer can paint them like any other path.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace pdf {

// One outline command in em units. type: 0 = move, 1 = line, 2 = cubic.
struct CffSeg {
    int    type = 0;
    double x1 = 0, y1 = 0;
    double x2 = 0, y2 = 0;
    double x = 0, y = 0;
};

class CffFont {
public:
    bool parse(const uint8_t* data, size_t len);
    bool ok() const { return ok_; }
    bool isCid() const { return cid_; }
    int  numGlyphs() const { return nGlyphs_; }

    // Name-keyed helpers.
    int gidForName(const std::string& name) const;
    int gidForCode(int code) const;          // built-in encoding, -1 if absent
    bool hasBuiltinEncoding() const { return !encToGid_.empty(); }

    // CID-keyed helper.
    int gidForCid(int cid) const;

    // Advance in em units (0 when the charstring carries no width).
    double advanceForGid(int gid) const;

    // Outline in em units, y up.
    bool glyphPath(int gid, std::vector<CffSeg>& out) const;

    // Used by the charstring interpreter (seac composites).
    bool glyphPathImpl(int gid, std::vector<CffSeg>& out, int depth) const;
    int  seacGid(int code) const;

private:
    struct Index { std::vector<std::pair<size_t, size_t>> items; size_t end = 0; };
    struct FdInfo { size_t subrsFirst = 0, subrsLast = 0; int nSubrs = 0;
                    double defaultWidth = 0, nominalWidth = 0;
                    std::vector<std::pair<size_t, size_t>> subrsList; };

    Index readIndex(size_t pos) const;
    bool  readDict(size_t start, size_t end, std::map<int, std::vector<double>>& out) const;
    bool  parseCharset(int offset, bool cid);
    bool  parseEncoding(int offset);
    bool  parsePrivate(const std::map<int, std::vector<double>>& pd, size_t base,
                       FdInfo& fd) const;
    size_t subrBias(int n) const { return n < 1240 ? 107 : (n < 33900 ? 1131 : 32768); }

    std::vector<uint8_t> data_;
    bool ok_ = false, cid_ = false;
    int  nGlyphs_ = 0;
    double fmScale_ = 0.001;                  // FontMatrix x/y scale

    Index charStrings_;
    Index gsubrs_;
    FdInfo defFd_;
    std::vector<FdInfo> fds_;
    std::vector<int>    fdOfGid_;             // CID-keyed FDSelect
    std::vector<int>    cidOfGid_;            // CID-keyed charset (gid -> cid)
    std::map<int, int>  cidToGid_;
    std::vector<std::string> glyphNames_;     // name-keyed charset (gid -> name)
    std::map<std::string, int> nameToGid_;
    std::vector<int>    encToGid_;            // 256 entries, 0 = unmapped
    std::vector<std::string> strings_;        // String INDEX (SID >= 391)
    mutable std::map<int, double> widthCache_;
};

}  // namespace pdf
