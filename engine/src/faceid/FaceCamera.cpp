#include <myengine/faceid/FaceCamera.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

namespace myengine::faceid
{
    namespace
    {
        using Microsoft::WRL::ComPtr;
        constexpr DWORD kVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

        std::string Utf8(const std::wstring& text)
        {
            if (text.empty())
            {
                return {};
            }
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            std::string result(static_cast<std::size_t>(std::max(bytes, 0)), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), bytes, nullptr, nullptr);
            return result;
        }

        std::string HexText(const HRESULT hr)
        {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
            return buffer;
        }

        // COM and Media Foundation for the scope of a call or a thread
        class MediaScope
        {
        public:
            MediaScope()
            {
                const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                comInitialised_ = SUCCEEDED(com);   // RPC_E_CHANGED_MODE: the thread has its apartment already, it is not ours to close
                ok_ = SUCCEEDED(MFStartup(MF_VERSION));
            }
            ~MediaScope()
            {
                if (ok_)
                {
                    MFShutdown();
                }
                if (comInitialised_)
                {
                    CoUninitialize();
                }
            }
            bool Ok() const { return ok_; }

        private:
            bool comInitialised_ = false;
            bool ok_ = false;
        };

        struct DeviceList
        {
            IMFActivate** devices = nullptr;
            UINT32 count = 0;
            ~DeviceList()
            {
                for (UINT32 i = 0; i < count; ++i)
                {
                    devices[i]->Release();
                }
                CoTaskMemFree(devices);
            }
        };

