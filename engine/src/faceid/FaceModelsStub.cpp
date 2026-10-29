// FaceModelsStub.cpp: what the editor gets without ONNX Runtime (MYENGINE_FACEID is OFF): Face ID is not available, everything else works as before.

#include <myengine/faceid/FaceModels.h>

namespace myengine::faceid
{
    struct FaceModels::Impl
    {
    };

    FaceModels::FaceModels() = default;
    FaceModels::~FaceModels() = default;

    bool FaceModels::Available() { return false; }

    std::filesystem::path FaceModels::DefaultDirectory() { return {}; }

    bool FaceModels::Load(const std::filesystem::path&, std::string& error)
    {
        error = "this build has no ONNX Runtime (CMake option MYENGINE_FACEID is OFF): run tools/faceid/fetch_models.ps1 and configure again";
        return false;
    }

    bool FaceModels::Loaded() const { return false; }

    std::vector<FaceDetection> FaceModels::Detect(const FaceImage&, float, float) { return {}; }

    bool FaceModels::Embed(const FaceImage&, const FaceDetection&, FaceEmbedding&) { return false; }
}
