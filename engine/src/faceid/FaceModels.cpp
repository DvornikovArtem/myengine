// FaceModels.cpp: YuNet and SFace on ONNX Runtime. The decoding of the outputs of YuNet follows the one of OpenCV (FaceDetectorYN): three strides (8, 16, 32), the score is
// sqrt(cls * obj), the box is a centre shift and a log size in strides, the five points are shifts in strides, then non-maximum suppression.

#include <myengine/faceid/FaceModels.h>

#include <myengine/faceid/FaceAlign.h>
#include <myengine/faceid/FaceMatcher.h>

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cmath>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace myengine::faceid
{
    namespace
    {
        constexpr int kDetectorSize = 640;
        constexpr int kStrides[3] = {8, 16, 32};

        // bilinear scaling of an RGB image
        FaceImage Resize(const FaceImage& source, const int width, const int height)
        {
            FaceImage out;
            out.width = width;
            out.height = height;
            out.rgb.resize(static_cast<std::size_t>(width) * height * 3u);
            const double scaleX = static_cast<double>(source.width) / width;
            const double scaleY = static_cast<double>(source.height) / height;
            for (int y = 0; y < height; ++y)
            {
                const double sy = std::max(0.0, (y + 0.5) * scaleY - 0.5);
                const int y0 = std::min(static_cast<int>(sy), source.height - 1);
                const int y1 = std::min(y0 + 1, source.height - 1);
                const double fy = sy - y0;
                for (int x = 0; x < width; ++x)
                {
                    const double sx = std::max(0.0, (x + 0.5) * scaleX - 0.5);
                    const int x0 = std::min(static_cast<int>(sx), source.width - 1);
                    const int x1 = std::min(x0 + 1, source.width - 1);
                    const double fx = sx - x0;
                    for (int c = 0; c < 3; ++c)
                    {
                        const auto at = [&](const int px, const int py) { return static_cast<double>(source.rgb[(static_cast<std::size_t>(py) * source.width + px) * 3u + c]); };
                        const double value = (1.0 - fy) * ((1.0 - fx) * at(x0, y0) + fx * at(x1, y0)) + fy * ((1.0 - fx) * at(x0, y1) + fx * at(x1, y1));
                        out.rgb[(static_cast<std::size_t>(y) * width + x) * 3u + c] = static_cast<std::uint8_t>(std::clamp(std::lround(value), 0l, 255l));
                    }
                }
            }
            return out;
        }

        float IntersectionOverUnion(const FaceDetection& a, const FaceDetection& b)
        {
            const float x1 = std::max(a.x, b.x);
            const float y1 = std::max(a.y, b.y);
            const float x2 = std::min(a.x + a.w, b.x + b.w);
            const float y2 = std::min(a.y + a.h, b.y + b.h);
            const float inter = std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
            const float uni = a.w * a.h + b.w * b.h - inter;
            return uni > 0.0f ? inter / uni : 0.0f;
        }
    }

    struct FaceModels::Impl
    {
        Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "faceid"};
        Ort::Session detector{nullptr};
        Ort::Session recognizer{nullptr};
        Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        bool loaded = false;
    };

    FaceModels::FaceModels() : impl_(std::make_unique<Impl>()) {}
    FaceModels::~FaceModels() = default;

    bool FaceModels::Available() { return true; }

    std::filesystem::path FaceModels::DefaultDirectory()
    {
        wchar_t buffer[MAX_PATH * 2]{};
        const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
        if (length == 0 || length >= std::size(buffer))
        {
            return std::filesystem::path("assets") / "faceid" / "models";
        }
        return std::filesystem::path(buffer).parent_path() / "assets" / "faceid" / "models";
    }

    bool FaceModels::Loaded() const { return impl_ != nullptr && impl_->loaded; }

    bool FaceModels::Load(const std::filesystem::path& modelsDirectory, std::string& error)
    {
        impl_->loaded = false;
        const std::filesystem::path detectorFile = modelsDirectory / "face_detection_yunet_2023mar.onnx";
        const std::filesystem::path recognizerFile = modelsDirectory / "face_recognition_sface_2021dec.onnx";
        std::error_code ec;
        if (!std::filesystem::exists(detectorFile, ec) || !std::filesystem::exists(recognizerFile, ec))
        {
            error = "the models of Face ID are not in assets/faceid/models: run tools/faceid/fetch_models.ps1";
            return false;
        }
        try
        {
            Ort::SessionOptions options;
            options.SetIntraOpNumThreads(2);
            options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            impl_->detector = Ort::Session(impl_->env, detectorFile.c_str(), options);
            impl_->recognizer = Ort::Session(impl_->env, recognizerFile.c_str(), options);
        }
        catch (const Ort::Exception& exception)
        {
            error = std::string("ONNX Runtime could not read the models: ") + exception.what();
            return false;
        }
        impl_->loaded = true;
        return true;
    }

    std::vector<FaceDetection> FaceModels::Detect(const FaceImage& image, const float scoreThreshold, const float nmsThreshold)
    {
        std::vector<FaceDetection> faces;
        if (!Loaded() || image.Empty())
        {
            return faces;
        }
        const float scale = std::min(static_cast<float>(kDetectorSize) / static_cast<float>(image.width), static_cast<float>(kDetectorSize) / static_cast<float>(image.height));
        const int scaledWidth = std::clamp(static_cast<int>(std::lround(image.width * scale)), 1, kDetectorSize);
        const int scaledHeight = std::clamp(static_cast<int>(std::lround(image.height * scale)), 1, kDetectorSize);
        const FaceImage scaled = (scaledWidth == image.width && scaledHeight == image.height) ? image : Resize(image, scaledWidth, scaledHeight);

        // BGR planes, 0..255, the picture in the top left corner of the 640 x 640 input
        std::vector<float> input(static_cast<std::size_t>(3) * kDetectorSize * kDetectorSize, 0.0f);
        const std::size_t plane = static_cast<std::size_t>(kDetectorSize) * kDetectorSize;
        for (int y = 0; y < scaledHeight; ++y)
        {
            for (int x = 0; x < scaledWidth; ++x)
            {
                const std::size_t from = (static_cast<std::size_t>(y) * scaledWidth + x) * 3u;
                const std::size_t to = static_cast<std::size_t>(y) * kDetectorSize + x;
                input[to] = scaled.rgb[from + 2];
                input[plane + to] = scaled.rgb[from + 1];
                input[2 * plane + to] = scaled.rgb[from];
            }
        }
        try
        {
            const std::array<int64_t, 4> shape = {1, 3, kDetectorSize, kDetectorSize};
            Ort::Value tensor = Ort::Value::CreateTensor<float>(impl_->memory, input.data(), input.size(), shape.data(), shape.size());
            const char* inputNames[] = {"input"};
            const char* outputNames[] = {"cls_8", "cls_16", "cls_32", "obj_8", "obj_16", "obj_32", "bbox_8", "bbox_16", "bbox_32", "kps_8", "kps_16", "kps_32"};
            std::vector<Ort::Value> outputs = impl_->detector.Run(Ort::RunOptions{nullptr}, inputNames, &tensor, 1, outputNames, 12);
            for (int s = 0; s < 3; ++s)
            {
                const int stride = kStrides[s];
                const int columns = kDetectorSize / stride;
                const float* cls = outputs[static_cast<std::size_t>(s)].GetTensorData<float>();
                const float* obj = outputs[static_cast<std::size_t>(3 + s)].GetTensorData<float>();
                const float* bbox = outputs[static_cast<std::size_t>(6 + s)].GetTensorData<float>();
                const float* kps = outputs[static_cast<std::size_t>(9 + s)].GetTensorData<float>();
                const int count = columns * columns;
                for (int i = 0; i < count; ++i)
                {
                    const float score = std::sqrt(std::clamp(cls[i], 0.0f, 1.0f) * std::clamp(obj[i], 0.0f, 1.0f));
                    if (score < scoreThreshold)
                    {
                        continue;
                    }
                    const int row = i / columns;
                    const int column = i % columns;
                    const float centreX = (static_cast<float>(column) + bbox[i * 4]) * static_cast<float>(stride);
                    const float centreY = (static_cast<float>(row) + bbox[i * 4 + 1]) * static_cast<float>(stride);
                    const float width = std::exp(bbox[i * 4 + 2]) * static_cast<float>(stride);
                    const float height = std::exp(bbox[i * 4 + 3]) * static_cast<float>(stride);
                    FaceDetection face;
                    face.score = score;
                    face.x = (centreX - width * 0.5f) / scale;
                    face.y = (centreY - height * 0.5f) / scale;
                    face.w = width / scale;
                    face.h = height / scale;
                    for (int n = 0; n < 5; ++n)
                    {
                        face.landmarks[2 * n] = (kps[i * 10 + 2 * n] + static_cast<float>(column)) * static_cast<float>(stride) / scale;
                        face.landmarks[2 * n + 1] = (kps[i * 10 + 2 * n + 1] + static_cast<float>(row)) * static_cast<float>(stride) / scale;
                    }
                    faces.push_back(face);
                }
            }
        }
        catch (const Ort::Exception&)
        {
            return {};
        }
        std::sort(faces.begin(), faces.end(), [](const FaceDetection& a, const FaceDetection& b) { return a.score > b.score; });
        std::vector<FaceDetection> kept;
        for (const FaceDetection& face : faces)
        {
            const bool overlaps = std::any_of(kept.begin(), kept.end(), [&](const FaceDetection& other) { return IntersectionOverUnion(face, other) > nmsThreshold; });
            if (!overlaps)
            {
                kept.push_back(face);
            }
        }
        return kept;
    }

    bool FaceModels::Embed(const FaceImage& image, const FaceDetection& face, FaceEmbedding& out)
    {
        if (!Loaded())
        {
            return false;
        }
        FaceImage aligned;
        if (!AlignFace(image, face.landmarks, aligned))
        {
            return false;
        }
        // RGB planes, 0..255
        std::vector<float> input(static_cast<std::size_t>(3) * kAlignedSize * kAlignedSize);
        const std::size_t plane = static_cast<std::size_t>(kAlignedSize) * kAlignedSize;
        for (std::size_t i = 0; i < plane; ++i)
        {
            input[i] = aligned.rgb[i * 3u];
            input[plane + i] = aligned.rgb[i * 3u + 1];
            input[2 * plane + i] = aligned.rgb[i * 3u + 2];
        }
        try
        {
            const std::array<int64_t, 4> shape = {1, 3, kAlignedSize, kAlignedSize};
            Ort::Value tensor = Ort::Value::CreateTensor<float>(impl_->memory, input.data(), input.size(), shape.data(), shape.size());
            const char* inputNames[] = {"data"};
            const char* outputNames[] = {"fc1"};
            std::vector<Ort::Value> outputs = impl_->recognizer.Run(Ort::RunOptions{nullptr}, inputNames, &tensor, 1, outputNames, 1);
            const float* values = outputs[0].GetTensorData<float>();
            for (int i = 0; i < kEmbeddingSize; ++i)
            {
                out[static_cast<std::size_t>(i)] = values[i];
            }
        }
        catch (const Ort::Exception&)
        {
            return false;
        }
        Normalize(out);
        return true;
    }
}
