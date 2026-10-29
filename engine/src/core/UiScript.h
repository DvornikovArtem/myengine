// UiScript.h
// The scripted input of the tests of the game (MYENGINE_UI_SCRIPT=<file>, docs/game/rl-ui.md): a text file of timed lines drives the keys, the mouse and the
// pad of the screens and of the match, takes screenshots and quits, so that the whole way through the menus can be run and photographed without a person.
//
//   <seconds> key <name> [down|up|tap]        Enter Esc Up Down Left Right Tab Space Del Backspace Shift A..Z 0..9 F1..F12   (tap is the default)
//   <seconds> pad <button> [down|up|tap]      A B X Y LB RB Back Start Up Down Left Right
//   <seconds> axis <name> <value>             LeftX LeftY RightX RightY LT RT  (-1..1, 0..1)
//   <seconds> mouse <x> <y>                   window pixels
//   <seconds> click [left|right] [down|up]    a click is a press, a pause and a release
//   <seconds> wheel <notches>
//   <seconds> shot <file>                     a screenshot of the next frame (.tga)
//   <seconds> note <text>                     a line in the log
//   <seconds> game <command> [...]            a command for the game system (RlGameSystem::DebugCommand: goal-blue, goal-orange, demolish ...)
//   <seconds> editor <command> [...]          a command for the editor (SceneEditor::RunEditorCommand: build <game> ..., build-cancel, build-open; docs/editor/build-panel.md)
//   <seconds> quit
//
// Lines starting with # are comments. The times are seconds since the first frame of the script and are not sorted: they run in the order of the times.

#pragma once

#include <functional>
#include <string>
#include <vector>

#include <myengine/ui/rl/RlUiInput.h>

namespace myengine::core
{
    class UiScript
    {
    public:
        struct Sink
        {
            std::function<void(int virtualKey, bool down)> key;
            std::function<void(int button, bool down)> mouseButton;  // 0 left, 1 right
            std::function<void(const std::string& path)> screenshot;
            std::function<void(const std::string& text)> note;
            std::function<void(const std::string& command)> game;
            std::function<void(const std::string& command)> editor;
            std::function<void()> quit;
        };

        bool Load(const std::string& text, std::string& error);
        // Runs what is due at `seconds` since the start of the script
        void Advance(double deltaSeconds, const Sink& sink);
        // The devices as the script holds them: added to what the real ones say
        void ApplyTo(rlui::RawInput& raw) const;
        bool Finished() const { return next_ >= commands_.size() && pendingReleases_.empty(); }
        double Time() const { return time_; }

    private:
        struct Command
        {
            double time = 0.0;
            std::string verb;
            std::vector<std::string> args;
        };
        struct Release
        {
            double time = 0.0;
            bool pad = false;
            int code = 0;
            bool mouse = false;
        };

        void Execute(const Command& command, const Sink& sink);
        void SetKey(int code, bool down, const Sink& sink);

        std::vector<Command> commands_;
        std::size_t next_ = 0;
        double time_ = 0.0;
        std::vector<Release> pendingReleases_;
        bool keys_[256] = {};
        std::uint32_t padButtons_ = 0;
        float padAxes_[6] = {};
        bool mouse_[3] = {};
        rlui::Vec2 mousePosition_{};
        float wheel_ = 0.0f;
        std::string typed_;   // `text <utf8>`: what is typed this frame
    };
}
