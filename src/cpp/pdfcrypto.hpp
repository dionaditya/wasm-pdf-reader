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

    void encryptBlock(const uint8_t* in, uint8_t* out) const {
        static uint8_t S[256]; static bool init = false;
        if (!init) { sbox(S); init = true; }
        uint8_t s[16];
        for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ rk[i]);
        for (int round = 1; round <= rounds; round++) {
            for (int i = 0; i < 16; i++) s[i] = S[s[i]];
            uint8_t t[16];
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                    t[r + 4*c] = s[r + 4*((c + r) & 3)];
            if (round != rounds) {
                for (int c = 0; c < 4; c++) {
                    uint8_t* col = t + 4*c;
                    uint8_t a0=col[0],a1=col[1],a2=col[2],a3=col[3];
                    col[0] = (uint8_t)(mul(a0,2)^mul(a1,3)^a2^a3);
                    col[1] = (uint8_t)(a0^mul(a1,2)^mul(a2,3)^a3);
                    col[2] = (uint8_t)(a0^a1^mul(a2,2)^mul(a3,3));
                    col[3] = (uint8_t)(mul(a0,3)^a1^a2^mul(a3,2));
                }
            }
            for (int i = 0; i < 16; i++) t[i] ^= rk[round*16 + i];
            memcpy(s, t, 16);
        }
        memcpy(out, s, 16);
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


// ── SHA-256 / SHA-384 / SHA-512 (AES-256 key derivation, R5/R6) ─────────────
struct SHA256 {
    uint32_t h[8] = {0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
                     0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    uint64_t len = 0; uint8_t blk[64]; size_t fill = 0;
    static uint32_t ror(uint32_t x, int c) { return (x >> c) | (x << (32 - c)); }
    void process(const uint8_t* p) {
        static uint32_t K[64]; static bool init = false;
        if (!init) {
            // first 32 bits of the fractional parts of the cube roots of the first 64 primes
            auto isPrime = [](int n){ if(n<2) return false; for(int d=2;d*d<=n;d++) if(n%d==0) return false; return true; };
            int idx = 0;
            for (int n = 2; idx < 64; ++n) if (isPrime(n)) {
                double cbrt = std::cbrt((double)n);
                K[idx++] = (uint32_t)((cbrt - (double)(uint32_t)cbrt) * 4294967296.0);
            }
            init = true;
        }
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) | ((uint32_t)p[i*4+2] << 8) | p[i*4+3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ror(w[i-15],7) ^ ror(w[i-15],18) ^ (w[i-15] >> 3);
            uint32_t s1 = ror(w[i-2],17) ^ ror(w[i-2],19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = ror(e,6) ^ ror(e,11) ^ ror(e,25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = ror(a,2) ^ ror(a,13) ^ ror(a,22);
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
    }
    void update(const void* data, size_t n) {
        const uint8_t* p = (const uint8_t*)data; len += n;
        while (n) {
            size_t take = 64 - fill; if (take > n) take = n;
            memcpy(blk + fill, p, take); fill += take; p += take; n -= take;
            if (fill == 64) { process(blk); fill = 0; }
        }
    }
    std::string final() {
        uint64_t bits = len * 8;
        uint8_t pad = 0x80; update(&pad, 1);
        uint8_t z = 0; while (fill != 56) update(&z, 1);
        uint8_t lb[8]; for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8*i));
        update(lb, 8);
        std::string out(32, '\0');
        for (int i = 0; i < 8; i++) for (int j = 0; j < 4; j++)
            out[i*4 + j] = (char)((h[i] >> (24 - 8*j)) & 0xff);
        return out;
    }
};
inline std::string sha256(const std::string& in) { SHA256 s; s.update(in.data(), in.size()); return s.final(); }


// ── SHA-512 / SHA-384 (R6 hardened hash uses all three) ─────────────────────
struct SHA512 {
    uint64_t h[8];
    uint64_t len = 0; uint8_t blk[128]; size_t fill = 0;
    bool is384;
    static uint64_t ror(uint64_t x, int c) { return (x >> c) | (x << (64 - c)); }
    static constexpr uint64_t Kc[80] = {
        0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
        0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
        0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
        0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
        0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
        0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
        0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
        0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
        0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
        0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
        0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
        0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
        0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
        0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
        0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
        0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
        0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
        0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
        0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
        0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
    };

