// FaceTypes.h
// The types of Face ID (docs/editor/faceid.md): an image, a detected face and the embedding of a face. Nothing here knows the camera, the models or the screens.

#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace myengine::faceid
{
    constexpr int kEmbeddingSize = 128;   // SFace
    constexpr int kAlignedSize = 112;     // the side of the aligned face that SFace takes

    using FaceEmbedding = std::array<float, kEmbeddingSize>;

    // RGB, 8 bits per channel, row by row from the top, no padding
    struct FaceImage
    {
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> rgb;

        bool Empty() const { return width <= 0 || height <= 0 || rgb.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u; }
    };

    // A face as YuNet finds it: the box and five points in the pixels of the image (the person's right eye comes first and is the one on the left of the picture)
    struct FaceDetection
    {
        float score = 0.0f;
        float x = 0.0f;
        float y = 0.0f;
        float w = 0.0f;
        float h = 0.0f;
        float landmarks[10] = {};   // right eye, left eye, nose, right corner of the mouth, left corner of the mouth: x, y each

        float EyeDistance() const { return std::hypot(landmarks[2] - landmarks[0], landmarks[3] - landmarks[1]); }
        // How far the nose is from the middle between the eyes, in eye distances (about 0 for a face that looks at the camera, grows when the head turns so that
        // the nose goes to the right of the picture)
        float Yaw() const
        {
            const float eyes = EyeDistance();
            return eyes > 1e-3f ? (landmarks[4] - 0.5f * (landmarks[0] + landmarks[2])) / eyes : 0.0f;
        }
    };
}
