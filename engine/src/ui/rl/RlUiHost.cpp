#include <myengine/ui/rl/RlUiHost.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <xinput.h>

#include <imgui/imgui.h>

#include <myengine/core/AppPaths.h>
#include <myengine/ui/UiManager.h>

#include "RlUiInternal.h"

namespace myengine::rlui
{
    namespace
    {
        // The atlas is baked for one of these scales and the text is scaled by the small difference to the real one: the fonts are not baked
        // again while a window is dragged to another size, only when its size class changes
        constexpr float kBuckets[] = {0.5f, 0.67f, 0.8f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f};
    }

    float FontBucketFor(const float scale)
    {
        float best = kBuckets[0];
        float bestDistance = 1e9f;
        for (const float bucket : kBuckets)
        {
            const float distance = std::fabs(std::log(bucket / std::max(scale, 0.1f)));
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = bucket;
            }
        }
        return best;
    }

    float UiScaleForWindow(const std::uint32_t width, const std::uint32_t height, const float uiScaleSetting)
    {
        return std::min(static_cast<float>(height) / 1080.0f, static_cast<float>(width) / 1920.0f) * uiScaleSetting;
    }

    namespace
    {
        std::filesystem::path FindFontDirectory()
        {
            std::vector<std::filesystem::path> candidates;
            wchar_t module[MAX_PATH]{};
            if (GetModuleFileNameW(nullptr, module, MAX_PATH) > 0)
            {
                candidates.push_back(std::filesystem::path(module).parent_path() / "assets" / "fonts");
            }
            const std::filesystem::path source = core::apppaths::SourceRoot();
            if (!source.empty())
            {
                candidates.push_back(source / "assets" / "fonts");
            }
            std::error_code ec;
            candidates.push_back(std::filesystem::current_path(ec) / "assets" / "fonts");
            for (const auto& candidate : candidates)
            {
                if (std::filesystem::exists(candidate / "exo2" / "Exo2-BlackItalic.ttf", ec))
                {
                    return candidate;
                }
            }
            return candidates.empty() ? std::filesystem::path("assets/fonts") : candidates.front();
        }

        void AppendUtf8(std::string& out, unsigned int c)
        {
            if (c < 0x80)
            {
                out.push_back(static_cast<char>(c));
            }
            else if (c < 0x800)
            {
                out.push_back(static_cast<char>(0xC0 | (c >> 6)));
                out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
            }
            else
            {
                out.push_back(static_cast<char>(0xE0 | (c >> 12)));
                out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
            }
        }

        // The first pad of XInput into the layout of RawInput
        void ReadPad(RawInput& raw)
        {
            XINPUT_STATE state{};
            if (XInputGetState(0, &state) != ERROR_SUCCESS)
            {
                raw.padConnected = false;
                return;
            }
            raw.padConnected = true;
            const WORD b = state.Gamepad.wButtons;
            const struct
            {
                WORD mask;
                int bit;
            } map[] = {{XINPUT_GAMEPAD_A, kPadA}, {XINPUT_GAMEPAD_B, kPadB}, {XINPUT_GAMEPAD_X, kPadX}, {XINPUT_GAMEPAD_Y, kPadY},
                       {XINPUT_GAMEPAD_LEFT_SHOULDER, kPadLB}, {XINPUT_GAMEPAD_RIGHT_SHOULDER, kPadRB}, {XINPUT_GAMEPAD_BACK, kPadBack},
                       {XINPUT_GAMEPAD_START, kPadStart}, {XINPUT_GAMEPAD_LEFT_THUMB, kPadLeftStick}, {XINPUT_GAMEPAD_RIGHT_THUMB, kPadRightStick},
                       {XINPUT_GAMEPAD_DPAD_UP, kPadDpadUp}, {XINPUT_GAMEPAD_DPAD_DOWN, kPadDpadDown}, {XINPUT_GAMEPAD_DPAD_LEFT, kPadDpadLeft},
                       {XINPUT_GAMEPAD_DPAD_RIGHT, kPadDpadRight}};
            raw.padButtons = 0;
            for (const auto& entry : map)
            {
                if ((b & entry.mask) != 0)
                {
                    raw.padButtons |= 1u << entry.bit;
                }
            }
            raw.padAxes[0] = std::max(state.Gamepad.sThumbLX / 32767.0f, -1.0f);
            raw.padAxes[1] = std::max(state.Gamepad.sThumbLY / 32767.0f, -1.0f);
            raw.padAxes[2] = std::max(state.Gamepad.sThumbRX / 32767.0f, -1.0f);
            raw.padAxes[3] = std::max(state.Gamepad.sThumbRY / 32767.0f, -1.0f);
            raw.padAxes[4] = state.Gamepad.bLeftTrigger / 255.0f;
            raw.padAxes[5] = state.Gamepad.bRightTrigger / 255.0f;
            // the dead zone of the sticks for the menu: the small drift of a stick must not move the focus
            for (int a = 0; a < 4; ++a)
            {
                if (std::fabs(raw.padAxes[a]) < 0.25f)
                {
                    raw.padAxes[a] = 0.0f;
                }
            }
        }

        bool ProcessIsForeground()
        {
            DWORD processId = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &processId);
            return processId == GetCurrentProcessId();
        }
    }

    struct Host::Impl
    {
        ui::UiManager* manager = nullptr;
        HostConfig config;
        std::filesystem::path fontDirectory;
        Fonts fonts;
        float bakedBucket = 0.0f;
        double time = 0.0;
        double lastRebuild = -10.0;
        Host* owner = nullptr;
    };

    Host::Host() : impl_(new Impl) { impl_->owner = this; }

    Host::~Host()
    {
        Uninstall();
        delete impl_;
    }

    void Host::Install(ui::UiManager& manager, const HostConfig& config)
    {
        impl_->manager = &manager;
        impl_->config = config;
        uiScale_ = config.uiScale;
        impl_->fontDirectory = config.fontDirectory.empty() ? FindFontDirectory() : config.fontDirectory;

        ui::GameUiHooks hooks;
        Impl* impl = impl_;
        hooks.bakeFonts = [impl](ImFontAtlas& atlas, const std::uint32_t width, const std::uint32_t height)
        {
            impl->bakedBucket = FontBucketFor(UiScaleForWindow(width, height, impl->owner->uiScale_));
            impl->fonts.Clear();
            std::string error;
            if (!impl->fonts.Bake(atlas, impl->fontDirectory, impl->bakedBucket, &error))
            {
                impl->owner->fontError_ = error;
            }
            else
            {
                impl->owner->fontError_.clear();
            }
        };
        hooks.build = [impl](const core::WindowId, const std::uint32_t width, const std::uint32_t height, const float deltaTime)
        {
            Host& host = *impl->owner;
            ImGuiIO& io = ImGui::GetIO();
            impl->time += static_cast<double>(deltaTime);

            // the devices
            RawInput raw;
            raw.windowActive = ProcessIsForeground();
            if (raw.windowActive)
            {
                // The state of the keys as the window has seen them: one call. (256 calls of GetAsyncKeyState cost 20 ms a frame on a machine
                // where every call into the input system takes 80 us.)
                BYTE state[256] = {};
                if (GetKeyboardState(state) != FALSE)
                {
                    for (int vk = 8; vk < 256; ++vk)
                    {
                        raw.keys[vk] = (state[vk] & 0x80) != 0;
                    }
                }
            }
            raw.mouse[0] = io.MouseDown[0];
            raw.mouse[1] = io.MouseDown[1];
            raw.mouse[2] = io.MouseDown[2];
            raw.mousePosition = {io.MousePos.x, io.MousePos.y};
            raw.mouseWheel = io.MouseWheel;
            for (int i = 0; i < io.InputQueueCharacters.Size; ++i)
            {
                AppendUtf8(raw.typed, io.InputQueueCharacters[i]);
            }
            if (raw.windowActive)
            {
                ReadPad(raw);
            }
            if (host.rawHook_)
            {
                host.rawHook_(raw);
            }
            host.lastRaw_ = raw;

            // the size class of the window changed: the fonts have to be baked again (not more often than twice a second)
            const float scale = UiScaleForWindow(width, height, host.uiScale_);
            if (impl->manager != nullptr && FontBucketFor(scale) != impl->bakedBucket && impl->time - impl->lastRebuild > 0.5)
            {
                impl->lastRebuild = impl->time;
                impl->manager->RequestFontRebuild();
            }

            FrameDesc desc;
            desc.width = static_cast<float>(width);
            desc.height = static_cast<float>(height);
            desc.uiScale = host.uiScale_;
            desc.deltaTime = deltaTime;
            desc.time = impl->time;
            host.frame_.Begin(desc, raw, ImGui::GetForegroundDrawList(), &impl->fonts);
            if (host.screen_)
            {
                host.screen_(host.frame_);
            }
            host.frame_.End();
        };
        manager.SetGameUi(std::move(hooks));
        manager.SetGameUiCapturesInput(true);
    }

    void Host::Uninstall()
    {
        if (impl_ != nullptr && impl_->manager != nullptr)
        {
            impl_->manager->SetGameUi({});
            impl_->manager->SetGameUiCapturesInput(false);
            impl_->manager = nullptr;
        }
    }

    void Host::SetScreen(std::function<void(Frame&)> screen) { screen_ = std::move(screen); }

    void Host::SetUiScale(const float scale)
    {
        const float clamped = std::clamp(scale, 0.75f, 1.25f);
        if (std::fabs(clamped - uiScale_) < 1e-4f)
        {
            return;
        }
        uiScale_ = clamped;
        if (impl_->manager != nullptr)
        {
            impl_->manager->RequestFontRebuild();
        }
    }
}
