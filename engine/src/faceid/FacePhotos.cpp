#include <myengine/faceid/FaceAlign.h>
#include <myengine/faceid/FaceMatcher.h>
#include <myengine/faceid/FacePhotos.h>

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>

namespace myengine::faceid
{
    namespace
    {
        constexpr std::uintmax_t kMaxPixels = 120u * 1000u * 1000u;

        const FaceDetection* Largest(const std::vector<FaceDetection>& faces)
        {
            const FaceDetection* best = nullptr;
            for (const FaceDetection& face : faces)
            {
                if (best == nullptr || face.w * face.h > best->w * best->h)
                {
                    best = &face;
                }
            }
            return best;
        }

        // The eyes are above the nose and the nose above the mouth, and the eyes are about level
        bool Upright(const FaceDetection& face)
        {
            const float eyeY = std::max(face.landmarks[1], face.landmarks[3]);
            const float mouthY = std::min(face.landmarks[7], face.landmarks[9]);
            return face.landmarks[5] > eyeY && mouthY > face.landmarks[5] && std::fabs(face.landmarks[3] - face.landmarks[1]) < 0.6f * face.EyeDistance();
        }

        // The face as it is in the picture turned left to right: the box and the points move, and the first of the pair (the left one in the picture) is now the other eye
        FaceDetection Mirrored(const FaceDetection& face, const int imageWidth)
        {
            FaceDetection result = face;
            const float last = static_cast<float>(imageWidth);
            result.x = last - (face.x + face.w);
            const int source[5] = {1, 0, 2, 4, 3};
            for (int i = 0; i < 5; ++i)
            {
                result.landmarks[2 * i] = last - 1.0f - face.landmarks[2 * source[i]];
                result.landmarks[2 * i + 1] = face.landmarks[2 * source[i] + 1];
            }
            return result;
        }

        std::string Pixels(const float value) { return std::to_string(static_cast<int>(std::lround(value))) + " px"; }
    }

    bool LoadPhoto(const std::filesystem::path& file, FaceImage& out, std::string& reason)
    {
        std::error_code error;
        const std::uintmax_t size = std::filesystem::file_size(file, error);
        if (error || size == 0)
        {
            reason = "the file cannot be read";
            return false;
        }
        if (size > kPhotoMaxBytes)
        {
            reason = "the file is too large (over 64 MB)";
            return false;
        }
        std::ifstream stream(file, std::ios::binary);
        const std::vector<char> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        if (bytes.empty())
        {
            reason = "the file cannot be read";
            return false;
        }
        const auto* data = reinterpret_cast<const stbi_uc*>(bytes.data());
        int width = 0;
        int height = 0;
        int channels = 0;
        if (!stbi_info_from_memory(data, static_cast<int>(bytes.size()), &width, &height, &channels) || width <= 0 || height <= 0)
        {
            reason = "it is not a jpg or png picture";
            return false;
        }
        if (static_cast<std::uintmax_t>(width) * static_cast<std::uintmax_t>(height) > kMaxPixels)
        {
            reason = "the picture is too large (over 120 megapixels)";
            return false;
        }
        stbi_uc* pixels = stbi_load_from_memory(data, static_cast<int>(bytes.size()), &width, &height, &channels, 3);
        if (pixels == nullptr)
        {
            reason = "the picture is damaged or of a kind that cannot be read";
            return false;
        }
        out.width = width;
        out.height = height;
        out.rgb.assign(pixels, pixels + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u);
        stbi_image_free(pixels);
        return true;
    }

