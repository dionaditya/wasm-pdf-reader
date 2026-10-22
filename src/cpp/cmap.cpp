// Predefined CMaps: character code -> CID for Type0 fonts.
//
// A PDF may name its encoding (/UniJIS-UCS2-H, /GBK-EUC-H, ...) instead of using
// /Identity-H. Those names refer to Adobe's CMap resources, which the build
// compresses into cmaps_data.cpp (BSD-3-Clause, see THIRD-PARTY.md). Without
// them CJK text renders with the wrong glyphs or none at all.

#include "cmap.hpp"

#include <algorithm>
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

std::shared_ptr<const CMap> cmapFindPtr(const std::string& name) {
	const CMap* m = cmapFind(name);
	if (!m) return nullptr;
	return std::shared_ptr<const CMap>(std::shared_ptr<void>(), m);
}

// ─── embedded CMap streams ──────────────────────────────────────────────────
// A Type0 /Encoding may be a stream instead of a name: a small PostScript-ish
// program whose only interesting parts are the codespace ranges (how many
// bytes a code occupies), the code -> CID blocks, and `usecmap`, which pulls in
// a predefined table. That last one matters: plenty of producers write a tiny
// CMap that only overrides a handful of codes.

namespace {

bool isHex(char c) {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Skip whitespace and postscript comments (% to end of line).
void skipSpace(const std::string& s, size_t& i) {
	while (i < s.size()) {
		char c = s[i];
		if (c == '%') {
			while (i < s.size() && s[i] != '\n') i++;
		} else if (c == ' ' || c == '	' || c == '\r' || c == '\n' || c == '\f' || c == '\0') {
			i++;
		} else {
			return;
		}
	}
}

// <AABB> -> value; returns false when the next token is not a hex string.
bool readHex(const std::string& s, size_t& i, unsigned int& val, int& nbytes) {
	skipSpace(s, i);
	if (i >= s.size() || s[i] != '<') return false;
	i++;
	unsigned int v = 0;
	int n = 0;
	while (i < s.size() && s[i] != '>') {
		if (!isHex(s[i])) return false;
		auto hexv = [](char c) -> unsigned { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
		v = (v << 4) | hexv(s[i]);
		n++;
		i++;
	}
	if (i >= s.size()) return false;
	i++;                 // '>'
	if (n == 0 || n > 8) return false;
	val = v;
	nbytes = (n + 1) / 2;
	return true;
}

bool readInt(const std::string& s, size_t& i, long& val) {
	skipSpace(s, i);
	bool neg = false;
	if (i < s.size() && (s[i] == '-' || s[i] == '+')) { neg = s[i] == '-'; i++; }
	size_t start = i;
	long v = 0;
	while (i < s.size() && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); i++; }
	if (i == start) return false;
	val = neg ? -v : v;
	return true;
}

// Index of `tok` at or after `from`, std::string::npos when absent. A token at
// offset 0 is real, so callers must compare against npos rather than truthiness.
size_t findToken(const std::string& s, const char* tok, size_t from) {
	return s.find(tok, from);
}

void addSorted(CMap& cm, unsigned long long key, int cid) {
	cm.entries.push_back(CMapEntry{key, cid});
}

}  // namespace

std::shared_ptr<const CMap> cmapParse(const std::string& data) {
	if (data.empty()) return nullptr;
	auto cm = std::make_shared<CMap>();
	bool sawMapping = false;

	// /UseCMap inheritance: either `/Name usecmap` or a `/UseCMap /Name` pair.
	// Pull the base table in first; explicit ranges later override it.
	{
		size_t use = findToken(data, "usecmap", 0);
		if (use != std::string::npos) {
			// walk backwards to the preceding /Name
			size_t j = use;
			while (j > 0 && (data[j - 1] == ' ' || data[j - 1] == '	' ||
			                 data[j - 1] == '\r' || data[j - 1] == '\n')) j--;
			size_t end = j;
			while (j > 0 && !strchr(" 	\r\n\f()<>[]{}/%", data[j - 1])) j--;
			if (j < end) {
				std::string baseName = data.substr(j, end - j);
				if (const CMap* base = cmapFind(baseName)) {
					cm->codespaces = base->codespaces;
					cm->entries = base->entries;
				}
			}
		}
	}

	// codespace ranges
	size_t p = 0;
	while (p < data.size()) {
		size_t b = findToken(data, "begincodespacerange", p);
		if (b == std::string::npos) break;
		size_t i = b + strlen("begincodespacerange");
		for (;;) {
			unsigned int lo = 0, hi = 0;
			int nlo = 0, nhi = 0;
			size_t before = i;
			if (!readHex(data, i, lo, nlo)) { i = before; break; }
			if (!readHex(data, i, hi, nhi)) break;
			if (nlo == nhi) {
				CMapCodespace cs;
				cs.nbytes = nlo;
				cs.lo = lo;
				cs.hi = hi;
				cm->codespaces.push_back(cs);
			}
		}
		size_t e = findToken(data, "endcodespacerange", b);
		p = e != std::string::npos ? e + strlen("endcodespacerange") : b + 1;
	}

	// cid ranges: <lo> <hi> <cid>
	p = 0;
	while (p < data.size()) {
		size_t b = findToken(data, "begincidrange", p);
		if (b == std::string::npos) break;
		size_t i = b + strlen("begincidrange");
		for (;;) {
			unsigned int lo = 0, hi = 0;
			int nlo = 0, nhi = 0;
			long cid = 0;
			if (!readHex(data, i, lo, nlo)) break;
			if (!readHex(data, i, hi, nhi)) break;
			if (!readInt(data, i, cid)) break;
			if (nlo != nhi || hi < lo || (hi - lo) > 200000) continue;
			for (unsigned int c = lo; c <= hi; c++)
				addSorted(*cm, ((unsigned long long)nlo << 32) | c, (int)(cid + (c - lo)));
			sawMapping = true;
		}
		size_t e = findToken(data, "endcidrange", b);
		p = e != std::string::npos ? e + strlen("endcidrange") : b + 1;
	}

	// single mappings: <code> <cid>
	p = 0;
	while (p < data.size()) {
		size_t b = findToken(data, "begincidchar", p);
		if (b == std::string::npos) break;
		size_t i = b + strlen("begincidchar");
		for (;;) {
			unsigned int code = 0;
			int n = 0;
			long cid = 0;
			if (!readHex(data, i, code, n)) break;
			if (!readInt(data, i, cid)) break;
			addSorted(*cm, ((unsigned long long)n << 32) | code, (int)cid);
			sawMapping = true;
		}
		size_t e = findToken(data, "endcidchar", b);
		p = e != std::string::npos ? e + strlen("endcidchar") : b + 1;
	}

	if (!sawMapping && cm->codespaces.empty()) return nullptr;

	// sort + last-one-wins so an explicit range can override an inherited entry
	std::sort(cm->entries.begin(), cm->entries.end(),
	          [](const CMapEntry& a, const CMapEntry& b) { return a.key < b.key; });
	std::vector<CMapEntry> uniq;
	uniq.reserve(cm->entries.size());
	for (const CMapEntry& e : cm->entries) {
		if (!uniq.empty() && uniq.back().key == e.key) uniq.back().cid = e.cid;
		else uniq.push_back(e);
	}
	cm->entries.swap(uniq);
	return cm;
}

}  // namespace pdf