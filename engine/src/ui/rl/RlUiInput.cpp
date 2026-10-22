#include <myengine/ui/rl/RlUiInput.h>

#include <algorithm>
#include <cmath>

namespace myengine::rlui
{
    namespace
    {
        constexpr float kRepeatDelay = 0.35f;
        constexpr float kRepeatInterval = 1.0f / 12.0f;
        constexpr float kStickThreshold = 0.5f;

        bool PadDown(const RawInput& raw, const int bit) { return raw.padConnected && ((raw.padButtons >> bit) & 1u) != 0; }

        // Is a navigation event held in this frame of the devices
        bool NavDown(const RawInput& raw, const Nav nav, const bool textEditing)
        {
            const auto key = [&](const int vk) { return raw.keys[vk & 255]; };
            // W A S D / Q E / R are letters of a text while a field is edited
            const bool letters = !textEditing;
            const float lx = raw.padConnected ? raw.padAxes[0] : 0.0f;
            const float ly = raw.padConnected ? raw.padAxes[1] : 0.0f;
            switch (nav)
            {
            case Nav::Up: return key(0x26) || (letters && key('W')) || PadDown(raw, kPadDpadUp) || ly > kStickThreshold;
            case Nav::Down: return key(0x28) || (letters && key('S')) || PadDown(raw, kPadDpadDown) || ly < -kStickThreshold;
            case Nav::Left: return key(0x25) || (letters && key('A')) || PadDown(raw, kPadDpadLeft) || lx < -kStickThreshold;
            case Nav::Right: return key(0x27) || (letters && key('D')) || PadDown(raw, kPadDpadRight) || lx > kStickThreshold;
            case Nav::Confirm: return key(0x0D) || (!textEditing && key(0x20)) || PadDown(raw, kPadA);
            case Nav::Back: return key(0x1B) || PadDown(raw, kPadB);
            case Nav::TabPrev: return (letters && key('Q')) || PadDown(raw, kPadLB);
            case Nav::TabNext: return (letters && key('E')) || PadDown(raw, kPadRB);
            case Nav::ResetRow: return (letters && key('R')) || PadDown(raw, kPadY);
            case Nav::Clear: return key(0x2E) || PadDown(raw, kPadX);
            case Nav::Count: break;
            }
            return false;
        }

        struct KeyName
        {
            const char* name;
            int code;
        };
        // The names rl::RlControls understands (RlControls.cpp), generic modifiers
        const KeyName kNamedKeys[] = {{"Space", 0x20}, {"Tab", 0x09}, {"Enter", 0x0D}, {"Backspace", 0x08}, {"Shift", 0x10}, {"Ctrl", 0x11}, {"Alt", 0x12},
                                      {"Up", 0x26}, {"Down", 0x28}, {"Left", 0x25}, {"Right", 0x27}, {"Home", 0x24}, {"End", 0x23}, {"Insert", 0x2D},
                                      {"PageUp", 0x21}, {"PageDown", 0x22}, {"CapsLock", 0x14}};

        const char* const kPadButtonTokens[] = {"PadA", "PadB", "PadX", "PadY", "PadLB", "PadRB", "PadBack", "PadStart", "PadLeftStick", "PadRightStick",
                                                "PadDpadUp", "PadDpadDown", "PadDpadLeft", "PadDpadRight"};
        const char* const kPadAxisTokens[] = {"PadLeftX", "PadLeftY", "PadRightX", "PadRightY", "PadLT", "PadRT"};
    }

