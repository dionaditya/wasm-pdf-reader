#include "iccp.hpp"

#include <cmath>
#include <cstring>

namespace pdf {
namespace {

static inline uint32_t u32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline uint16_t u16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline double s15f16(const uint8_t* p) { return (int32_t)u32(p) / 65536.0; }

static inline double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

// D50 XYZ -> sRGB (the Bradford adaptation from D50 to D65 is folded in).
static const double kXyzToSrgb[3][3] = {
    {3.1338561, -1.6168667, -0.4906146},
    {-0.9787684, 1.9161415, 0.0334540},
    {0.0719453, -0.2289914, 1.4052427},
};

static inline double srgbEncode(double v) {
    v = clamp01(v);
    return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}

static void xyzD50ToRgb(double X, double Y, double Z, float* rgb) {
    double r = kXyzToSrgb[0][0] * X + kXyzToSrgb[0][1] * Y + kXyzToSrgb[0][2] * Z;
    double g = kXyzToSrgb[1][0] * X + kXyzToSrgb[1][1] * Y + kXyzToSrgb[1][2] * Z;
    double b = kXyzToSrgb[2][0] * X + kXyzToSrgb[2][1] * Y + kXyzToSrgb[2][2] * Z;
    rgb[0] = (float)srgbEncode(r);
    rgb[1] = (float)srgbEncode(g);
    rgb[2] = (float)srgbEncode(b);
}

static void labD50ToRgb(double L, double a, double b, float* rgb) {
    double fy = (L + 16.0) / 116.0;
    double fx = fy + a / 500.0;
    double fz = fy - b / 200.0;
    auto finv = [](double t) {
        const double d = 6.0 / 29.0;
        return t > d ? t * t * t : 3.0 * d * d * (t - 4.0 / 29.0);
    };
    // D50 white point
    double X = 0.9642 * finv(fx), Y = 1.0 * finv(fy), Z = 0.8249 * finv(fz);
    xyzD50ToRgb(X, Y, Z, rgb);
}

static double evalCurve(const IccModel::Curve& c, double v) {
    v = clamp01(v);
    switch (c.type) {
        case 1: return clamp01(std::pow(v, c.gamma));
        case 2: {
            if (c.table.empty()) return v;
            double x = v * (double)(c.table.size() - 1);
            size_t i = (size_t)x;
            if (i >= c.table.size() - 1) return c.table.back();
            double f = x - (double)i;
            return c.table[i] * (1.0 - f) + c.table[i + 1] * f;
        }
        case 3: {
            const double* p = c.p;
            switch (c.paraType) {
                case 0: return clamp01(std::pow(v, p[0]));
                case 1: return clamp01(v >= -p[2] / p[1] ? std::pow(p[0] * v + p[1], p[3]) : 0.0);
                case 2: return clamp01(v >= -p[1] / p[0] ? std::pow(p[0] * v + p[1], p[2]) + p[3] : p[3]);
                case 3: return clamp01(v >= p[1] ? std::pow(p[0] * v + p[1], p[2]) : 0.0);
                case 4: return clamp01(v >= p[1] ? std::pow(p[0] * v + p[1], p[2]) + p[3] : p[3]);
                default: return v;
            }
        }
        default: return v;
    }
}

}  // namespace

bool iccParse(const std::string& bytes, IccModel& out) {
    if (bytes.size() < 132) return false;
    const uint8_t* d = (const uint8_t*)bytes.data();
    if (std::memcmp(d + 36, "acsp", 4) != 0) return false;

    std::string space((const char*)d + 16, 4);   // 'RGB ', 'CMYK', 'GRAY'
    std::string pcs((const char*)d + 20, 4);     // 'XYZ ' or 'Lab '
    out.n = space == "CMYK" ? 4 : (space == "GRAY" ? 1 : 3);
    out.pcsLab = pcs == "Lab ";

    uint32_t count = u32(d + 128);
    if (count > 200 || 132 + count * 12 > bytes.size()) return false;

    struct Tag { uint32_t off, size; };
    Tag rXYZ{}, gXYZ{}, bXYZ{}, wpt{}, rTRC{}, gTRC{}, bTRC{}, kTRC{}, a2b{}, gray{};
    auto findTag = [&](const char* sig, Tag& t) {
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t* e = d + 132 + i * 12;
            if (std::memcmp(e, sig, 4) == 0) {
                t.off = u32(e + 4);
                t.size = u32(e + 8);
                return t.off + t.size <= bytes.size();
            }
        }
        return false;
    };

