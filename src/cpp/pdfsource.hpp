// pdfsource.hpp — where a document's bytes come from.
//
// The parser used to require the whole file in one contiguous wasm buffer, so a
// 300 MB scan needed 300 MB of heap before the first page could be drawn. A
// ByteSource replaces that: the Document keeps a small resident *window* and
// asks the source for bytes as it walks the file, which is what makes ranged
// loading (HTTP `Range:` / Blob slices) possible at all.
//
// Two implementations:
//   * MemorySource — the whole file is already in memory (an ArrayBuffer the
//     caller handed us, or the streaming copy into wasm). `direct()` returns it,
//     so the Document uses it in place: no window, no copies, byte-identical
//     behaviour to the pre-M9.2 engine. Every existing test runs this path.
//   * CallbackSource — a synchronous pull from the host (in the browser: a
//     worker doing `FileReaderSync` on a Blob slice, or a sync XHR with a Range
//     header). Only the requested window is resident.
//
// The callback is synchronous by necessity: the parser is a synchronous
// recursive-descent lexer. That is the same trade pdf.js makes when it falls
// back to a ranged source, except we do it off the main thread (M9.1) so a
// blocking fetch cannot freeze the tab.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace pdf {

struct ByteSource {
    virtual ~ByteSource() {}
    /// File size in bytes. 64-bit on purpose: wasm32's `size_t` is 32 bits and
    /// a file of exactly 4 GiB would truncate to 0 (see the ranged path notes in
    /// pdfdoc.cpp). The *cap* below stays narrow — it is a window read.
    virtual uint64_t size() const = 0;

    /// Copy up to `cap` bytes starting at `off` into `dst`.
    /// Returns the number of bytes copied; 0 means the read failed.
    virtual size_t fetch(uint64_t off, uint8_t* dst, size_t cap) = 0;

    /// Whole file resident in one contiguous buffer? Then the Document uses it
    /// directly and never slides a window (the pre-M9.2 path).
    virtual const uint8_t* direct() const { return nullptr; }

    /// Bytes actually pulled through `fetch` — the number that proves ranged
    /// loading is doing something (`test/ranged.mjs` asserts on it).
    size_t fetched = 0;
    /// Number of fetch calls, for the same reason.
    size_t fetches = 0;
};

/// The file is a contiguous buffer someone else owns (or this one owns).
struct MemorySource : ByteSource {
    std::string buf;

    MemorySource() {}
    explicit MemorySource(std::string bytes) : buf(std::move(bytes)) {}

    uint64_t size() const override { return (uint64_t)buf.size(); }
    const uint8_t* direct() const override { return (const uint8_t*)buf.data(); }

    size_t fetch(uint64_t off, uint8_t* dst, size_t cap) override {
        if (off >= buf.size()) return 0;
        size_t n = std::min(cap, (size_t)(buf.size() - (size_t)off));
        std::memcpy(dst, buf.data() + off, n);
        fetched += n;
        fetches++;
        return n;
    }
};

/// Bytes pulled from the host through a synchronous callback.
/// `fn(off, dst, cap, ctx)` returns the number of bytes written into `dst`.
struct CallbackSource : ByteSource {
    uint64_t total = 0;
    size_t (*fn)(uint64_t off, uint8_t* dst, size_t cap, void* ctx) = nullptr;
    void* ctx = nullptr;

    CallbackSource() {}
    CallbackSource(uint64_t n, size_t (*f)(uint64_t, uint8_t*, size_t, void*), void* c)
        : total(n), fn(f), ctx(c) {}

    uint64_t size() const override { return total; }

    size_t fetch(uint64_t off, uint8_t* dst, size_t cap) override {
        if (!fn || off >= total) return 0;
        size_t want = (size_t)std::min<uint64_t>(cap, total - off);
        size_t got = fn(off, dst, want, ctx);
        if (got > want) got = want;
        fetched += got;
        fetches++;
        return got;
    }
};

}  // namespace pdf