    SHA512(bool sha384 = false) : is384(sha384) {
        uint64_t init384[8] = {0xcbbb9d5dc1059ed8ULL,0x629a292a367cd507ULL,0x9159015a3070dd17ULL,
                               0x152fecd8f70e5939ULL,0x67332667ffc00b31ULL,0x8eb44a8768581511ULL,
                               0xdb0c2e0d64f98fa7ULL,0x47b5481dbefa4fa4ULL};
        uint64_t init512[8] = {0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,0x3c6ef372fe94f82bULL,
                               0xa54ff53a5f1d36f1ULL,0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,
                               0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL};
        for (int i = 0; i < 8; i++) h[i] = sha384 ? init384[i] : init512[i];
    }
    void process(const uint8_t* p) {
        uint64_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = 0;
            for (int k = 0; k < 8; k++) w[i] = (w[i] << 8) | p[i*8 + k];
        }
        for (int i = 16; i < 80; i++) {
            uint64_t s0 = ror(w[i-15],1) ^ ror(w[i-15],8) ^ (w[i-15] >> 7);
            uint64_t s1 = ror(w[i-2],19) ^ ror(w[i-2],61) ^ (w[i-2] >> 6);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint64_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 80; i++) {
            uint64_t S1 = ror(e,14) ^ ror(e,18) ^ ror(e,41);
            uint64_t ch = (e & f) ^ (~e & g);
            uint64_t t1 = hh + S1 + ch + Kc[i] + w[i];
            uint64_t S0 = ror(a,28) ^ ror(a,34) ^ ror(a,39);
            uint64_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint64_t t2 = S0 + mj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
    }
    void update(const void* data, size_t n) {
        const uint8_t* p = (const uint8_t*)data; len += n;
        while (n) {
            size_t take = 128 - fill; if (take > n) take = n;
            memcpy(blk + fill, p, take); fill += take; p += take; n -= take;
            if (fill == 128) { process(blk); fill = 0; }
        }
    }
    std::string final() {
        uint64_t bits = len * 8;
        uint8_t pad = 0x80; update(&pad, 1);
        uint8_t z = 0; while (fill != 112) update(&z, 1);
        uint8_t lb[16];
        for (int i = 0; i < 8; i++) lb[i] = 0;                 // length < 2^61 bytes
        for (int i = 0; i < 8; i++) lb[8 + i] = (uint8_t)(bits >> (56 - 8*i));
        update(lb, 16);
        size_t words = is384 ? 6 : 8;
        std::string out(words * 8, '\0');
        for (size_t i = 0; i < words; i++)
            for (int j = 0; j < 8; j++) out[i*8 + j] = (char)((h[i] >> (56 - 8*j)) & 0xff);
        return out;
    }
};
inline std::string sha512(const std::string& in) { SHA512 s(false); s.update(in.data(), in.size()); return s.final(); }
inline std::string sha384(const std::string& in) { SHA512 s(true);  s.update(in.data(), in.size()); return s.final(); }

// AES-256 block encrypt is needed by the R6 hash loop; reuse the AES key schedule.
inline std::string aesCbcEncrypt(const std::string& key, const std::string& iv, const std::string& data);


inline std::string aesCbcEncrypt(const std::string& key, const std::string& iv, const std::string& data) {
    AES aes; aes.expand(key);
    std::string out; out.reserve(data.size() + 16);
    uint8_t prev[16]; memcpy(prev, iv.data(), 16);
    for (size_t p = 0; p < data.size(); p += 16) {
        uint8_t blk[16];
        for (int i = 0; i < 16; i++) blk[i] = (uint8_t)data[p + i] ^ prev[i];
        aes.encryptBlock(blk, prev);
        out.append((char*)prev, 16);
    }
    return out;
}

}  // namespace pdfcrypto