    auto readCurve = [&](const Tag& t, IccModel::Curve& c) {
        if (!t.off || !t.size) return;
        const uint8_t* p = d + t.off;
        if (std::memcmp(p, "curv", 4) == 0) {
            uint32_t cnt = u32(p + 8);
            if (cnt == 0) { c.type = 0; return; }
            if (cnt == 1) { c.type = 1; c.gamma = u16(p + 12) / 256.0; return; }
            c.type = 2;
            c.table.resize(cnt);
            for (uint32_t i = 0; i < cnt; i++) c.table[i] = u16(p + 12 + i * 2) / 65535.0;
        } else if (std::memcmp(p, "para", 4) == 0) {
            c.type = 3;
            c.paraType = u16(p + 8);
            int nParams = c.paraType == 0 ? 1 : (c.paraType == 1 ? 3 : (c.paraType == 2 ? 4 : (c.paraType == 3 ? 5 : 7)));
            for (int i = 0; i < nParams && i < 7; i++) c.p[i] = s15f16(p + 12 + i * 4);
        }
    };

    // A2B0 wins when present: a CMYK profile has no usable matrix/TRC chain.
    if (findTag("A2B0", a2b)) {
        const uint8_t* p = d + a2b.off;
        int nIn = p[8], nOut = p[9], grid = p[10];
        if (std::memcmp(p, "mft2", 4) == 0) {
            int inEnt = u16(p + 48), outEnt = u16(p + 50);
            const uint8_t* q = p + 52;
            out.kind = IccModel::LUT;
            out.gridPoints = grid;
            out.nOut = nOut;
            out.inEntries = inEnt;
            out.outEntries = outEnt;
            out.n = nIn;
            out.inTable.resize((size_t)nIn * inEnt);
            for (size_t i = 0; i < out.inTable.size(); i++) out.inTable[i] = u16(q + i * 2) / 65535.0;
            q += (size_t)nIn * inEnt * 2;
            size_t cells = 1;
            for (int i = 0; i < nIn; i++) cells *= (size_t)grid;
            out.clut.resize(cells * nOut);
            for (size_t i = 0; i < out.clut.size(); i++) out.clut[i] = u16(q + i * 2) / 65535.0;
            q += cells * nOut * 2;
            out.outTable.resize((size_t)nOut * outEnt);
            for (size_t i = 0; i < out.outTable.size(); i++) out.outTable[i] = u16(q + i * 2) / 65535.0;
            return true;
        }
        if (std::memcmp(p, "mft1", 4) == 0) {
            const uint8_t* q = p + 48;
            out.kind = IccModel::LUT;
            out.gridPoints = grid;
            out.nOut = nOut;
            out.inEntries = out.outEntries = 256;
            out.n = nIn;
            out.inTable.resize((size_t)nIn * 256);
            for (size_t i = 0; i < out.inTable.size(); i++) out.inTable[i] = q[i] / 255.0;
            q += (size_t)nIn * 256;
            size_t cells = 1;
            for (int i = 0; i < nIn; i++) cells *= (size_t)grid;
            out.clut.resize(cells * nOut);
            for (size_t i = 0; i < out.clut.size(); i++) out.clut[i] = q[i] / 255.0;
            q += cells * nOut;
            out.outTable.resize((size_t)nOut * 256);
            for (size_t i = 0; i < out.outTable.size(); i++) out.outTable[i] = q[i] / 255.0;
            return true;
        }
        return false;
    }

    if (out.n == 1 && findTag("kTRC", kTRC)) {
        readCurve(kTRC, out.trc[0]);
        out.kind = IccModel::MATRIX_TRC;
        out.matrix[0][0] = out.matrix[1][1] = out.matrix[2][2] = 1.0;
        if (findTag("wtpt", wpt)) {
            double X = s15f16(d + wpt.off + 8), Y = s15f16(d + wpt.off + 12), Z = s15f16(d + wpt.off + 16);
            if (Y > 1e-6) {
                out.matrix[0][0] = X / Y;
                out.matrix[1][1] = 1.0;
                out.matrix[2][2] = Z / Y;
            }
        }
        return true;
    }

    if (findTag("rXYZ", rXYZ) && findTag("gXYZ", gXYZ) && findTag("bXYZ", bXYZ)) {
        const double* cols[3] = {nullptr, nullptr, nullptr};
        double cx[3], cy[3], cz[3];
        const Tag* tags[3] = {&rXYZ, &gXYZ, &bXYZ};
        for (int c = 0; c < 3; c++) {
            cx[c] = s15f16(d + tags[c]->off + 8);
            cy[c] = s15f16(d + tags[c]->off + 12);
            cz[c] = s15f16(d + tags[c]->off + 16);
            (void)cols;
        }
        for (int r = 0; r < 3; r++) {
            out.matrix[r][0] = r == 0 ? cx[0] : (r == 1 ? cy[0] : cz[0]);
            out.matrix[r][1] = r == 0 ? cx[1] : (r == 1 ? cy[1] : cz[1]);
            out.matrix[r][2] = r == 0 ? cx[2] : (r == 1 ? cy[2] : cz[2]);
        }
        if (!findTag("rTRC", rTRC)) findTag("kTRC", rTRC);
        if (!findTag("gTRC", gTRC)) findTag("kTRC", gTRC);
        if (!findTag("bTRC", bTRC)) findTag("kTRC", bTRC);
        readCurve(rTRC, out.trc[0]);
        readCurve(gTRC, out.trc[1]);
        readCurve(bTRC, out.trc[2]);
        out.kind = IccModel::MATRIX_TRC;
        return true;
    }
    (void)gray;
    return false;
}

