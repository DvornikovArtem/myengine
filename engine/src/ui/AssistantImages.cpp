// AssistantImages.cpp

#include "AssistantImages.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <windows.h>
#include <objidl.h>
#include <propidl.h>
#include <shellapi.h>

// gdiplus.h wants the min / max macros that NOMINMAX removes
using std::max;
using std::min;
#include <gdiplus.h>

namespace myengine::ui::assistant_images
{
    namespace
    {
        // GDI+ is started once and kept for the life of the process
        bool EnsureGdiPlus()
        {
            static ULONG_PTR token = 0;
            static bool started = false;
            if (!started)
            {
                Gdiplus::GdiplusStartupInput input;
                started = Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
            }
            return started;
        }

        bool FindEncoder(const wchar_t* mimeType, CLSID& clsid)
        {
            UINT count = 0;
            UINT size = 0;
            if (Gdiplus::GetImageEncodersSize(&count, &size) != Gdiplus::Ok || size == 0)
            {
                return false;
            }
            std::vector<unsigned char> buffer(size);
            auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
            if (Gdiplus::GetImageEncoders(count, size, encoders) != Gdiplus::Ok)
            {
                return false;
            }
            for (UINT i = 0; i < count; ++i)
            {
                if (encoders[i].MimeType != nullptr && wcscmp(encoders[i].MimeType, mimeType) == 0)
                {
                    clsid = encoders[i].Clsid;
                    return true;
                }
            }
            return false;
        }

        std::wstring Lower(std::wstring text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](const wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
            return text;
        }

        class ClipboardGuard
        {
        public:
            ClipboardGuard()
            {
                for (int attempt = 0; attempt < 5 && !open_; ++attempt)
                {
                    open_ = OpenClipboard(nullptr) != FALSE;
                    if (!open_)
                    {
                        Sleep(10);
                    }
                }
            }
            ~ClipboardGuard()
            {
                if (open_)
                {
                    CloseClipboard();
                }
            }
            bool IsOpen() const { return open_; }

        private:
            bool open_ = false;
        };
    }

    bool ClipboardHasContent()
    {
        return IsClipboardFormatAvailable(CF_BITMAP) != FALSE || IsClipboardFormatAvailable(CF_DIB) != FALSE ||
            IsClipboardFormatAvailable(CF_HDROP) != FALSE;
    }

    std::string ImageMediaType(const std::filesystem::path& file)
    {
        const auto extension = Lower(file.extension().wstring());
        if (extension == L".png")
        {
            return "image/png";
        }
        if (extension == L".jpg" || extension == L".jpeg")
        {
            return "image/jpeg";
        }
        if (extension == L".gif")
        {
            return "image/gif";
        }
        if (extension == L".webp")
        {
            return "image/webp";
        }
        return {};
    }

    bool IsUnsupportedPicture(const std::filesystem::path& file)
    {
        const auto extension = Lower(file.extension().wstring());
        for (const wchar_t* unsupported : {L".bmp", L".tga", L".dds", L".tif", L".tiff", L".ico", L".hdr", L".psd", L".exr"})
        {
            if (extension == unsupported)
            {
                return true;
            }
        }
        return false;
    }

    std::vector<std::filesystem::path> PasteFromClipboard(const std::filesystem::path& directory, std::string& error)
    {
        std::vector<std::filesystem::path> result;
        ClipboardGuard clipboard;
        if (!clipboard.IsOpen())
        {
            error = "The clipboard is busy.";
            return result;
        }

        // Files copied in Explorer
        if (IsClipboardFormatAvailable(CF_HDROP) != FALSE)
        {
            if (const auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP)); drop != nullptr)
            {
                const UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
                for (UINT i = 0; i < count && i < 32; ++i)
                {
                    std::wstring path(static_cast<std::size_t>(DragQueryFileW(drop, i, nullptr, 0)) + 1, L'\0');
                    const UINT length = DragQueryFileW(drop, i, path.data(), static_cast<UINT>(path.size()));
                    path.resize(length);
                    result.emplace_back(path);
                }
                return result;
            }
        }

        // A picture
        if (!EnsureGdiPlus())
        {
            error = "The picture could not be read (GDI+ is unavailable).";
            return result;
        }
        const auto bitmapHandle = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP));
        if (bitmapHandle == nullptr)
        {
            return result;
        }
        std::unique_ptr<Gdiplus::Bitmap> bitmap(Gdiplus::Bitmap::FromHBITMAP(bitmapHandle, nullptr));
        CLSID png{};
        if (bitmap == nullptr || bitmap->GetLastStatus() != Gdiplus::Ok || !FindEncoder(L"image/png", png))
        {
            error = "The picture on the clipboard could not be converted.";
            return result;
        }
        std::error_code fileError;
        std::filesystem::create_directories(directory, fileError);
        const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto file = directory / (L"clipboard-" + std::to_wstring(stamp) + L".png");
        if (bitmap->Save(file.c_str(), &png, nullptr) != Gdiplus::Ok)
        {
            error = "The picture could not be saved to " + file.u8string();
            return result;
        }
        result.push_back(file);
        return result;
    }

    bool LoadThumbnail(const std::filesystem::path& file, const int maxSize, std::vector<unsigned char>& rgba, int& width, int& height)
    {
        if (!EnsureGdiPlus() || maxSize <= 0)
        {
            return false;
        }
        const auto extension = Lower(file.extension().wstring());
        if (extension != L".png" && extension != L".jpg" && extension != L".jpeg" && extension != L".gif" && extension != L".bmp")
        {
            return false;
        }
        Gdiplus::Bitmap source(file.c_str(), FALSE);
        if (source.GetLastStatus() != Gdiplus::Ok || source.GetWidth() == 0 || source.GetHeight() == 0)
        {
            return false;
        }
        const double scale = std::min(1.0, static_cast<double>(maxSize) / static_cast<double>(std::max(source.GetWidth(), source.GetHeight())));
        width = std::max(1, static_cast<int>(source.GetWidth() * scale));
        height = std::max(1, static_cast<int>(source.GetHeight() * scale));

        Gdiplus::Bitmap scaled(width, height, PixelFormat32bppARGB);
        {
            Gdiplus::Graphics graphics(&scaled);
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
            graphics.DrawImage(&source, 0, 0, width, height);
        }
        Gdiplus::BitmapData data{};
        const Gdiplus::Rect rect(0, 0, width, height);
        if (scaled.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) != Gdiplus::Ok)
        {
            return false;
        }
        rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
        for (int y = 0; y < height; ++y)
        {
            const auto* row = static_cast<const unsigned char*>(data.Scan0) + static_cast<std::ptrdiff_t>(y) * data.Stride;
            auto* out = rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
            for (int x = 0; x < width; ++x)
            {
                // GDI+ stores BGRA
                out[x * 4 + 0] = row[x * 4 + 2];
                out[x * 4 + 1] = row[x * 4 + 1];
                out[x * 4 + 2] = row[x * 4 + 0];
                out[x * 4 + 3] = row[x * 4 + 3];
            }
        }
        scaled.UnlockBits(&data);
        return true;
    }
}