    void InputMapper::Update(const RawInput& raw, const float deltaTime, const bool textEditing)
    {
        previous_ = raw_;
        raw_ = raw;
        typed_ = raw.typed;
        wheel_ = raw.mouseWheel;

        // keys: edges
        bool anyKeyEdge = false;
        for (int k = 0; k < 256; ++k)
        {
            keyPressed_[k] = raw_.keys[k] && !previous_.keys[k];
            anyKeyEdge = anyKeyEdge || keyPressed_[k];
        }

        // the mouse
        const float dx = raw_.mousePosition.x - mouse_.x;
        const float dy = raw_.mousePosition.y - mouse_.y;
        mouseMoved_ = raw_.windowActive && (dx * dx + dy * dy) > 0.25f;
        mouse_ = raw_.mousePosition;
        mousePressed_ = raw_.mouse[0] && !previous_.mouse[0];
        mouseReleased_ = !raw_.mouse[0] && previous_.mouse[0];
        mouseDown_ = raw_.mouse[0];

        // navigation with the repeat of a held direction
        for (int n = 0; n < static_cast<int>(Nav::Count); ++n)
        {
            const Nav nav = static_cast<Nav>(n);
            const bool down = raw_.windowActive && NavDown(raw_, nav, textEditing);
            bool fire = false;
            if (down && !held_[n])
            {
                fire = true;
                repeat_[n] = kRepeatDelay;
            }
            else if (down && nav <= Nav::Right)
            {
                repeat_[n] -= deltaTime;
                while (repeat_[n] <= 0.0f)
                {
                    fire = true;
                    repeat_[n] += kRepeatInterval;
                }
            }
            held_[n] = down;
            pressed_[n] = fire;
        }

        // the device used last: a pad press or a stick past the half, or the keyboard / the mouse
        bool padActivity = (raw_.padButtons & ~previous_.padButtons) != 0;
        for (int a = 0; a < 4 && !padActivity; ++a)
        {
            padActivity = std::fabs(raw_.padAxes[a]) > 0.6f && std::fabs(previous_.padAxes[a]) <= 0.6f;
        }
        for (int a = 4; a < 6 && !padActivity; ++a)
        {
            padActivity = raw_.padAxes[a] > 0.5f && previous_.padAxes[a] <= 0.5f;
        }
        const bool pointerActivity = mouseMoved_ || mousePressed_ || raw_.mouse[1] != previous_.mouse[1] || std::fabs(wheel_) > 0.0f;
        if (raw_.padConnected && padActivity)
        {
            device_ = Device::Gamepad;
        }
        else if (anyKeyEdge || pointerActivity)
        {
            device_ = Device::KeyboardMouse;
        }
    }

    std::string TokenDisplayName(std::string_view token)
    {
        if (!token.empty() && token.front() == '-')
        {
            token.remove_prefix(1);
        }
        struct Pair
        {
            const char* token;
            const char* shown;
        };
        static const Pair kShown[] = {
            {"MouseLeft", "Left mouse"}, {"MouseRight", "Right mouse"}, {"MouseMiddle", "Middle mouse"},
            {"PadA", "A"}, {"PadB", "B"}, {"PadX", "X"}, {"PadY", "Y"}, {"PadLB", "LB"}, {"PadRB", "RB"}, {"PadBack", "Back"}, {"PadStart", "Start"},
            {"PadLeftStick", "L stick"}, {"PadRightStick", "R stick"}, {"PadDpadUp", "D-pad up"}, {"PadDpadDown", "D-pad down"}, {"PadDpadLeft", "D-pad left"},
            {"PadDpadRight", "D-pad right"}, {"PadLeftX", "Left stick"}, {"PadLeftY", "Left stick"}, {"PadRightX", "Right stick"}, {"PadRightY", "Right stick"},
            {"PadLT", "LT"}, {"PadRT", "RT"}, {"PadLeftTrigger", "LT"}, {"PadRightTrigger", "RT"},
        };
        for (const Pair& pair : kShown)
        {
            if (token == pair.token)
            {
                return pair.shown;
            }
        }
        return std::string(token);
    }

    std::string CaptureToken(const RawInput& previous, const RawInput& now, const bool padOnly)
    {
        if (padOnly)
        {
            if (!now.padConnected)
            {
                return {};
            }
            for (int b = 0; b < 14; ++b)
            {
                if (((now.padButtons >> b) & 1u) != 0 && ((previous.padButtons >> b) & 1u) == 0)
                {
                    return kPadButtonTokens[b];
                }
            }
            for (int a = 0; a < 6; ++a)
            {
                const bool trigger = a >= 4;
                const float high = trigger ? 0.5f : 0.6f;
                const float low = trigger ? 0.3f : 0.4f;
                const float v = now.padAxes[a];
                if (std::fabs(v) > high && std::fabs(previous.padAxes[a]) < low)
                {
                    return std::string(v < 0.0f ? "-" : "") + kPadAxisTokens[a];
                }
            }
            return {};
        }

        const char* const mouseTokens[3] = {"MouseLeft", "MouseRight", "MouseMiddle"};
        for (int m = 0; m < 3; ++m)
        {
            if (now.mouse[m] && !previous.mouse[m])
            {
                return mouseTokens[m];
            }
        }
        for (int k = 8; k < 256; ++k)
        {
            if (!now.keys[k] || previous.keys[k] || k == 0x1B || k == 0x2E)
            {
                continue;
            }
            if ((k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9'))
            {
                return std::string(1, static_cast<char>(k));
            }
            if (k >= 0x70 && k <= 0x7B)
            {
                return "F" + std::to_string(k - 0x70 + 1);
            }
            for (const KeyName& named : kNamedKeys)
            {
                if (named.code == k)
                {
                    return named.name;
                }
            }
        }
        return {};
    }
}
