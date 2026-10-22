// JBIG2 and CCITT decoding, compiled into the engine.
//
// PDFium's decoders (via mozilla/pdf.js.jbig2) are shipped as a small wasm
// module. Rather than load and drive that module from JavaScript — a second
// binary, an async hand-off and a second page render — the module is translated
// to C with wabt's wasm2c and linked straight into pdf-reader.wasm.
//
// jbig2.c is generated, do not edit it. Regenerate with:
//   wasm2c jbig2.wasm -o jbig2.c
//
// The import shims below replace the JavaScript library pdf.js.jbig2 expects
// (setImageData / setLineData / createImageData / timers / heap growth).

#include "jbig2.h"

#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// wasm-rt calls this when the module traps (wasm-rt-impl.c is compiled with
// -DWASM_RT_TRAP_HANDLER=pdf_jbig2_trap). A trap in one image must not abort the
// whole engine, so it unwinds back to jbig2Decode.
extern "C" void pdf_jbig2_trap(wasm_rt_trap_t code);

// The generated code only forward-declares its import module; any non-null
// address works because the shims below are free functions.
struct w2c_a {
	int unused;
};

namespace {

w2c_jbig2 gInst;
w2c_a gImports;
bool gReady = false;

// Output sink: the generated code hands decoded rows back through imports, the
// same way the JS glue stored them on Module.imageData.
std::vector<uint8_t>* gOut = nullptr;
size_t gOutPitch8 = 0;
size_t gOutHeight = 0;
bool gOverflow = false;

wasm_rt_memory_t* memory() { return w2c_jbig2_i(&gInst); }

uint8_t* base() { return memory()->data; }

std::jmp_buf gTrap;
bool gTrapArmed = false;

void ensureReady() {
	if (!gReady) {
		if (getenv("JBIG2_DEBUG")) fprintf(stderr, "[jbig2] wasm_rt_init\n");
		wasm_rt_init();
		if (getenv("JBIG2_DEBUG")) fprintf(stderr, "[jbig2] instantiate\n");
		wasm2c_jbig2_instantiate(&gInst, &gImports);
		w2c_jbig2_j(&gInst);   // __wasm_call_ctors: run the module's static initialisers
		// Pre-grow: growing mid-decode works, but the module's own bounds checks
		// are cheap to satisfy up front and the peak for a page is a few tens
		// of MB.
		w2c_a_b(&gImports, 256u << 20);
		if (getenv("JBIG2_DEBUG")) fprintf(stderr, "[jbig2] memory %llu bytes\n", (unsigned long long)memory()->size);
		gReady = true;
	}
}

}  // namespace

extern "C" void pdf_jbig2_trap(wasm_rt_trap_t code) {
	if (getenv("JBIG2_DEBUG")) fprintf(stderr, "[jbig2] trap %d (%s)\n", (int)code, wasm_rt_strerror(code));
	if (gTrapArmed) std::longjmp(gTrap, code ? (int)code : 1);
	std::abort();
}

// ─── imports the generated module calls back into ───────────────────────────

extern "C" u32 w2c_a_a(struct w2c_a*, u32, f64) {
	return 0;   // __setitimer_js: no timers are used by the decoder
}

extern "C" u32 w2c_a_b(struct w2c_a*, u32 size) {
	wasm_rt_memory_t* mem = memory();
	if (size <= mem->size) return 1;
	const u64 need = ((u64)size - mem->size + 65535) / 65536;
	return wasm_rt_grow_memory(mem, need) != 0 ? 1 : 0;
}

extern "C" void w2c_a_c(struct w2c_a*, u32) {
	gOverflow = true;   // _proc_exit: treat as a decode failure
}

extern "C" void w2c_a_d(struct w2c_a*) {}   // __emscripten_runtime_keepalive_clear

extern "C" void w2c_a_e(struct w2c_a*) {    // __abort_js
	gOverflow = true;
}

extern "C" void w2c_a_g(struct w2c_a*, u32 size) {
	// _createImageData: called by the CCITT path before writing rows.
	if (!gOut) return;
	gOverflow = false;
	gOut->assign(size, 0);
}

extern "C" void w2c_a_f(struct w2c_a*, u32 linePtr, u32 pitch8, u32 offset) {
	if (!gOut || !gOut->size()) return;
	if ((size_t)offset + pitch8 > gOut->size()) {
		gOverflow = true;
		return;
	}
	std::memcpy(gOut->data() + offset, base() + linePtr, pitch8);
}

extern "C" void w2c_a_h(struct w2c_a*, u32 ptr, u32 pitch8, u32 pitch32, u32 height) {
	if (!gOut) return;
	gOut->assign((size_t)pitch8 * height, 0);
	gOutPitch8 = pitch8;
	gOutHeight = height;
	const uint8_t* src = base() + ptr;
	for (u32 y = 0; y < height; y++) {
		std::memcpy(gOut->data() + (size_t)y * pitch8, src + (size_t)y * pitch32, pitch8);
	}
}

// ─── public entry point used by the rasterizer ──────────────────────────────

namespace pdf {

// Decodes a JBIG2 image to 1 bit per pixel, rows packed MSB-first, top row
// first (pitch = (width + 7) / 8). `globals` may be empty. Returns false when
// the stream cannot be decoded; the caller then paints nothing.
bool jbig2Decode(const uint8_t* data, size_t len, int width, int height,
                 const uint8_t* globals, size_t globalsLen,
                 std::vector<uint8_t>& out) {
	if (!data || !len || width <= 0 || height <= 0) return false;
	if (width > 20000 || height > 20000) return false;

	ensureReady();
	gOut = &out;
	gOverflow = false;
	out.clear();

	u32 src = w2c_jbig2_k(&gInst, (u32)len);
	if (!src) {
		// Heap full: ask the module to grow, then retry once.
		if (!w2c_a_b(&gImports, (u32)(memory()->size + len + 65536))) return false;
		src = w2c_jbig2_k(&gInst, (u32)len);
		if (!src) return false;
	}
	if (getenv("JBIG2_DEBUG")) fprintf(stderr, "[jbig2] buffer %u for %zu bytes\n", src, len);
	std::memcpy(base() + src, data, len);

	u32 glo = 0;
	if (globals && globalsLen) {
		glo = w2c_jbig2_k(&gInst, (u32)globalsLen);
		if (glo) std::memcpy(base() + glo, globals, globalsLen);
	}

	gTrapArmed = true;
	if (setjmp(gTrap) == 0) {
		w2c_jbig2_m(&gInst, src, (u32)len, (u32)width, (u32)height, glo, (u32)(glo ? globalsLen : 0));
	} else {
		gOverflow = true;   // trapped: treat as a decode failure
	}
	gTrapArmed = false;

	w2c_jbig2_l(&gInst, src);
	if (glo) w2c_jbig2_l(&gInst, glo);

	const size_t expected = ((size_t)width + 7) / 8 * (size_t)height;
	const bool ok = !gOverflow && out.size() >= expected;
	out.resize(expected);
	gOut = nullptr;
	return ok;
}

}  // namespace pdf