void iccToRgb(const IccModel& m, const float* in, float* rgb) {
    if (m.kind == IccModel::MATRIX_TRC) {
        double lin[3] = {0, 0, 0};
        if (m.n == 1) {
            double g = evalCurve(m.trc[0], in[0]);
            lin[0] = g * m.matrix[0][0];
            lin[1] = g * m.matrix[1][1];
            lin[2] = g * m.matrix[2][2];
        } else {
            for (int c = 0; c < 3; c++) lin[c] = evalCurve(m.trc[c], in[c]);
        }
        double X, Y, Z;
        if (m.n == 1) {
            X = lin[0]; Y = lin[1]; Z = lin[2];
        } else {
            X = m.matrix[0][0] * lin[0] + m.matrix[0][1] * lin[1] + m.matrix[0][2] * lin[2];
            Y = m.matrix[1][0] * lin[0] + m.matrix[1][1] * lin[1] + m.matrix[1][2] * lin[2];
            Z = m.matrix[2][0] * lin[0] + m.matrix[2][1] * lin[1] + m.matrix[2][2] * lin[2];
        }
        xyzD50ToRgb(X, Y, Z, rgb);
        return;
    }
    if (m.kind == IccModel::LUT) {
        int n = m.n, grid = m.gridPoints;
        if (grid < 2) { rgb[0] = rgb[1] = rgb[2] = 0.f; return; }
        // input curves
        double t[4] = {0, 0, 0, 0};
        for (int c = 0; c < n && c < 4; c++) {
            double v = clamp01(in[c]) * (m.inEntries - 1);
            int i = (int)v;
            if (i >= m.inEntries - 1) t[c] = m.inTable[(size_t)c * m.inEntries + m.inEntries - 1];
            else {
                double f = v - i;
                double a = m.inTable[(size_t)c * m.inEntries + i];
                double b = m.inTable[(size_t)c * m.inEntries + i + 1];
                t[c] = a + (b - a) * f;
            }
        }
        // n-dimensional linear interpolation over the CLUT
        double pos[4];
        int i0[4], i1[4];
        double frac[4];
        for (int c = 0; c < n; c++) {
            double v = clamp01(t[c]) * (grid - 1);
            int i = (int)v;
            if (i >= grid - 1) i = grid - 2;
            i0[c] = i;
            i1[c] = i + 1;
            frac[c] = v - i;
            pos[c] = v;
        }
        double outv[4] = {0, 0, 0, 0};
        int combos = 1 << n;
        for (int mask = 0; mask < combos; mask++) {
            double w = 1.0;
            size_t idx = 0;
            for (int c = 0; c < n; c++) {
                int bit = (mask >> c) & 1;
                idx = idx * (size_t)grid + (size_t)(bit ? i1[c] : i0[c]);
                w *= bit ? frac[c] : (1.0 - frac[c]);
            }
            if (w == 0.0) continue;
            for (int o = 0; o < m.nOut; o++) outv[o] += w * m.clut[idx * m.nOut + o];
        }
        // output curves
        double o[3] = {0, 0, 0};
        for (int c = 0; c < 3 && c < m.nOut; c++) {
            double v = clamp01(outv[c]) * (m.outEntries - 1);
            int i = (int)v;
            if (i >= m.outEntries - 1) o[c] = m.outTable[(size_t)c * m.outEntries + m.outEntries - 1];
            else {
                double f = v - i;
                double a = m.outTable[(size_t)c * m.outEntries + i];
                double b = m.outTable[(size_t)c * m.outEntries + i + 1];
                o[c] = a + (b - a) * f;
            }
        }
        if (m.pcsLab) {
            labD50ToRgb(o[0] * 100.0, o[1] * 255.0 - 128.0, o[2] * 255.0 - 128.0, rgb);
        } else {
            xyzD50ToRgb(o[0], o[1], o[2], rgb);
        }
        return;
    }
    rgb[0] = rgb[1] = rgb[2] = 0.f;
}

}  // namespace pdf