    FaceImage ShrinkImage(const FaceImage& image, const int factor)
    {
        if (factor <= 1 || image.Empty())
        {
            return image;
        }
        FaceImage result;
        result.width = std::max(1, image.width / factor);
        result.height = std::max(1, image.height / factor);
        result.rgb.resize(static_cast<std::size_t>(result.width) * static_cast<std::size_t>(result.height) * 3u);
        const unsigned count = static_cast<unsigned>(factor) * static_cast<unsigned>(factor);
        for (int y = 0; y < result.height; ++y)
        {
            for (int x = 0; x < result.width; ++x)
            {
                unsigned sum[3] = {0, 0, 0};
                for (int dy = 0; dy < factor; ++dy)
                {
                    const std::uint8_t* row = image.rgb.data() + (static_cast<std::size_t>(y * factor + dy) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x * factor)) * 3u;
                    for (int dx = 0; dx < factor; ++dx)
                    {
                        sum[0] += row[dx * 3];
                        sum[1] += row[dx * 3 + 1];
                        sum[2] += row[dx * 3 + 2];
                    }
                }
                std::uint8_t* target = result.rgb.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(result.width) + static_cast<std::size_t>(x)) * 3u;
                for (int c = 0; c < 3; ++c)
                {
                    target[c] = static_cast<std::uint8_t>((sum[c] + count / 2u) / count);
                }
            }
        }
        return result;
    }

    FaceImage RotateClockwise(const FaceImage& image)
    {
        FaceImage result;
        result.width = image.height;
        result.height = image.width;
        result.rgb.resize(image.rgb.size());
        for (int y = 0; y < image.height; ++y)
        {
            for (int x = 0; x < image.width; ++x)
            {
                // the pixel (x, y) goes to (height - 1 - y, x)
                const std::uint8_t* from = image.rgb.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) * 3u;
                std::uint8_t* to = result.rgb.data() + (static_cast<std::size_t>(x) * static_cast<std::size_t>(result.width) + static_cast<std::size_t>(image.height - 1 - y)) * 3u;
                to[0] = from[0];
                to[1] = from[1];
                to[2] = from[2];
            }
        }
        return result;
    }

    FaceImage MirrorImage(const FaceImage& image)
    {
        FaceImage result = image;
        for (int y = 0; y < image.height; ++y)
        {
            for (int x = 0; x < image.width; ++x)
            {
                const std::uint8_t* from = image.rgb.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) * 3u;
                std::uint8_t* to = result.rgb.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(image.width - 1 - x)) * 3u;
                to[0] = from[0];
                to[1] = from[1];
                to[2] = from[2];
            }
        }
        return result;
    }

    PhotoResult ExaminePhoto(FaceModels& models, const FaceImage& photo)
    {
        PhotoResult result;
        if (photo.Empty() || !models.Loaded())
        {
            result.reason = "the face models are not loaded";
            return result;
        }
        const int longSide = std::max(photo.width, photo.height);
        const int factor = std::max(1, (longSide + kPhotoWorkSide - 1) / kPhotoWorkSide);
        FaceImage work = ShrinkImage(photo, factor);

        // upright first; a photo that a phone took sideways has its face turned by a quarter, three quarters or a half. A "face" whose eyes are not above the nose and the
        // mouth is a face found on its side and is not used; when the upright search is not sure (a score under 0.9), the four turns compete and the surest face wins
        const auto sane = [](std::vector<FaceDetection> found)
        {
            found.erase(std::remove_if(found.begin(), found.end(), [](const FaceDetection& face) { return !Upright(face); }), found.end());
            return found;
        };
        const auto surest = [](const std::vector<FaceDetection>& found)
        {
            float best = 0.0f;
            for (const FaceDetection& face : found)
            {
                best = std::max(best, face.score);
            }
            return best;
        };
        std::vector<FaceDetection> faces = sane(models.Detect(work));
        if (surest(faces) < 0.9f)
        {
            FaceImage turned = work;
            FaceImage chosen = work;
            float chosenScore = surest(faces);
            for (int turn = 1; turn < 4; ++turn)
            {
                turned = RotateClockwise(turned);
                std::vector<FaceDetection> found = sane(models.Detect(turned));
                if (surest(found) > chosenScore)
                {
                    chosenScore = surest(found);
                    chosen = turned;
                    faces = std::move(found);
                }
            }
            work = std::move(chosen);
        }
        if (faces.empty())
        {
            result.reason = "no face found (it has to look at the camera, or turn only a little)";
            return result;
        }
        const FaceDetection& face = *Largest(faces);
        int count = 1;
        for (const FaceDetection& other : faces)
        {
            if (&other != &face && other.w * other.h > kPhotoOtherFaceShare * face.w * face.h)
            {
                ++count;
            }
        }
        if (count > 1)
        {
            result.reason = std::to_string(count) + " faces found: a photo of one person is needed";
            return result;
        }
        result.faceWidth = face.w * static_cast<float>(factor);
        if (result.faceWidth < kPhotoMinFaceWidth)
        {
            result.reason = "the face is too small (" + Pixels(result.faceWidth) + " wide, " + Pixels(kPhotoMinFaceWidth) + " needed)";
            return result;
        }
        result.yaw = face.Yaw();
        if (std::fabs(result.yaw) > kPhotoMaxYaw)
        {
            result.reason = "the head is turned too far (a straight look or a light turn is needed)";
            return result;
        }
        FaceImage aligned;
        if (!AlignFace(work, face.landmarks, aligned))
        {
            result.reason = "the face cannot be read from this photo";
            return result;
        }
        result.sharpness = Sharpness(aligned);
        if (result.sharpness < kPhotoMinSharpness)
        {
            result.reason = "the face is blurred";
            return result;
        }
        if (!models.Embed(work, face, result.embedding))
        {
            result.reason = "the face cannot be read from this photo";
            return result;
        }
        const FaceImage flipped = MirrorImage(work);
        if (!models.Embed(flipped, Mirrored(face, work.width), result.mirrored))
        {
            result.mirrored = result.embedding;
        }
        result.accepted = true;
        return result;
    }

    std::vector<std::filesystem::path> ChoosePhotoFiles()
    {
        // "dir\0name1\0name2\0\0" for several files, one full path for a single file
        std::vector<wchar_t> buffer(32768, L'\0');
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = GetActiveWindow();
        dialog.lpstrFile = buffer.data();
        dialog.nMaxFile = static_cast<DWORD>(buffer.size());
        static const wchar_t kFilter[] = L"Photos (jpg, png)\0*.jpg;*.jpeg;*.png\0All files\0*.*\0\0";
        dialog.lpstrFilter = kFilter;
        dialog.lpstrTitle = L"Choose photos of one person (3 to 10)";
        dialog.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        std::vector<std::filesystem::path> files;
        if (!GetOpenFileNameW(&dialog))
        {
            return files;
        }
        const std::wstring first(buffer.data());
        const wchar_t* next = buffer.data() + first.size() + 1;
        if (*next == L'\0')
        {
            files.emplace_back(first);
        }
        else
        {
            while (*next != L'\0')
            {
                const std::wstring name(next);
                files.emplace_back(std::filesystem::path(first) / name);
                next += name.size() + 1;
            }
        }
        return files;
    }
}
