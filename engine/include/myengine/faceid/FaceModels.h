// FaceModels.h
// The two networks of Face ID on ONNX Runtime (CPU): YuNet finds faces and their five points (face_detection_yunet_2023mar.onnx, MIT), SFace turns an aligned face into an
// embedding of 128 numbers (face_recognition_sface_2021dec.onnx, Apache 2.0); both come from OpenCV Zoo (tools/faceid/fetch_models.ps1). OpenCV itself is not used. Without
// MYENGINE_FACEID the class is a stub: Available() is false and Load() says why.

#pragma once

#include <myengine/faceid/FaceTypes.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace myengine::faceid
{
    class FaceModels
    {
    public:
        FaceModels();
        ~FaceModels();
        FaceModels(const FaceModels&) = delete;
        FaceModels& operator=(const FaceModels&) = delete;

        // true when the build has ONNX Runtime
        static bool Available();
        // <the folder of the exe>/assets/faceid/models (the build copies assets/ there)
        static std::filesystem::path DefaultDirectory();

        // Reads both models from the folder; false and `error` when ONNX Runtime is not built in or a file is missing or wrong
        bool Load(const std::filesystem::path& modelsDirectory, std::string& error);
        bool Loaded() const;

        // The faces of an image, the best first. YuNet takes 640 x 640: a larger or smaller image is scaled in (one scale, the empty part is black);
        // the boxes and the points come back in the pixels of the given image.
        std::vector<FaceDetection> Detect(const FaceImage& image, float scoreThreshold = 0.7f, float nmsThreshold = 0.3f);
        // The embedding (normalised) of a face found in `image`
        bool Embed(const FaceImage& image, const FaceDetection& face, FaceEmbedding& out);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
