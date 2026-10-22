// UiScript.cpp

#include "UiScript.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace myengine::core
{
    namespace
    {
        int KeyCode(const std::string& name)
        {
            struct Named
            {
                const char* name;
                int code;
            };
            static const Named kNamed[] = {{"Enter", VK_RETURN}, {"Esc", VK_ESCAPE}, {"Escape", VK_ESCAPE}, {"Up", VK_UP}, {"Down", VK_DOWN}, {"Left", VK_LEFT},
                                           {"Right", VK_RIGHT}, {"Tab", VK_TAB}, {"Space", VK_SPACE}, {"Del", VK_DELETE}, {"Delete", VK_DELETE}, {"Backspace", VK_BACK},
                                           {"Shift", VK_SHIFT}};
            for (const Named& n : kNamed)
            {
                if (name == n.name)
                {
                    return n.code;
                }
            }
            if (name.size() == 1 && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= '0' && name[0] <= '9')))
            {
                return name[0];
            }
            if (name.size() >= 2 && name[0] == 'F' && std::atoi(name.c_str() + 1) >= 1 && std::atoi(name.c_str() + 1) <= 12)
            {
                return VK_F1 + std::atoi(name.c_str() + 1) - 1;
            }
            return -1;
        }

        int PadCode(const std::string& name)
        {
            static const char* const kNames[] = {"A", "B", "X", "Y", "LB", "RB", "Back", "Start", "LeftStick", "RightStick", "Up", "Down", "Left", "Right"};
            for (int i = 0; i < 14; ++i)
            {
                if (name == kNames[i])
                {
                    return i;
                }
            }
            return -1;
        }

        int AxisCode(const std::string& name)
        {
            static const char* const kNames[] = {"LeftX", "LeftY", "RightX", "RightY", "LT", "RT"};
            for (int i = 0; i < 6; ++i)
            {
                if (name == kNames[i])
                {
                    return i;
                }
            }
            return -1;
        }
    }

    bool UiScript::Load(const std::string& text, std::string& error)
    {
        commands_.clear();
        next_ = 0;
        time_ = 0.0;
        std::istringstream stream(text);
        std::string line;
        int number = 0;
        while (std::getline(stream, line))
        {
            ++number;
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            std::istringstream words(line);
            std::string first;
            if (!(words >> first) || first[0] == '#')
            {
                continue;
            }
            Command command;
            char* end = nullptr;
            command.time = std::strtod(first.c_str(), &end);
            if (end == first.c_str() || !(words >> command.verb))
            {
                error = "line " + std::to_string(number) + ": expected '<seconds> <command> ...'";
                return false;
            }
            std::string rest;
            std::getline(words, rest);
            if (command.verb == "note" || command.verb == "shot" || command.verb == "game")
            {
                const std::size_t start = rest.find_first_not_of(" \t");
                command.args.push_back(start == std::string::npos ? std::string() : rest.substr(start));
            }
            else
            {
                std::istringstream args(rest);
                std::string arg;
                while (args >> arg)
                {
                    command.args.push_back(arg);
                }
            }
            commands_.push_back(command);
        }
        std::stable_sort(commands_.begin(), commands_.end(), [](const Command& a, const Command& b) { return a.time < b.time; });
        return true;
    }

    void UiScript::SetKey(const int code, const bool down, const Sink& sink)
    {
        if (code < 0 || code >= 256)
        {
            return;
        }
        keys_[code] = down;
        if (sink.key)
        {
            sink.key(code, down);
        }
    }

    void UiScript::Execute(const Command& c, const Sink& sink)
    {
        const std::string mode = c.args.size() >= 2 ? c.args[1] : std::string("tap");
        if (c.verb == "key" && !c.args.empty())
        {
            const int code = KeyCode(c.args[0]);
            if (mode != "up")
            {
                SetKey(code, true, sink);
            }
            if (mode == "up")
            {
                SetKey(code, false, sink);
            }
            else if (mode == "tap")
            {
                pendingReleases_.push_back({time_ + 0.12, false, code, false});
            }
        }
        else if (c.verb == "pad" && !c.args.empty())
        {
            const int code = PadCode(c.args[0]);
            if (code >= 0)
            {
                if (mode != "up")
                {
                    padButtons_ |= 1u << code;
                }
                if (mode == "up")
                {
                    padButtons_ &= ~(1u << code);
                }
                else if (mode == "tap")
                {
                    pendingReleases_.push_back({time_ + 0.12, true, code, false});
                }
            }
        }
        else if (c.verb == "axis" && c.args.size() >= 2)
        {
            const int code = AxisCode(c.args[0]);
            if (code >= 0)
            {
                padAxes_[code] = static_cast<float>(std::atof(c.args[1].c_str()));
            }
        }
        else if (c.verb == "mouse" && c.args.size() >= 2)
        {
            mousePosition_ = {static_cast<float>(std::atof(c.args[0].c_str())), static_cast<float>(std::atof(c.args[1].c_str()))};
        }
        else if (c.verb == "click")
        {
            const std::string button = c.args.empty() ? std::string("left") : c.args[0];
            const int index = button == "right" ? 1 : 0;
            const std::string phase = c.args.size() >= 2 ? c.args[1] : std::string("tap");
            if (phase != "up")
            {
                mouse_[index] = true;
                if (sink.mouseButton)
                {
                    sink.mouseButton(index, true);
                }
            }
            if (phase == "up")
            {
                mouse_[index] = false;
                if (sink.mouseButton)
                {
                    sink.mouseButton(index, false);
                }
            }
            else if (phase == "tap")
            {
                pendingReleases_.push_back({time_ + 0.12, false, index, true});
            }
        }
        else if (c.verb == "wheel" && !c.args.empty())
        {
            wheel_ += static_cast<float>(std::atof(c.args[0].c_str()));
        }
        else if (c.verb == "shot" && sink.screenshot && !c.args.empty())
        {
            sink.screenshot(c.args[0]);
        }
        else if (c.verb == "note" && sink.note)
        {
            sink.note(c.args.empty() ? std::string() : c.args[0]);
        }
        else if (c.verb == "game" && sink.game && !c.args.empty())
        {
            sink.game(c.args[0]);
        }
        else if (c.verb == "quit" && sink.quit)
        {
            sink.quit();
        }
    }

    void UiScript::Advance(const double deltaSeconds, const Sink& sink)
    {
        wheel_ = 0.0f;
        time_ += deltaSeconds;
        for (std::size_t i = 0; i < pendingReleases_.size();)
        {
            const Release release = pendingReleases_[i];
            if (release.time > time_)
            {
                ++i;
                continue;
            }
            if (release.mouse)
            {
                mouse_[release.code] = false;
                if (sink.mouseButton)
                {
                    sink.mouseButton(release.code, false);
                }
            }
            else if (release.pad)
            {
                padButtons_ &= ~(1u << release.code);
            }
            else
            {
                SetKey(release.code, false, sink);
            }
            pendingReleases_.erase(pendingReleases_.begin() + static_cast<std::ptrdiff_t>(i));
        }
        // a command that is due runs on this frame; the ones of the same instant run together
        while (next_ < commands_.size() && commands_[next_].time <= time_)
        {
            Execute(commands_[next_], sink);
            ++next_;
        }
    }

    void UiScript::ApplyTo(rlui::RawInput& raw) const
    {
        for (int i = 0; i < 256; ++i)
        {
            raw.keys[i] = raw.keys[i] || keys_[i];
        }
        for (int i = 0; i < 3; ++i)
        {
            raw.mouse[i] = raw.mouse[i] || mouse_[i];
        }
        raw.padButtons |= padButtons_;
        bool anyPad = padButtons_ != 0;
        for (int i = 0; i < 6; ++i)
        {
            if (padAxes_[i] != 0.0f)
            {
                raw.padAxes[i] = padAxes_[i];
                anyPad = true;
            }
        }
        raw.padConnected = raw.padConnected || anyPad;
        if (mousePosition_.x != 0.0f || mousePosition_.y != 0.0f)
        {
            raw.mousePosition = mousePosition_;
        }
        raw.mouseWheel += wheel_;
        raw.windowActive = true;
    }
}
