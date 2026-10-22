#pragma once
// Predefined CMaps: character codes -> CID, for Type0 fonts that name an Adobe
// CMap in /Encoding instead of using /Identity-H.

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace pdf {

struct CMapCodespace {
	int nbytes = 0;              // 1..4
	unsigned int lo = 0, hi = 0;
};

struct CMapEntry {
	unsigned long long key = 0;  // (nbytes << 32) | code
	int cid = 0;
};

class CMap {
public:
	std::vector<CMapCodespace> codespaces;
	std::vector<CMapEntry> entries;   // sorted by key

	// How many bytes the code starting at `bytes` occupies, or 0 if the bytes
	// do not fall in any codespace range.
	int codeLength(const unsigned char* bytes, size_t avail) const;

	// CID for a code of `nbytes` bytes; 0 when unmapped (CID 0 is .notdef).
	int lookup(unsigned int code, int nbytes) const;
};

// nullptr when the name is not one of the shipped CMaps.
const CMap* cmapFind(const std::string& name);

// Same, as a shared_ptr the caller can keep on a font.
std::shared_ptr<const CMap> cmapFindPtr(const std::string& name);

// Parse a CMap *stream* (an embedded `/Encoding`): begin codespacerange /
// begincidrange / begincidchar blocks plus `/Name usecmap` inheritance, which
// is resolved against the shipped tables. nullptr when nothing parseable.
std::shared_ptr<const CMap> cmapParse(const std::string& data);

}  // namespace pdf
