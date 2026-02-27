// Window.cpp

#include <mutex>

#include <myengine/core/Window.h>
#include <myengine/core/Application.h>

namespace myengine::core
{
    Window::Window(WindowDesc desc) : desc_(std::move(desc)) {}

    Window::~Window()
    {
        if (hwnd_ != nullptr)
        {
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

        RECT rect{0, 0, static_cast<LONG>(desc_.width), static_cast<LONG>(desc_.height)};
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;

        hwnd_ = CreateWindowExW(0, ClassName(), desc_.title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                width, height, nullptr, nullptr, GetModuleHandleW(nullptr), this);

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
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            wc.lpszClassName = Window::ClassName();

            registered = RegisterClassExW(&wc) != 0;
        });

        return registered;
    }
}