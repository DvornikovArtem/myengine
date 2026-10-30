// FlyBrainCircuitTool.cpp
// N2 (FlyDoom): cuts the circuit of the swarm flies out of MaleCNS and saves it (docs/flybrain/flybrain.md, section "Swarm").
//
//   myengine_flybrain_circuit [--fbrain <malecns_v1.fbrain>] [--out <swarm_circuit.fbrain>] [--up-hops 3] [--up-share 0.0125]
//                             [--down-hops 3] [--down-share 0.0125] [--dry-run]

#include <flybrain/Circuit.h>
#include <flybrain/Connectome.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

int main(int argc, char** argv)
{
    using namespace flybrain;
    std::string source = std::string(MYENGINE_SOURCE_DIR) + "/assets/flybrain/malecns_v1.fbrain";
    std::string out = std::string(MYENGINE_SOURCE_DIR) + "/assets/flybrain/swarm_circuit.fbrain";
    CircuitSpec spec;
    bool dryRun = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (arg == "--fbrain") source = next();
        else if (arg == "--out") out = next();
        else if (arg == "--up-hops") spec.upstreamHops = static_cast<uint32_t>(std::stoul(next()));
        else if (arg == "--up-share") spec.upstreamShare = std::stod(next());
        else if (arg == "--down-hops") spec.downstreamHops = static_cast<uint32_t>(std::stoul(next()));
        else if (arg == "--down-share") spec.downstreamShare = std::stod(next());
        else if (arg == "--dry-run") dryRun = true;
        else
        {
            std::fprintf(stderr, "unknown argument %s\n", arg.c_str());
            return 2;
        }
    }

    std::string error;
    const auto connectome = Connectome::Load(source, &error);
    if (connectome == nullptr)
    {
        std::fprintf(stderr, "cannot load %s: %s (tools/flybrain/fetch_malecns.ps1)\n", source.c_str(), error.c_str());
        return 77;
    }
    CircuitReport report;
    const std::vector<std::byte> image = ExtractCircuitImage(*connectome, spec, &report, &error);
    if (image.empty())
    {
        std::fprintf(stderr, "cannot cut the circuit: %s\n", error.c_str());
        return 1;
    }
    std::printf("%s\n", report.Text(40).c_str());
    if (dryRun)
    {
        return 0;
    }
    std::ofstream file(std::filesystem::u8path(out), std::ios::binary | std::ios::trunc);
    if (!file || !file.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size())))
    {
        std::fprintf(stderr, "cannot write %s\n", out.c_str());
        return 1;
    }
    std::printf("wrote %s (%.1f KB)\n", out.c_str(), image.size() / 1024.0);
    return 0;
}
