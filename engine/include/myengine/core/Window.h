// Window.h

#pragma once

#include <string>
#include <utility>
#include <vector>

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
		/// width and height are for a screen of 100 % (96 DPI): the client size grows with the Windows scale of the screen that the window
		/// is made on (1600 x 900 at 125 %), so that the window looks the same size on every screen. false: they are real pixels.
		bool scaleToDpi = true;
	};

	// How the window sits on the screen (the video settings of the game, docs/game/rl-ui.md)
	enum class DisplayMode
	{
		Windowed,    // a normal window with the given client size
		Borderless,  // no frame, covers the monitor at the desktop resolution
		Fullscreen,  // covers the monitor; the display is switched to the given resolution (reverted when the mode changes or the process ends)
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

		/// The DPI of the screen the window is on (96 at 100 %, 120 at 125 %) and the scale it makes (1.25); the process is Per-Monitor V2 aware,
		/// so all the sizes of the window are real pixels
		std::uint32_t Dpi() const { return dpi_; }
		float DpiScale() const;
		/// WM_DPICHANGED: the window was moved to a screen of another scale (or the scale was changed). `suggested` is the outer rectangle that
		/// Windows suggests for the new scale; a windowed window takes it (and WM_SIZE follows), a borderless or a fullscreen one covers its
		/// monitor and ignores it. Returns true when the window took the rectangle.
		bool OnDpiChanged(std::uint32_t newDpi, const RECT& suggested);

		/// Changes how the window sits on the screen. For Windowed `width` x `height` is the client size; for Fullscreen the resolution of the display.
		/// Borderless ignores them (the desktop resolution). The swap chain is resized by the WM_SIZE that follows.
		bool SetDisplayMode(DisplayMode mode, std::uint32_t width, std::uint32_t height);
		DisplayMode GetDisplayMode() const { return displayMode_; }
		/// The desktop resolution of the monitor the window is on
		bool GetMonitorSize(std::uint32_t& width, std::uint32_t& height) const;
		/// The resolutions the display of the window can be set to (width x height, 1280 x 720 and up, without repeats), the largest last
		std::vector<std::pair<std::uint32_t, std::uint32_t>> ListResolutions() const;

	private:
		static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
		LRESULT WndProc(UINT msg, WPARAM wparam, LPARAM lparam);

		static const wchar_t* ClassName();
		static bool RegisterWindowClass(HINSTANCE instance);

		WindowDesc desc_;
		HWND hwnd_ = nullptr;
		Application* owner_ = nullptr;
		bool dropRegistered_ = false; // the OLE drop target of the window (files from Explorer)
		DisplayMode displayMode_ = DisplayMode::Windowed;
		bool displaySwitched_ = false; // ChangeDisplaySettings made the display differ from the desktop
		std::uint32_t dpi_ = 96;
	};
}