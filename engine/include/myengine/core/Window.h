// Window.h

#pragma once

#include <string>

#include <myengine/core/Types.h>

// If the WIN32_LEAN_AND_MEAN macro is defined before including windows.h, rarely used parts are excluded from the header to speed up compilation
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace myengine::core
{
	class Application;

	struct WindowDesc
	{
		WindowId id = 0;
		std::wstring title;
		std::uint32_t width = 1280;
		std::uint32_t height = 720;
	};

	class Window
	{
	public:
		explicit Window(WindowDesc desc);
		~Window();

		Window(const Window&) = delete;
		Window& operator=(const Window&) = delete;

		bool Create(Application* owner);
		void Show(int showCommand = SW_SHOWDEFAULT) const;

		HWND Handle() const;
		WindowId Id() const;
		std::uint32_t Width() const;
		std::uint32_t Height() const;
		const std::wstring& Title() const;
		void SetTitle(const std::wstring& title);

		void SetClientSize(std::uint32_t width, std::uint32_t height);

	private:
		static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
		LRESULT WndProc(UINT msg, WPARAM wparam, LPARAM lparam);

		static const wchar_t* ClassName();
		static bool RegisterWindowClass(HINSTANCE instance);

		WindowDesc desc_;
		HWND hwnd_ = nullptr;
		Application* owner_ = nullptr;
	};
}