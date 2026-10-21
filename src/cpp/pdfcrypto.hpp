// Minimal crypto for the PDF Standard security handler: MD5 (key derivation),
// RC4 (V1/V2) and AES-128/256-CBC (V4 AESV2, V5 AESV3). Tables are computed at
// startup so no long constant blocks can be mistyped.
#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

namespace pdfcrypto {

// ── MD5 ─────────────────────────────────────────────────────────────────────
struct MD5 {
    uint32_t h[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    uint64_t len = 0;
    uint8_t  block[64];
    size_t   fill = 0;

    static uint32_t rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

    void process(const uint8_t* p) {
        static uint32_t K[64]; static bool init = false;
        static const int S[64] = {7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
                                  5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
                                  4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
                                  6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21};
        if (!init) { for (int i = 0; i < 64; i++) K[i] = (uint32_t)(fabs(sin((double)(i + 1))) * 4294967296.0); init = true; }
        uint32_t m[16];
        for (int i = 0; i < 16; i++)
            m[i] = (uint32_t)p[i*4] | ((uint32_t)p[i*4+1] << 8) | ((uint32_t)p[i*4+2] << 16) | ((uint32_t)p[i*4+3] << 24);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; i++) {
            uint32_t f, g;
            if (i < 16)      { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5*i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d;          g = (3*i + 5) % 16; }
            else             { f = c ^ (b | ~d);       g = (7*i) % 16; }
            uint32_t tmp = d;
            d = c; c = b;
            b = b + rol(a + f + K[i] + m[g], S[i]);
            a = tmp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    }
    void update(const void* data, size_t n) {
        const uint8_t* p = (const uint8_t*)data;
        len += n;
        while (n) {
            size_t take = 64 - fill; if (take > n) take = n;
            memcpy(block + fill, p, take); fill += take; p += take; n -= take;
            if (fill == 64) { process(block); fill = 0; }
        }
    }
    std::string final() {
        uint64_t bits = len * 8;
        uint8_t pad = 0x80; update(&pad, 1);
        uint8_t z = 0;
        while (fill != 56) update(&z, 1);
        uint8_t lenb[8];
        for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (8 * i));
        update(lenb, 8);
        std::string out(16, '\0');
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) out[i*4 + j] = (char)((h[i] >> (8*j)) & 0xff);
        return out;
    }
};
inline std::string md5(const std::string& in) { MD5 m; m.update(in.data(), in.size()); return m.final(); }

// ── RC4 ─────────────────────────────────────────────────────────────────────
inline std::string rc4(const std::string& key, const std::string& data) {
    uint8_t S[256]; uint8_t K[256];
    int klen = (int)key.size(); if (klen <= 0) return data;
    for (int i = 0; i < 256; i++) { S[i] = (uint8_t)i; K[i] = (uint8_t)key[i % klen]; }
    for (int i = 0, j = 0; i < 256; i++) { j = (j + S[i] + K[i]) & 0xff; std::swap(S[i], S[j]); }
    std::string out(data.size(), '\0');
    for (size_t n = 0, i = 0, j = 0; n < data.size(); n++) {
        i = (i + 1) & 0xff; j = (j + S[i]) & 0xff; std::swap(S[i], S[j]);
        out[n] = (char)((uint8_t)data[n] ^ S[(S[i] + S[j]) & 0xff]);
    }
    return out;
}

