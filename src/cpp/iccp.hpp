// Minimal ICC colour management: matrix/TRC display profiles and A2B0 CLUTs.
//
// A PDF /ICCBased stream carries a full profile, but virtually everything that
// matters for rendering falls into two shapes: an RGB monitor profile
// (rXYZ/gXYZ/bXYZ + rTRC/gTRC/bTRC) or a CMYK/spot profile with an A2B0 lookup
// table. Both are implemented here and converted to sRGB, which is what the
// canvas stores. Anything else reports false and the caller keeps its naive
// conversion, so an exotic profile degrades instead of crashing.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pdf {

struct IccModel {
    enum Kind { NONE, MATRIX_TRC, LUT } kind = NONE;
    int n = 3;                       // device components
    bool pcsLab = false;             // LUT output is Lab, not XYZ

    // matrix/TRC
    double matrix[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};   // linear device -> XYZ (D50)
    struct Curve {
        int type = 0;                // 0 identity, 1 gamma, 2 table, 3 parametric
        double gamma = 1.0;
        std::vector<double> table;   // 0..1 output, evenly spaced input
        int paraType = 0;
        double p[7] = {0, 0, 0, 0, 0, 0, 0};
    };
    Curve trc[3];

    // LUT
    int gridPoints = 0;
    int inEntries = 256, outEntries = 256;
    std::vector<double> inTable;     // n * inEntries
    std::vector<double> clut;        // gridPoints^n * nOut
    std::vector<double> outTable;    // nOut * outEntries
    int nOut = 3;
};

bool iccParse(const std::string& bytes, IccModel& out);
void iccToRgb(const IccModel& m, const float* in, float* rgb);

}  // namespace pdf
