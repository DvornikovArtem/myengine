// FlyProbe2.cpp: how the lattice of the optic lobe columns (hex1, hex2) lies in the body of the fly: a fit of the 3D position of L1 against the column (manual tool)
#include <flybrain/Connectome.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    // least squares: value = c0 + c1 * x + c2 * y, returns R^2
    double Fit(const std::vector<double>& x, const std::vector<double>& y, const std::vector<double>& value, double c[3])
    {
        const size_t n = x.size();
        double sx = 0, sy = 0, sv = 0, sxx = 0, sxy = 0, syy = 0, sxv = 0, syv = 0;
        for (size_t i = 0; i < n; ++i)
        {
            sx += x[i]; sy += y[i]; sv += value[i]; sxx += x[i] * x[i]; sxy += x[i] * y[i]; syy += y[i] * y[i]; sxv += x[i] * value[i]; syv += y[i] * value[i];
        }
        // normal equations 3x3
        double a[3][4] = {{double(n), sx, sy, sv}, {sx, sxx, sxy, sxv}, {sy, sxy, syy, syv}};
        for (int i = 0; i < 3; ++i)
        {
            int p = i;
            for (int r = i + 1; r < 3; ++r) if (std::fabs(a[r][i]) > std::fabs(a[p][i])) p = r;
            for (int k = 0; k < 4; ++k) std::swap(a[i][k], a[p][k]);
            for (int r = 0; r < 3; ++r)
            {
                if (r == i) continue;
                const double f = a[r][i] / a[i][i];
                for (int k = 0; k < 4; ++k) a[r][k] -= f * a[i][k];
            }
        }
        for (int i = 0; i < 3; ++i) c[i] = a[i][3] / a[i][i];
        const double mean = sv / n;
        double ssTot = 0, ssRes = 0;
        for (size_t i = 0; i < n; ++i)
        {
            const double pred = c[0] + c[1] * x[i] + c[2] * y[i];
            ssTot += (value[i] - mean) * (value[i] - mean);
            ssRes += (value[i] - pred) * (value[i] - pred);
        }
        return 1.0 - ssRes / ssTot;
    }
}

int main(int argc, char** argv)
{
    std::string error;
    auto c = flybrain::Connectome::Load(argc > 1 ? argv[1] : "assets/flybrain/malecns_v1.fbrain", &error);
    if (!c)
    {
        std::printf("%s\n", error.c_str());
        return 1;
    }
    for (const char* type : {"L1", "L2", "L3", "Mi1", "Tm3", "T4a"})
    {
        for (flybrain::Side side : {flybrain::Side::Left, flybrain::Side::Right})
        {
            std::vector<double> xh, yh, px, py, pz;
            size_t withHex = 0, zeros = 0;
            for (uint32_t n : c->Find(type, side).neurons)
            {
                if (c->Hex1(n) == flybrain::kNoHex) continue;
                ++withHex;
                const float* p = c->Position(n);
                if (p[0] == 0.0f && p[1] == 0.0f && p[2] == 0.0f) { ++zeros; continue; }
                // the axial lattice to the plane: x = hex2 - hex1 / 2, y = hex1 * sqrt(3) / 2
                xh.push_back(c->Hex2(n) - 0.5 * c->Hex1(n));
                yh.push_back(c->Hex1(n) * 0.8660254);
                px.push_back(p[0]); py.push_back(p[1]); pz.push_back(p[2]);
            }
            if (xh.size() < 20) { std::printf("%s %s: with hex %zu, zero positions %zu, too few\n", type, flybrain::ToString(side), withHex, zeros); continue; }
            double cx[3], cy[3], cz[3];
            const double rx = Fit(xh, yh, px, cx), ry = Fit(xh, yh, py, cy), rz = Fit(xh, yh, pz, cz);
            std::printf("%s %s: hex %zu, zero pos %zu, used %zu\n", type, flybrain::ToString(side), withHex, zeros, xh.size());
            std::printf("    pos.x = %.0f %+.0f*hx %+.0f*hy  R2 %.2f\n", cx[0], cx[1], cx[2], rx);
            std::printf("    pos.y = %.0f %+.0f*hx %+.0f*hy  R2 %.2f   (y: dorsal -> ventral)\n", cy[0], cy[1], cy[2], ry);
            std::printf("    pos.z = %.0f %+.0f*hx %+.0f*hy  R2 %.2f   (z: anterior -> posterior)\n", cz[0], cz[1], cz[2], rz);
        }
    }
    return 0;
}