// ── AES (128/192/256) ───────────────────────────────────────────────────────
struct AES {
    uint8_t rk[240] = {0};
    int rounds = 10;
    static uint8_t mul(uint8_t a, uint8_t b) {
        uint8_t p = 0;
        for (int i = 0; i < 8; i++) {
            if (b & 1) p ^= a;
            bool hi = a & 0x80; a = (uint8_t)(a << 1);
            if (hi) a ^= 0x1b;
            b >>= 1;
        }
        return p;
    }
    // a^254 is the multiplicative inverse in GF(2^8)
    static uint8_t inv(uint8_t a) {
        if (!a) return 0;
        uint8_t result = 1, base = a;
        int e = 254;
        while (e) {
            if (e & 1) result = mul(result, base);
            base = mul(base, base);
            e >>= 1;
        }
        return result;
    }
    static void sbox(uint8_t* out) {
        for (int i = 0; i < 256; i++) {
            uint8_t x = inv((uint8_t)i);
            out[i] = (uint8_t)(x ^ (uint8_t)((x << 1) | (x >> 7)) ^
                               (uint8_t)((x << 2) | (x >> 6)) ^
                               (uint8_t)((x << 3) | (x >> 5)) ^
                               (uint8_t)((x << 4) | (x >> 4)) ^ 0x63);
        }
    }
    void expand(const std::string& key) {
        static uint8_t S[256]; static uint8_t Si[256]; static bool init = false;
        if (!init) { sbox(S); for (int i = 0; i < 256; i++) Si[S[i]] = (uint8_t)i; init = true; }
        int nk = (int)key.size() / 4;
        rounds = nk + 6;
        memcpy(rk, key.data(), key.size());
        uint8_t rcon = 1;
        for (int i = nk; i < 4 * (rounds + 1); i++) {
            uint8_t t[4];
            memcpy(t, rk + (i - 1) * 4, 4);
            if (i % nk == 0) {
                uint8_t tmp = t[0];
                t[0] = (uint8_t)(S[t[1]] ^ rcon); t[1] = S[t[2]]; t[2] = S[t[3]]; t[3] = S[tmp];
                rcon = mul(rcon, 2);
            } else if (nk > 6 && i % nk == 4) {
                for (int k = 0; k < 4; k++) t[k] = S[t[k]];
            }
            for (int k = 0; k < 4; k++) rk[i*4 + k] = (uint8_t)(rk[(i - nk)*4 + k] ^ t[k]);
        }
    }
    void decryptBlock(const uint8_t* in, uint8_t* out) const {
        static uint8_t S[256]; static uint8_t Si[256]; static bool init = false;
        if (!init) { sbox(S); for (int i = 0; i < 256; i++) Si[S[i]] = (uint8_t)i; init = true; }
        uint8_t s[16];
        for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ rk[rounds*16 + i]);
        for (int round = rounds - 1; round >= 0; round--) {
            // InvShiftRows
            uint8_t t[16];
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                    t[r + 4*((c + r) & 3)] = s[r + 4*c];
            // InvSubBytes
            for (int i = 0; i < 16; i++) t[i] = Si[t[i]];
            // AddRoundKey
            for (int i = 0; i < 16; i++) t[i] ^= rk[round*16 + i];
            if (round) {
                // InvMixColumns
                for (int c = 0; c < 4; c++) {
                    uint8_t* col = t + 4*c;
                    uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                    auto g = [](uint8_t a, uint8_t b) { return mul(a, b); };
                    col[0] = (uint8_t)(g(a0,14) ^ g(a1,11) ^ g(a2,13) ^ g(a3,9));
                    col[1] = (uint8_t)(g(a0,9)  ^ g(a1,14) ^ g(a2,11) ^ g(a3,13));
                    col[2] = (uint8_t)(g(a0,13) ^ g(a1,9)  ^ g(a2,14) ^ g(a3,11));
                    col[3] = (uint8_t)(g(a0,11) ^ g(a1,13) ^ g(a2,9)  ^ g(a3,14));
                }
            }
            memcpy(s, t, 16);
        }
        memcpy(out, s, 16);
    }
};
inline std::string aesCbcDecrypt(const std::string& key, const std::string& data, bool stripIV = true) {
    if (data.size() < 16) return std::string();
    AES aes; aes.expand(key);
    size_t off = stripIV ? 16 : 0;
    std::string iv = stripIV ? data.substr(0, 16) : std::string(16, '\0');
    std::string out;
    uint8_t prev[16]; memcpy(prev, iv.data(), 16);
    for (size_t p = off; p + 16 <= data.size(); p += 16) {
        uint8_t blk[16]; aes.decryptBlock((const uint8_t*)data.data() + p, blk);
        for (int i = 0; i < 16; i++) blk[i] ^= prev[i];
        out.append((char*)blk, 16);
        memcpy(prev, data.data() + p, 16);
    }
    // PKCS#7 unpad
    if (!out.empty()) {
        uint8_t pad = (uint8_t)out.back();
        if (pad >= 1 && pad <= 16 && pad <= out.size()) out.resize(out.size() - pad);
    }
    return out;
}

}  // namespace pdfcrypto
