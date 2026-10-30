// FlyProbe.cpp - exploring the connectome for the eyes of the fly player (N1). Manual tool, not a test.
#include <flybrain/Connectome.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

using namespace flybrain;

int main(int argc, char** argv)
{
    std::string error;
    auto c = Connectome::Load(argc > 1 ? argv[1] : "assets/flybrain/malecns_v1.fbrain", &error);
    if (!c) { std::printf("load: %s\n", error.c_str()); return 1; }
    std::printf("neurons %u\n", c->NeuronCount());

    std::map<std::string, uint32_t> superclasses;
    for (uint32_t i = 0; i < c->NeuronCount(); ++i) superclasses[std::string(c->Superclass(i))]++;
    for (auto& s : superclasses) std::printf("  superclass %-24s %u\n", s.first.c_str(), s.second);

    for (const char* type : {"R1-R6", "L1", "L2", "L3", "L5", "R7d", "R8d", "LPLC2", "LC4", "DNa01", "DNa02", "DNp09", "MDN", "DNp01", "MN9", "DNp20", "DNpe017", "PPL101", "LB3c"})
    {
        Group g = c->Find(type);
        int left = 0, right = 0, hex1 = 0;
        for (uint32_t n : g.neurons)
        {
            left += c->NeuronSide(n) == Side::Left;
            right += c->NeuronSide(n) == Side::Right;
            hex1 += c->Hex1(n) != kNoHex;
        }
        std::printf("type %-8s n=%zu left=%d right=%d with hex=%d\n", type, g.Size(), left, right, hex1);
        if (g.Size() <= 4)
            for (uint32_t n : g.neurons) std::printf("      body %llu side %s\n", (unsigned long long)c->BodyId(n), ToString(c->NeuronSide(n)));
    }

    // hex range of L1 per side
    for (Side side : {Side::Left, Side::Right})
    {
        Group l1 = c->Find("L1", side);
        int mn1 = 1 << 20, mx1 = -(1 << 20), mn2 = 1 << 20, mx2 = -(1 << 20);
        double cx = 0, cy = 0, cz = 0; int cnt = 0;
        for (uint32_t n : l1.neurons)
        {
            if (c->Hex1(n) == kNoHex) continue;
            mn1 = std::min<int>(mn1, c->Hex1(n)); mx1 = std::max<int>(mx1, c->Hex1(n));
            mn2 = std::min<int>(mn2, c->Hex2(n)); mx2 = std::max<int>(mx2, c->Hex2(n));
            const float* p = c->Position(n); cx += p[0]; cy += p[1]; cz += p[2]; ++cnt;
        }
        std::printf("L1 %s: hex1 %d..%d hex2 %d..%d centroid %.0f %.0f %.0f (n=%d)\n", ToString(side), mn1, mx1, mn2, mx2, cx / cnt, cy / cnt, cz / cnt, cnt);
    }
    // R1-R6 positions per side
    for (Side side : {Side::Left, Side::Right})
    {
        Group r = c->Find("R1-R6", side);
        double m[3] = {}; double lo[3] = {1e30,1e30,1e30}, hi[3] = {-1e30,-1e30,-1e30};
        for (uint32_t n : r.neurons) { const float* p = c->Position(n); for (int k = 0; k < 3; ++k) { m[k] += p[k]; lo[k] = std::min<double>(lo[k], p[k]); hi[k] = std::max<double>(hi[k], p[k]); } }
        std::printf("R1-R6 %s n=%zu mean %.0f %.0f %.0f range x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f\n", ToString(side), r.Size(), m[0]/r.Size(), m[1]/r.Size(), m[2]/r.Size(), lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
    }
    // centroids of landmarks
    auto centroid = [&](const char* prefix) {
        Group g = c->FindPrefix(prefix); double m[3] = {};
        for (uint32_t n : g.neurons) { const float* p = c->Position(n); for (int k = 0; k < 3; ++k) m[k] += p[k]; }
        if (g.Size()) std::printf("landmark %-8s n=%zu centroid %.0f %.0f %.0f\n", prefix, g.Size(), m[0]/g.Size(), m[1]/g.Size(), m[2]/g.Size());
        else std::printf("landmark %s: none\n", prefix);
    };
    for (const char* p : {"ORN", "EPG", "PEN", "PFN", "FB", "DNa02", "DNp09", "MBON", "KC", "AMMC", "JO", "ER"}) centroid(p);

    // Do R1-R6 have hex? their targets?
    {
        Group r = c->Find("R1-R6");
        std::map<std::string, uint32_t> targetTypes;
        for (size_t k = 0; k < std::min<size_t>(r.Size(), 200); ++k)
            for (uint32_t e = c->EdgeBegin(r.neurons[k]); e < c->EdgeEnd(r.neurons[k]); ++e)
                targetTypes[std::string(c->Type(c->Targets()[e]))] += 1;
        std::vector<std::pair<uint32_t, std::string>> v;
        for (auto& t : targetTypes) v.push_back({t.second, t.first});
        std::sort(v.rbegin(), v.rend());
        for (size_t i = 0; i < std::min<size_t>(v.size(), 10); ++i) std::printf("  R1-R6 target %-12s %u\n", v[i].second.c_str(), v[i].first);
    }
    return 0;
}