        bool EnumerateCameras(DeviceList& list)
        {
            ComPtr<IMFAttributes> attributes;
            if (FAILED(MFCreateAttributes(&attributes, 1)) || FAILED(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)))
            {
                return false;
            }
            return SUCCEEDED(MFEnumDeviceSources(attributes.Get(), &list.devices, &list.count));
        }

        std::wstring GetString(IMFActivate* activate, const GUID& key)
        {
            WCHAR* text = nullptr;
            UINT32 length = 0;
            std::wstring result;
            if (SUCCEEDED(activate->GetAllocatedString(key, &text, &length)) && text != nullptr)
            {
                result.assign(text, length);
                CoTaskMemFree(text);
            }
            return result;
        }
    }

    FaceCamera::~FaceCamera() { Stop(); }

    std::vector<CameraInfo> FaceCamera::List()
    {
        std::vector<CameraInfo> cameras;
        MediaScope scope;
        if (!scope.Ok())
        {
            return cameras;
        }
        DeviceList list;
        if (!EnumerateCameras(list))
        {
            return cameras;
        }
        for (UINT32 i = 0; i < list.count; ++i)
        {
            CameraInfo info;
            info.name = Utf8(GetString(list.devices[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME));
            info.symbolicLink = GetString(list.devices[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
            cameras.push_back(std::move(info));
        }
        return cameras;
    }

    std::string FaceCamera::Status() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return status_;
    }

    void FaceCamera::SetStatus(const State state, const std::string& text)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            status_ = text;
        }
        state_.store(state);
    }

    void FaceCamera::Start(const std::string& name, const int width, const int height, const int fps)
    {
        if (running_.load())
        {
            return;
        }
        if (thread_.joinable())
        {
            thread_.join();
        }
        stopRequested_.store(false);
        running_.store(true);
        SetStatus(State::Opening, "opening the camera ...");
        thread_ = std::thread([this, name, width, height, fps]() { Run(name, width, height, fps); });
    }

    void FaceCamera::Stop()
    {
        stopRequested_.store(true);
        if (thread_.joinable())
        {
            thread_.join();
        }
        running_.store(false);
        state_.store(State::Stopped);
        std::lock_guard<std::mutex> lock(mutex_);
        latest_ = {};   // the last picture of a face does not stay in memory
    }

    bool FaceCamera::Latest(FaceImage& out, std::uint64_t& serial)
    {
        if (serial_.load() == serial)
        {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (latest_.Empty())
        {
            return false;
        }
        out = latest_;
        serial = serial_.load();
        return true;
    }

    void FaceCamera::Run(std::string name, const int width, const int height, const int fps)
    {
        MediaScope scope;
        const auto finish = [this](const State state, const std::string& text)
        {
            SetStatus(state, text);
            running_.store(false);
        };
        if (!scope.Ok())
        {
            finish(State::Failed, "Media Foundation did not start");
            return;
        }
        DeviceList list;
        if (!EnumerateCameras(list) || list.count == 0)
        {
            finish(State::Failed, "no camera found: connect one (or use the password)");
            return;
        }
        UINT32 chosen = 0;
        for (UINT32 i = 0; i < list.count; ++i)
        {
            if (!name.empty() && Utf8(GetString(list.devices[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME)) == name)
            {
                chosen = i;
                break;
            }
        }
        ComPtr<IMFMediaSource> source;
        HRESULT hr = list.devices[chosen]->ActivateObject(IID_PPV_ARGS(&source));
        if (FAILED(hr))
        {
            if (hr == E_ACCESSDENIED)
            {
                finish(State::Failed, "access to the camera is denied: switch on 'Let desktop apps access your camera' in the privacy settings of Windows");
            }
            else
            {
                finish(State::Failed, "the camera cannot be opened (" + HexText(hr) + "): another program may be using it");
            }
            return;
        }
        ComPtr<IMFAttributes> readerAttributes;
        MFCreateAttributes(&readerAttributes, 2);
        readerAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        ComPtr<IMFSourceReader> reader;
        hr = MFCreateSourceReaderFromMediaSource(source.Get(), readerAttributes.Get(), &reader);
        if (FAILED(hr))
        {
            source->Shutdown();
            finish(State::Failed, "the camera cannot be read (" + HexText(hr) + ")");
            return;
        }

        // the native format of the size asked for (the camera gives 640 x 480 as YUY2 or MJPEG), then the conversion to RGB32 by the reader
        for (DWORD index = 0;; ++index)
        {
            ComPtr<IMFMediaType> native;
            if (FAILED(reader->GetNativeMediaType(kVideoStream, index, &native)))
            {
                break;
            }
            UINT32 nativeWidth = 0;
            UINT32 nativeHeight = 0;
            MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &nativeWidth, &nativeHeight);
            UINT32 rateNumerator = 0;
            UINT32 rateDenominator = 1;
            MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &rateNumerator, &rateDenominator);
            const double nativeFps = rateDenominator > 0 ? static_cast<double>(rateNumerator) / rateDenominator : 0.0;
            if (static_cast<int>(nativeWidth) == width && static_cast<int>(nativeHeight) == height && nativeFps >= fps - 1.0)
            {
                if (SUCCEEDED(reader->SetCurrentMediaType(kVideoStream, nullptr, native.Get())))
                {
                    break;
                }
            }
        }
        ComPtr<IMFMediaType> wanted;
        MFCreateMediaType(&wanted);
        wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        wanted->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        hr = reader->SetCurrentMediaType(kVideoStream, nullptr, wanted.Get());
        if (FAILED(hr))
        {
            source->Shutdown();
            finish(State::Failed, "the camera gives no picture that Face ID can read (" + HexText(hr) + ")");
            return;
        }
        ComPtr<IMFMediaType> actual;
        UINT32 frameWidth = static_cast<UINT32>(width);
        UINT32 frameHeight = static_cast<UINT32>(height);
        LONG defaultStride = 0;
        if (SUCCEEDED(reader->GetCurrentMediaType(kVideoStream, &actual)))
        {
            MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &frameWidth, &frameHeight);
            defaultStride = static_cast<LONG>(MFGetAttributeUINT32(actual.Get(), MF_MT_DEFAULT_STRIDE, 0));
        }
        if (frameWidth == 0 || frameHeight == 0)
        {
            source->Shutdown();
            finish(State::Failed, "the camera reports a picture of no size");
            return;
        }
        SetStatus(State::Streaming, "");

        while (!stopRequested_.load())
        {
            DWORD streamIndex = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;
            hr = reader->ReadSample(kVideoStream, 0, &streamIndex, &flags, &timestamp, &sample);
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0 || (flags & MF_SOURCE_READERF_ERROR) != 0)
            {
                SetStatus(State::Failed, "the camera stopped (it was unplugged or taken by another program)");
                break;
            }
            if (sample == nullptr)
            {
                continue;
            }
            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(sample->ConvertToContiguousBuffer(&buffer)))
            {
                continue;
            }
            const BYTE* scanline0 = nullptr;
            LONG pitch = 0;
            ComPtr<IMF2DBuffer> buffer2d;
            bool locked2d = false;
            BYTE* data = nullptr;
            if (SUCCEEDED(buffer.As(&buffer2d)) && SUCCEEDED(buffer2d->Lock2D(const_cast<BYTE**>(&scanline0), &pitch)))
            {
                locked2d = true;
            }
            else
            {
                DWORD maxLength = 0;
                DWORD currentLength = 0;
                if (FAILED(buffer->Lock(&data, &maxLength, &currentLength)))
                {
                    continue;
                }
                const LONG stride = defaultStride != 0 ? defaultStride : static_cast<LONG>(frameWidth * 4);
                if (stride < 0)
                {
                    scanline0 = data + static_cast<std::size_t>(frameHeight - 1) * static_cast<std::size_t>(-stride);   // bottom-up: the top row is the last in memory
                    pitch = stride;
                }
                else
                {
                    scanline0 = data;
                    pitch = stride;
                }
            }
            FaceImage image;
            image.width = static_cast<int>(frameWidth);
            image.height = static_cast<int>(frameHeight);
            image.rgb.resize(static_cast<std::size_t>(frameWidth) * frameHeight * 3u);
            for (UINT32 y = 0; y < frameHeight; ++y)
            {
                const BYTE* row = scanline0 + static_cast<std::ptrdiff_t>(y) * pitch;
                std::uint8_t* target = image.rgb.data() + static_cast<std::size_t>(y) * frameWidth * 3u;
                for (UINT32 x = 0; x < frameWidth; ++x)
                {
                    target[x * 3u] = row[x * 4u + 2];
                    target[x * 3u + 1] = row[x * 4u + 1];
                    target[x * 3u + 2] = row[x * 4u];
                }
            }
            if (locked2d)
            {
                buffer2d->Unlock2D();
            }
            else
            {
                buffer->Unlock();
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                latest_ = std::move(image);
            }
            serial_.fetch_add(1);
        }
        source->Shutdown();
        running_.store(false);
        if (state_.load() == State::Streaming || state_.load() == State::Opening)
        {
            state_.store(State::Stopped);
        }
    }
}
