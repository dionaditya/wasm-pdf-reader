// Predefined CMaps: character code -> CID for Type0 fonts.
//
// A PDF may name its encoding (/UniJIS-UCS2-H, /GBK-EUC-H, ...) instead of using
// /Identity-H. Those names refer to Adobe's CMap resources, which the build
// compresses into cmaps_data.cpp (BSD-3-Clause, see THIRD-PARTY.md). Without
// them CJK text renders with the wrong glyphs or none at all.

#include "cmap.hpp"

#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <zlib.h>

namespace pdf {
namespace cmapdata {
extern const unsigned char kPacked[];
extern const size_t kPackedSize;
extern const size_t kRawSize;
}  // namespace cmapdata

namespace {

std::map<std::string, CMap>* gTables = nullptr;
std::once_flag gOnce;

unsigned long long readVarint(const unsigned char*& p, const unsigned char* end) {
	unsigned long long v = 0;
	int shift = 0;
	while (p < end) {
		unsigned char b = *p++;
		v |= (unsigned long long)(b & 0x7F) << shift;
		if (!(b & 0x80)) return v;
		shift += 7;
	}
	return v;
}

int unzig(unsigned long long v) { return (v & 1) ? -(int)(v >> 1) - 1 : (int)(v >> 1); }

void build() {
	static std::vector<unsigned char> raw;
	raw.resize(cmapdata::kRawSize + 64);
	uLongf outLen = (uLongf)raw.size();
	if (uncompress(raw.data(), &outLen, cmapdata::kPacked, (uLong)cmapdata::kPackedSize) != Z_OK) {
		gTables = new std::map<std::string, CMap>();
		return;
	}
	raw.resize(outLen);

	const unsigned char* p = raw.data();
	const unsigned char* end = p + raw.size();
	if (raw.size() < 4 || std::memcmp(p, "WCM2", 4) != 0) {
		gTables = new std::map<std::string, CMap>();
		return;
	}
	p += 4;
	unsigned int nTables = readVarint(p, end);
	auto tables = new std::map<std::string, CMap>();
	for (unsigned int i = 0; i < nTables && p < end; i++) {
		unsigned int nameLen = readVarint(p, end);
		std::string name((const char*)p, nameLen);
		p += nameLen;
		CMap cmap;
		unsigned int nRanges = readVarint(p, end);
		cmap.codespaces.reserve(nRanges);
		for (unsigned int r = 0; r < nRanges; r++) {
			CMapCodespace cs;
			cs.nbytes = *p++;
			cs.lo = readVarint(p, end);
			cs.hi = readVarint(p, end);
			cmap.codespaces.push_back(cs);
		}
		unsigned int nPairs = readVarint(p, end);
		cmap.entries.reserve(nPairs);
		unsigned long long prevKey = 0;
		int prevCid = 0;
		for (unsigned int k = 0; k < nPairs; k++) {
			prevKey += readVarint(p, end);
			prevCid += unzig(readVarint(p, end));
			cmap.entries.push_back(CMapEntry{prevKey, prevCid});
		}
		(*tables)[name] = std::move(cmap);
	}
	gTables = tables;
}

}  // namespace

const CMap* cmapFind(const std::string& name) {
	std::call_once(gOnce, build);
	if (!gTables) return nullptr;
	auto it = gTables->find(name);
	return it == gTables->end() ? nullptr : &it->second;
}

int CMap::codeLength(const unsigned char* bytes, size_t avail) const {
	for (int n = 1; n <= 4 && (size_t)n <= avail; n++) {
		unsigned int code = 0;
		for (int i = 0; i < n; i++) code = (code << 8) | bytes[i];
		for (const CMapCodespace& cs : codespaces) {
			if (cs.nbytes == n && code >= cs.lo && code <= cs.hi) return n;
		}
	}
	return 0;
}

int CMap::lookup(unsigned int code, int nbytes) const {
	const unsigned long long want = ((unsigned long long)nbytes << 32) | code;
	size_t lo = 0, hi = entries.size();
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		unsigned long long k = entries[mid].key;
		if (k == want) return entries[mid].cid;
		if (k < want) lo = mid + 1;
		else hi = mid;
	}
	return 0;
}

}  // namespace pdf
