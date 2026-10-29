#include <myengine/faceid/FaceAlign.h>

#include <algorithm>
#include <cmath>

namespace myengine::faceid
{
    const float kAlignTemplate[10] = {38.2946f, 51.6963f, 73.5318f, 51.5014f, 56.0252f, 71.7366f, 41.5493f, 92.3655f, 70.7299f, 92.2041f};

    double Similarity::Scale() const { return std::hypot(a, b); }

    bool EstimateSimilarity(const float from[10], const float to[10], Similarity& out)
    {
        double meanFromX = 0.0;
        double meanFromY = 0.0;
        double meanToX = 0.0;
        double meanToY = 0.0;
        for (int i = 0; i < 5; ++i)
        {
            meanFromX += from[2 * i];
            meanFromY += from[2 * i + 1];
            meanToX += to[2 * i];
            meanToY += to[2 * i + 1];
        }
        meanFromX /= 5.0;
        meanFromY /= 5.0;
        meanToX /= 5.0;
        meanToY /= 5.0;
        double dot = 0.0;
        double cross = 0.0;
        double variance = 0.0;
        for (int i = 0; i < 5; ++i)
        {
            const double sx = from[2 * i] - meanFromX;
            const double sy = from[2 * i + 1] - meanFromY;
            const double dx = to[2 * i] - meanToX;
            const double dy = to[2 * i + 1] - meanToY;
            dot += sx * dx + sy * dy;
            cross += sx * dy - sy * dx;
            variance += sx * sx + sy * sy;
        }
        if (variance < 1e-9)
        {
            return false;
        }
        out.a = dot / variance;
        out.b = cross / variance;
        out.tx = meanToX - (out.a * meanFromX - out.b * meanFromY);
        out.ty = meanToY - (out.b * meanFromX + out.a * meanFromY);
        return true;
    }

    bool AlignFace(const FaceImage& image, const float landmarks[10], FaceImage& out)
    {
        if (image.Empty())
        {
            return false;
        }
        Similarity forward;
        if (!EstimateSimilarity(landmarks, kAlignTemplate, forward))
        {
            return false;
        }
        const double norm = forward.a * forward.a + forward.b * forward.b;
        if (norm < 1e-12)
        {
            return false;
        }
        // the inverse: from a pixel of the aligned picture back to the image
        const double ia = forward.a / norm;
        const double ib = -forward.b / norm;
        out.width = kAlignedSize;
        out.height = kAlignedSize;
        out.rgb.assign(static_cast<std::size_t>(kAlignedSize) * kAlignedSize * 3u, 0);
        const int w = image.width;
        const int h = image.height;
        for (int y = 0; y < kAlignedSize; ++y)
        {
            for (int x = 0; x < kAlignedSize; ++x)
            {
                const double px = static_cast<double>(x) - forward.tx;
                const double py = static_cast<double>(y) - forward.ty;
                const double sx = ia * px - ib * py;
                const double sy = ib * px + ia * py;
                const double fx0 = std::floor(sx);
                const double fy0 = std::floor(sy);
                const double ux = sx - fx0;
                const double uy = sy - fy0;
                const int x0 = static_cast<int>(fx0);
                const int y0 = static_cast<int>(fy0);
                const int xs[2] = {std::clamp(x0, 0, w - 1), std::clamp(x0 + 1, 0, w - 1)};
                const int ys[2] = {std::clamp(y0, 0, h - 1), std::clamp(y0 + 1, 0, h - 1)};
                const double weights[4] = {(1.0 - ux) * (1.0 - uy), ux * (1.0 - uy), (1.0 - ux) * uy, ux * uy};
                for (int c = 0; c < 3; ++c)
                {
                    const double value = weights[0] * image.rgb[(static_cast<std::size_t>(ys[0]) * w + xs[0]) * 3u + c] +
                                         weights[1] * image.rgb[(static_cast<std::size_t>(ys[0]) * w + xs[1]) * 3u + c] +
                                         weights[2] * image.rgb[(static_cast<std::size_t>(ys[1]) * w + xs[0]) * 3u + c] +
                                         weights[3] * image.rgb[(static_cast<std::size_t>(ys[1]) * w + xs[1]) * 3u + c];
                    out.rgb[(static_cast<std::size_t>(y) * kAlignedSize + x) * 3u + c] = static_cast<std::uint8_t>(std::clamp(std::lround(value), 0l, 255l));
                }
            }
        }
        return true;
    }

    float Sharpness(const FaceImage& image)
    {
        if (image.Empty() || image.width < 3 || image.height < 3)
        {
            return 0.0f;
        }
        const int w = image.width;
        const int h = image.height;
        std::vector<float> grey(static_cast<std::size_t>(w) * h);
        for (std::size_t i = 0; i < grey.size(); ++i)
        {
            grey[i] = 0.299f * image.rgb[i * 3u] + 0.587f * image.rgb[i * 3u + 1] + 0.114f * image.rgb[i * 3u + 2];
        }
        double sum = 0.0;
        double sumSquares = 0.0;
        std::size_t count = 0;
        for (int y = 1; y < h - 1; ++y)
        {
            for (int x = 1; x < w - 1; ++x)
            {
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                const double laplacian = 4.0 * grey[i] - grey[i - 1] - grey[i + 1] - grey[i - w] - grey[i + w];
                sum += laplacian;
                sumSquares += laplacian * laplacian;
                ++count;
            }
        }
        const double mean = sum / static_cast<double>(count);
        return static_cast<float>(sumSquares / static_cast<double>(count) - mean * mean);
    }
}
