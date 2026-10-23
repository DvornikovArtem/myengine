// Window.cpp

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include <ole2.h>
#include <shellapi.h>

#include <myengine/core/DpiScale.h>
#include <myengine/core/Window.h>
#include <myengine/core/Application.h>
#include <myengine/core/ServiceLocator.h>

namespace myengine::core
{
    namespace
    {
        std::string ToUtf8(const wchar_t* text, const int length)
        {
            if (length <= 0)
            {
                return {};
            }
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
            std::string result(static_cast<std::size_t>(std::max(bytes, 0)), '\0');
            if (bytes > 0)
            {
                WideCharToMultiByte(CP_UTF8, 0, text, length, result.data(), bytes, nullptr, nullptr);
            }
            return result;
        }

        bool MonitorInfoOf(HWND hwnd, MONITORINFOEXW& info)
        {
            const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            info = {};
            info.cbSize = sizeof(info);
            return monitor != nullptr && GetMonitorInfoW(monitor, &info) != FALSE;
        }

        // Accepts files dragged in from Explorer (OLE drop target) and reports them through the editor state:
        // the panels decide what to do. Everything runs on the thread of the window.
        class FileDropTarget final : public IDropTarget
        {
        public:
            FileDropTarget(const HWND hwnd, const WindowId windowId) : hwnd_(hwnd), windowId_(windowId) {}

            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override
            {
                if (id == IID_IUnknown || id == IID_IDropTarget)
                {
                    *object = static_cast<IDropTarget*>(this);
                    AddRef();
                    return S_OK;
                }
                *object = nullptr;
                return E_NOINTERFACE;
            }
            ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&references_)); }
            ULONG STDMETHODCALLTYPE Release() override
            {
                const auto count = static_cast<ULONG>(InterlockedDecrement(&references_));
                if (count == 0)
                {
                    delete this;
                }
                return count;
            }

            HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL point, DWORD* effect) override
            {
                hasFiles_ = HasFiles(data);
                *effect = hasFiles_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
                Update(point, hasFiles_);
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL point, DWORD* effect) override
            {
                *effect = hasFiles_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
                Update(point, hasFiles_);
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE DragLeave() override
            {
                auto& drag = ServiceLocator::GetEditorRuntimeState().fileDrag;
                drag.dragging = false;
                hasFiles_ = false;
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL point, DWORD* effect) override
            {
                auto& drag = ServiceLocator::GetEditorRuntimeState().fileDrag;
                drag.dragging = false;
                *effect = DROPEFFECT_NONE;
                FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
                STGMEDIUM medium{};
                if (data != nullptr && SUCCEEDED(data->GetData(&format, &medium)))
                {
                    const auto drop = static_cast<HDROP>(GlobalLock(medium.hGlobal));
                    if (drop != nullptr)
                    {
                        const UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
                        drag.dropped.clear();
                        for (UINT i = 0; i < count && i < 32; ++i)
                        {
                            std::wstring path(static_cast<std::size_t>(DragQueryFileW(drop, i, nullptr, 0)) + 1, L'\0');
                            const UINT length = DragQueryFileW(drop, i, path.data(), static_cast<UINT>(path.size()));
                            drag.dropped.push_back(ToUtf8(path.c_str(), static_cast<int>(length)));
                        }
                        GlobalUnlock(medium.hGlobal);
                        POINT client{point.x, point.y};
                        ScreenToClient(hwnd_, &client);
                        const float scale = dpi::ScaleFromDpi(GetDpiForWindow(hwnd_));
                        drag.dropX = dpi::ToLogical(static_cast<float>(client.x), scale); // the panels are laid out in logical pixels
                        drag.dropY = dpi::ToLogical(static_cast<float>(client.y), scale);
                        drag.windowId = windowId_;
                        *effect = DROPEFFECT_COPY;
                    }
                    ReleaseStgMedium(&medium);
                }
                hasFiles_ = false;
                return S_OK;
            }

        private:
            static bool HasFiles(IDataObject* data)
            {
                FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
                return data != nullptr && data->QueryGetData(&format) == S_OK;
            }

            void Update(const POINTL point, const bool dragging)
            {
                auto& drag = ServiceLocator::GetEditorRuntimeState().fileDrag;
                POINT client{point.x, point.y};
                ScreenToClient(hwnd_, &client);
                const float scale = dpi::ScaleFromDpi(GetDpiForWindow(hwnd_));
                drag.windowId = windowId_;
                drag.dragging = dragging;
                drag.x = dpi::ToLogical(static_cast<float>(client.x), scale);
                drag.y = dpi::ToLogical(static_cast<float>(client.y), scale);
            }

            HWND hwnd_ = nullptr;
            WindowId windowId_ = 0;
            LONG references_ = 1;
            bool hasFiles_ = false;
        };
    }

    Window::Window(WindowDesc desc) : desc_(std::move(desc)) {}

    Window::~Window()
    {
        if (displaySwitched_ && hwnd_ != nullptr)
        {
            MONITORINFOEXW info{};
            if (MonitorInfoOf(hwnd_, info))
            {
                ChangeDisplaySettingsExW(info.szDevice, nullptr, nullptr, 0, nullptr);
            }
            displaySwitched_ = false;
        }
        if (hwnd_ != nullptr)
        {
            if (dropRegistered_)
            {
                RevokeDragDrop(hwnd_);
                dropRegistered_ = false;
            }
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

    bool Window::Create(Application* owner)
    {
        owner_ = owner;

        if (!RegisterWindowClass(GetModuleHandleW(nullptr)))
        {
            return false;
        }

        // The window does not exist yet: the DPI of the system (of the main screen) decides the size; the real one is read below
        const std::uint32_t creationDpi = std::max<std::uint32_t>(GetDpiForSystem(), dpi::kBaseDpi);
        const auto outerSizeFor = [this](const std::uint32_t dpiForSize, int& outWidth, int& outHeight)
        {
            const float scale = desc_.scaleToDpi ? dpi::ScaleFromDpi(dpiForSize) : 1.0f;
            RECT rect{0, 0, static_cast<LONG>(dpi::ScaleLength(desc_.width, scale)), static_cast<LONG>(dpi::ScaleLength(desc_.height, scale))};
            AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0, dpiForSize);
            outWidth = rect.right - rect.left;
            outHeight = rect.bottom - rect.top;
        };
        int width = 0;
        int height = 0;
        outerSizeFor(creationDpi, width, height);

        hwnd_ = CreateWindowExW(0, ClassName(), desc_.title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                width, height, nullptr, nullptr, GetModuleHandleW(nullptr), this);

        if (hwnd_ != nullptr)
        {
            // The window may have been put on a screen of another scale than the main one: the size is made again for the real DPI
            dpi_ = std::max<std::uint32_t>(GetDpiForWindow(hwnd_), dpi::kBaseDpi);
            if (dpi_ != creationDpi)
            {
                outerSizeFor(dpi_, width, height);
                SetWindowPos(hwnd_, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            // desc_ keeps the real client size from now on (the swap chain is made with it)
            RECT client{};
            if (GetClientRect(hwnd_, &client) != FALSE)
            {
                desc_.width = static_cast<std::uint32_t>(client.right - client.left);
                desc_.height = static_cast<std::uint32_t>(client.bottom - client.top);
            }

            // Files dragged in from Explorer: the assistant attaches them. A failure only costs this feature.
            const HRESULT initialized = OleInitialize(nullptr);
            if (SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE)
            {
                auto* target = new FileDropTarget(hwnd_, desc_.id);
                dropRegistered_ = SUCCEEDED(RegisterDragDrop(hwnd_, target));
                target->Release();
            }
        }

        return hwnd_ != nullptr;
    }

    void Window::Show(const int showCommand) const
    {
        ShowWindow(hwnd_, showCommand);
        UpdateWindow(hwnd_);
    }

    HWND Window::Handle() const
    {
        return hwnd_;
    }

    WindowId Window::Id() const
    {
        return desc_.id;
    }

    std::uint32_t Window::Width() const
    {
        return desc_.width;
    }

    std::uint32_t Window::Height() const
    {
        return desc_.height;
    }

    const std::wstring& Window::Title() const
    {
        return desc_.title;
    }

    void Window::SetTitle(const std::wstring& title)
    {
        desc_.title = title;
        if (hwnd_ != nullptr)
        {
            SetWindowTextW(hwnd_, desc_.title.c_str());
        }
    }

    void Window::SetClientSize(const std::uint32_t width, const std::uint32_t height)
    {
        desc_.width = width;
        desc_.height = height;
    }

    float Window::DpiScale() const
    {
        return dpi::ScaleFromDpi(dpi_);
    }

    bool Window::OnDpiChanged(const std::uint32_t newDpi, const RECT& suggested)
    {
        dpi_ = std::max<std::uint32_t>(newDpi, dpi::kBaseDpi);
        if (hwnd_ == nullptr || !dpi::FollowsSuggestedRect(displayMode_ == DisplayMode::Windowed))
        {
            return false;
        }
        SetWindowPos(hwnd_, nullptr, suggested.left, suggested.top, suggested.right - suggested.left, suggested.bottom - suggested.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return true;
    }

    bool Window::GetMonitorSize(std::uint32_t& width, std::uint32_t& height) const
    {
        MONITORINFOEXW info{};
        if (hwnd_ == nullptr || !MonitorInfoOf(hwnd_, info))
        {
            return false;
        }
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (EnumDisplaySettingsW(info.szDevice, ENUM_REGISTRY_SETTINGS, &mode) != FALSE)
        {
            width = mode.dmPelsWidth;
            height = mode.dmPelsHeight;
        }
        else
        {
            width = static_cast<std::uint32_t>(info.rcMonitor.right - info.rcMonitor.left);
            height = static_cast<std::uint32_t>(info.rcMonitor.bottom - info.rcMonitor.top);
        }
        return true;
    }

    std::vector<std::pair<std::uint32_t, std::uint32_t>> Window::ListResolutions() const
    {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> result;
        MONITORINFOEXW info{};
        if (hwnd_ == nullptr || !MonitorInfoOf(hwnd_, info))
        {
            return result;
        }
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        for (DWORD index = 0; EnumDisplaySettingsW(info.szDevice, index, &mode) != FALSE; ++index)
        {
            if (mode.dmPelsWidth < 1280 || mode.dmPelsHeight < 720 || mode.dmBitsPerPel < 32)
            {
                continue;
            }
            const std::pair<std::uint32_t, std::uint32_t> size{mode.dmPelsWidth, mode.dmPelsHeight};
            if (std::find(result.begin(), result.end(), size) == result.end())
            {
                result.push_back(size);
            }
        }
        std::sort(result.begin(), result.end());
        return result;
    }

    bool Window::SetDisplayMode(const DisplayMode mode, std::uint32_t width, std::uint32_t height)
    {
        if (hwnd_ == nullptr)
        {
            return false;
        }
        MONITORINFOEXW info{};
        if (!MonitorInfoOf(hwnd_, info))
        {
            return false;
        }

        // Back to the desktop display first: a switch made for Fullscreen ends here, whatever the new mode is
        if (displaySwitched_)
        {
            ChangeDisplaySettingsExW(info.szDevice, nullptr, nullptr, 0, nullptr);
            displaySwitched_ = false;
            MonitorInfoOf(hwnd_, info);
        }

        if (mode == DisplayMode::Windowed)
        {
            width = std::max<std::uint32_t>(width, 640u);
            height = std::max<std::uint32_t>(height, 360u);
            SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
            RECT frame{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
            AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi_);
            const int outerWidth = frame.right - frame.left;
            const int outerHeight = frame.bottom - frame.top;
            const RECT& work = info.rcWork;
            // centred on the monitor, and never larger than its work area (the frame would be off the screen)
            const int x = work.left + std::max<int>(0, ((work.right - work.left) - outerWidth) / 2);
            const int y = work.top + std::max<int>(0, ((work.bottom - work.top) - outerHeight) / 2);
            SetWindowPos(hwnd_, HWND_NOTOPMOST, x, y, outerWidth, outerHeight, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
        else
        {
            if (mode == DisplayMode::Fullscreen && width > 0 && height > 0)
            {
                DEVMODEW current{};
                current.dmSize = sizeof(current);
                EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &current);
                if (current.dmPelsWidth != width || current.dmPelsHeight != height)
                {
                    DEVMODEW wanted = current;
                    wanted.dmPelsWidth = width;
                    wanted.dmPelsHeight = height;
                    wanted.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
                    displaySwitched_ = ChangeDisplaySettingsExW(info.szDevice, &wanted, nullptr, CDS_FULLSCREEN, nullptr) == DISP_CHANGE_SUCCESSFUL;
                    MonitorInfoOf(hwnd_, info);
                }
            }
            SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            const RECT& screen = info.rcMonitor;
            SetWindowPos(hwnd_, HWND_TOP, screen.left, screen.top, screen.right - screen.left, screen.bottom - screen.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
        displayMode_ = mode;
        return true;
    }

    LRESULT CALLBACK Window::StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        if (msg == WM_NCCREATE)
        {
            const auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            const auto window = static_cast<Window*>(create->lpCreateParams);

            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
            window->hwnd_ = hwnd;
        }

        const auto window = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (window != nullptr)
        {
            return window->WndProc(msg, wparam, lparam);
        }

        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    LRESULT Window::WndProc(UINT msg, WPARAM wparam, LPARAM lparam)
    {
        if (owner_ != nullptr)
        {
            return owner_->HandleWindowMessage(*this, msg, wparam, lparam);
        }

        return DefWindowProcW(hwnd_, msg, wparam, lparam);
    }

    const wchar_t* Window::ClassName()
    {
        return L"myengine_window_class";
    }

    bool Window::RegisterWindowClass(HINSTANCE instance)
    {
        static std::once_flag registerFlag;
        static bool registered = false;

        std::call_once(registerFlag, [instance]() {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(WNDCLASSEXW);
            wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
            wc.lpfnWndProc = &Window::StaticWndProc;
            wc.hInstance = instance;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            // The icon of the exe (app/resources, resource id 1) for the title bar and the taskbar; a binary without the resource (tests) has the default one
            wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetricsForDpi(SM_CXICON, GetDpiForSystem()), GetSystemMetricsForDpi(SM_CYICON, GetDpiForSystem()), LR_DEFAULTCOLOR));
            wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem()), GetSystemMetricsForDpi(SM_CYSMICON, GetDpiForSystem()), LR_DEFAULTCOLOR));
            // The editor background (kTitle): before the first frame the window is dark, not white
            wc.hbrBackground = CreateSolidBrush(RGB(0x15, 0x15, 0x15));
            wc.lpszClassName = Window::ClassName();

            registered = RegisterClassExW(&wc) != 0;
        });

        return registered;
    }
}