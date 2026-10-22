// RlControls.cpp

#include <rl/RlControls.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace rl
{
    namespace
    {
        using json = nlohmann::json;

        const char* const kButtonNames[static_cast<int>(RlButtonAction::Count)] = {"boost", "jump", "ballCam", "powerslide", "scoreboard", "menu"};
        const char* const kAxisNames[static_cast<int>(RlAxisAction::Count)] = {"throttle", "steer", "pitch", "roll"};
        const char* const kPadButtonNames[PadButtonCount] = {"PadA",         "PadB",         "PadX",         "PadY",         "PadLB",
                                                              "PadRB",        "PadBack",      "PadStart",     "PadLeftStick", "PadRightStick",
                                                              "PadDpadUp",    "PadDpadDown",  "PadDpadLeft",  "PadDpadRight"};
        const char* const kPadAxisNames[PadAxisCount] = {"PadLeftX", "PadLeftY", "PadRightX", "PadRightY", "PadLT", "PadRT"};

        struct KeyName
        {
            const char* name;
            int code;
        };
        // Windows virtual-key codes; Shift, Ctrl and Alt are the generic ones the window messages carry
        const KeyName kKeyNames[] = {
            {"Space", 0x20}, {"Tab", 0x09},   {"Enter", 0x0D}, {"Esc", 0x1B},    {"Backspace", 0x08}, {"Shift", 0x10}, {"LShift", 0x10}, {"RShift", 0x10},
            {"Ctrl", 0x11},  {"LCtrl", 0x11}, {"RCtrl", 0x11}, {"Alt", 0x12},    {"Up", 0x26},        {"Down", 0x28},  {"Left", 0x25},   {"Right", 0x27},
            {"Home", 0x24},  {"End", 0x23},   {"Insert", 0x2D}, {"Delete", 0x2E}, {"PageUp", 0x21},    {"PageDown", 0x22}, {"CapsLock", 0x14},
        };

        std::string Lower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        // "name" -> virtual key (letters, digits, F1..F12, the names above); -1 when unknown
        int KeyCode(const std::string& name)
        {
            const std::string lower = Lower(name);
            if (lower.size() == 1)
            {
                const char c = lower[0];
                if (c >= 'a' && c <= 'z')
                {
                    return 'A' + (c - 'a');
                }
                if (c >= '0' && c <= '9')
                {
                    return c;
                }
            }
            if (lower.size() >= 2 && lower[0] == 'f')
            {
                const int n = std::atoi(lower.c_str() + 1);
                if (n >= 1 && n <= 12 && lower.find_first_not_of("0123456789", 1) == std::string::npos)
                {
                    return 0x70 + n - 1;
                }
            }
            for (const KeyName& k : kKeyNames)
            {
                if (Lower(k.name) == lower)
                {
                    return k.code;
                }
            }
            return -1;
        }

        // the canonical name of a key code, for ToJson
        std::string KeyToken(int code)
        {
            if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9'))
            {
                return std::string(1, static_cast<char>(code));
            }
            if (code >= 0x70 && code <= 0x7B)
            {
                return "F" + std::to_string(code - 0x70 + 1);
            }
            for (const KeyName& k : kKeyNames)
            {
                if (k.code == code)
                {
                    return k.name;
                }
            }
            return "Key" + std::to_string(code);
        }
    }

    bool RlControls::ParseToken(const std::string& tokenIn, Binding& out, std::string* error)
    {
        std::string token = tokenIn;
        float sign = 1.0f;
        if (!token.empty() && token[0] == '-')
        {
            sign = -1.0f;
            token.erase(0, 1);
        }
        out = Binding{};
        out.sign = sign;
        const std::string lower = Lower(token);
        const auto finish = [&](Binding::Kind kind, int code, const std::string& canonical)
        {
            out.kind = kind;
            out.code = code;
            out.token = (sign < 0.0f ? "-" : "") + canonical;
            return true;
        };
        if (lower == "mouseleft") return finish(Binding::Mouse, 0, "MouseLeft");
        if (lower == "mouseright") return finish(Binding::Mouse, 1, "MouseRight");
        if (lower == "mousemiddle") return finish(Binding::Mouse, 2, "MouseMiddle");
        for (int i = 0; i < PadButtonCount; ++i)
        {
            if (lower == Lower(kPadButtonNames[i])) return finish(Binding::PadButton, i, kPadButtonNames[i]);
        }
        for (int i = 0; i < PadAxisCount; ++i)
        {
            if (lower == Lower(kPadAxisNames[i])) return finish(Binding::PadAxis, i, kPadAxisNames[i]);
        }
        // the triggers also by their long names
        if (lower == "padlefttrigger") return finish(Binding::PadAxis, PadLeftTrigger, "PadLT");
        if (lower == "padrighttrigger") return finish(Binding::PadAxis, PadRightTrigger, "PadRT");
        const int key = KeyCode(token);
        if (key >= 0)
        {
            return finish(Binding::Key, key, KeyToken(key));
        }
        if (error != nullptr)
        {
            *error = "unknown key, mouse button or pad control '" + tokenIn + "'";
        }
        return false;
    }

    float RlControls::Value(const Binding& b, const RlRawInput& raw) const
    {
        float v = 0.0f;
        switch (b.kind)
        {
        case Binding::Key: v = raw.keys[b.code & 255] ? 1.0f : 0.0f; break;
        case Binding::Mouse: v = raw.mouse[b.code] ? 1.0f : 0.0f; break;
        case Binding::PadButton: v = ((raw.padButtons >> b.code) & 1u) != 0 ? 1.0f : 0.0f; break;
        case Binding::PadAxis:
        {
            v = raw.padAxes[b.code];
            // sticks have a dead zone, triggers only a small one
            const float zone = (b.code == PadLeftTrigger || b.code == PadRightTrigger) ? 0.05f : padDeadzone;
            const float magnitude = std::fabs(v);
            v = magnitude <= zone ? 0.0f : std::copysign((magnitude - zone) / (1.0f - zone), v);
            break;
        }
        }
        return v * b.sign;
    }

    RlControlState RlControls::Evaluate(const RlRawInput& raw) const
    {
        RlControlState state;
        for (int a = 0; a < static_cast<int>(RlButtonAction::Count); ++a)
        {
            for (const Binding& b : buttons_[a])
            {
                if (Value(b, raw) > 0.5f)
                {
                    state.buttons[a] = true;
                    break;
                }
            }
        }
        float axes[static_cast<int>(RlAxisAction::Count)] = {};
        for (int a = 0; a < static_cast<int>(RlAxisAction::Count); ++a)
        {
            float sum = 0.0f;
            for (const Binding& b : axes_[a])
            {
                sum += Value(b, raw);
            }
            axes[a] = std::clamp(sum, -1.0f, 1.0f);
        }
        state.car.throttle = axes[static_cast<int>(RlAxisAction::Throttle)];
        state.car.steer = axes[static_cast<int>(RlAxisAction::Steer)];
        state.car.pitch = axes[static_cast<int>(RlAxisAction::Pitch)];
        state.car.roll = axes[static_cast<int>(RlAxisAction::Roll)];
        state.car.boost = state.Down(RlButtonAction::Boost);
        state.car.jump = state.Down(RlButtonAction::Jump);
        state.car.handbrake = state.Down(RlButtonAction::Powerslide);
        state.car.airRoll = state.Down(RlButtonAction::Powerslide);
        return state;
    }

    bool RlControls::SetButton(RlButtonAction action, const std::vector<std::string>& tokens, std::string* error)
    {
        std::vector<Binding> parsed;
        for (const std::string& token : tokens)
        {
            Binding b;
            std::string problem;
            if (!ParseToken(token, b, &problem))
            {
                if (error != nullptr)
                {
                    *error = std::string("rl_controls: action '") + kButtonNames[static_cast<int>(action)] + "': " + problem;
                }
                return false;
            }
            parsed.push_back(b);
        }
        buttons_[static_cast<int>(action)] = std::move(parsed);
        return true;
    }

    bool RlControls::SetAxis(RlAxisAction action, const std::vector<std::string>& tokens, std::string* error)
    {
        std::vector<Binding> parsed;
        for (const std::string& token : tokens)
        {
            Binding b;
            std::string problem;
            if (!ParseToken(token, b, &problem))
            {
                if (error != nullptr)
                {
                    *error = std::string("rl_controls: axis '") + kAxisNames[static_cast<int>(action)] + "': " + problem;
                }
                return false;
            }
            parsed.push_back(b);
        }
        axes_[static_cast<int>(action)] = std::move(parsed);
        return true;
    }

    std::vector<std::string> RlControls::ButtonTokens(RlButtonAction action) const
    {
        std::vector<std::string> out;
        for (const Binding& b : buttons_[static_cast<int>(action)])
        {
            out.push_back(b.token);
        }
        return out;
    }

    std::vector<std::string> RlControls::AxisTokens(RlAxisAction action) const
    {
        std::vector<std::string> out;
        for (const Binding& b : axes_[static_cast<int>(action)])
        {
            out.push_back(b.token);
        }
        return out;
    }

    RlControls RlControls::Defaults()
    {
        RlControls c;
        // Rocket League on a PC: W/S gas and reverse (nose down / up in the air), A/D steer (yaw in the air), the left mouse button boosts, the
        // right one jumps, Space switches the ball cam, Shift is the powerslide and the air roll, Q/E roll, Tab the scoreboard, M the menu
        // (Esc stops Play in the editor). The Xbox layout of RL: RT gas, LT reverse, the left stick steers and pitches, A jump, B boost,
        // X powerslide / air roll, Y ball cam, LB / RB roll, Back the scoreboard, Start the menu.
        c.SetAxis(RlAxisAction::Throttle, {"W", "-S", "PadRT", "-PadLT"});
        c.SetAxis(RlAxisAction::Steer, {"D", "-A", "PadLeftX"});
        c.SetAxis(RlAxisAction::Pitch, {"-W", "S", "-PadLeftY"});
        c.SetAxis(RlAxisAction::Roll, {"E", "-Q", "PadRB", "-PadLB"});
        c.SetButton(RlButtonAction::Boost, {"MouseLeft", "PadB"});
        c.SetButton(RlButtonAction::Jump, {"MouseRight", "PadA"});
        c.SetButton(RlButtonAction::BallCam, {"Space", "PadY"});
        c.SetButton(RlButtonAction::Powerslide, {"Shift", "PadX"});
        c.SetButton(RlButtonAction::Scoreboard, {"Tab", "PadBack"});
        c.SetButton(RlButtonAction::Menu, {"M", "PadStart"});
        return c;
    }

    std::string RlControls::ToJson() const
    {
        nlohmann::ordered_json root = nlohmann::ordered_json::object();
        root["version"] = 1;
        root["padDeadzone"] = static_cast<double>(std::lround(padDeadzone * 1000.0f)) / 1000.0;
        nlohmann::ordered_json axes = nlohmann::ordered_json::object();
        for (int a = 0; a < static_cast<int>(RlAxisAction::Count); ++a)
        {
            axes[kAxisNames[a]] = AxisTokens(static_cast<RlAxisAction>(a));
        }
        nlohmann::ordered_json buttons = nlohmann::ordered_json::object();
        for (int a = 0; a < static_cast<int>(RlButtonAction::Count); ++a)
        {
            buttons[kButtonNames[a]] = ButtonTokens(static_cast<RlButtonAction>(a));
        }
        root["axes"] = axes;
        root["buttons"] = buttons;
        return root.dump(2);
    }

    bool RlControls::LoadFromText(const std::string& text, std::string* error)
    {
        const json parsed = json::parse(text, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object())
        {
            if (error != nullptr)
            {
                *error = "rl_controls: the text is not a json object";
            }
            return false;
        }
        RlControls next = *this;
        if (parsed.contains("padDeadzone") && parsed["padDeadzone"].is_number())
        {
            next.padDeadzone = std::clamp(parsed["padDeadzone"].get<float>(), 0.0f, 0.9f);
        }
        const auto readList = [&](const json& list, const char* name, std::vector<std::string>& out) -> bool
        {
            out.clear();
            if (!list.is_array())
            {
                if (error != nullptr)
                {
                    *error = std::string("rl_controls: '") + name + "' must be a list of bindings";
                }
                return false;
            }
            for (const json& item : list)
            {
                if (!item.is_string())
                {
                    if (error != nullptr)
                    {
                        *error = std::string("rl_controls: '") + name + "' has a binding that is not a string";
                    }
                    return false;
                }
                out.push_back(item.get<std::string>());
            }
            return true;
        };
        if (parsed.contains("axes") && parsed["axes"].is_object())
        {
            for (int a = 0; a < static_cast<int>(RlAxisAction::Count); ++a)
            {
                const auto it = parsed["axes"].find(kAxisNames[a]);
                std::vector<std::string> tokens;
                if (it != parsed["axes"].end() && (!readList(*it, kAxisNames[a], tokens) || !next.SetAxis(static_cast<RlAxisAction>(a), tokens, error)))
                {
                    return false;
                }
            }
        }
        if (parsed.contains("buttons") && parsed["buttons"].is_object())
        {
            for (int a = 0; a < static_cast<int>(RlButtonAction::Count); ++a)
            {
                const auto it = parsed["buttons"].find(kButtonNames[a]);
                std::vector<std::string> tokens;
                if (it != parsed["buttons"].end() && (!readList(*it, kButtonNames[a], tokens) || !next.SetButton(static_cast<RlButtonAction>(a), tokens, error)))
                {
                    return false;
                }
            }
        }
        *this = std::move(next);
        return true;
    }

    bool RlControls::LoadFromFile(const std::string& path, std::string* error)
    {
        std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
        if (!file)
        {
            if (error != nullptr)
            {
                *error = "rl_controls: cannot open " + path;
            }
            return false;
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string problem;
        if (!LoadFromText(buffer.str(), &problem))
        {
            if (error != nullptr)
            {
                *error = problem + " (" + path + ")";
            }
            return false;
        }
        return true;
    }

    bool RlControls::LoadLayered(const std::string& shippedPath, const std::string& userPath, std::string* error)
    {
        RlControls next = Defaults();
        for (const std::string& path : {shippedPath, userPath})
        {
            if (path.empty())
            {
                continue;
            }
            std::error_code ec;
            if (!std::filesystem::exists(std::filesystem::u8path(path), ec))
            {
                continue;  // not there is fine: the layer below stays
            }
            if (!next.LoadFromFile(path, error))
            {
                return false;
            }
        }
        *this = std::move(next);
        return true;
    }

    bool RlControls::SaveToFile(const std::string& path, std::string* error) const
    {
        std::error_code ec;
        const std::filesystem::path target = std::filesystem::u8path(path);
        if (target.has_parent_path())
        {
            std::filesystem::create_directories(target.parent_path(), ec);
        }
        std::ofstream file(target, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            if (error != nullptr)
            {
                *error = "rl_controls: cannot write " + path;
            }
            return false;
        }
        const std::string text = ToJson() + "\n";
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        return static_cast<bool>(file);
    }

    namespace
    {
        float Larger(float held, float tapped) { return std::fabs(tapped) > std::fabs(held) ? tapped : held; }
    }

    void RlInputLatch::Press(const RlCarInput& tapped)
    {
        latched_.throttle = Larger(latched_.throttle, tapped.throttle);
        latched_.steer = Larger(latched_.steer, tapped.steer);
        latched_.pitch = Larger(latched_.pitch, tapped.pitch);
        latched_.roll = Larger(latched_.roll, tapped.roll);
        latched_.boost = latched_.boost || tapped.boost;
        latched_.jump = latched_.jump || tapped.jump;
        latched_.handbrake = latched_.handbrake || tapped.handbrake;
        latched_.airRoll = latched_.airRoll || tapped.airRoll;
        pending_ = pending_ || tapped.throttle != 0.0f || tapped.steer != 0.0f || tapped.pitch != 0.0f || tapped.roll != 0.0f || tapped.boost || tapped.jump ||
                   tapped.handbrake || tapped.airRoll;
    }

    RlCarInput RlInputLatch::Take(const RlCarInput& held)
    {
        RlCarInput result = held;
        if (pending_)
        {
            result.throttle = Larger(held.throttle, latched_.throttle);
            result.steer = Larger(held.steer, latched_.steer);
            result.pitch = Larger(held.pitch, latched_.pitch);
            result.roll = Larger(held.roll, latched_.roll);
            result.boost = held.boost || latched_.boost;
            result.jump = held.jump || latched_.jump;
            result.handbrake = held.handbrake || latched_.handbrake;
            result.airRoll = held.airRoll || latched_.airRoll;
        }
        Clear();
        return result;
    }
}
