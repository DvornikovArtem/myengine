// RlControls.h
// The controls of the Rocket League game as data: which keys, mouse buttons and pad buttons / axes do what. The defaults are the ones of
// Rocket League on a PC (left mouse button boosts, right one jumps, Space switches the ball cam, Shift is the powerslide / air roll) and
// of its Xbox controller layout; assets/data/rl_controls.json holds them and a file of the player (%APPDATA%/myengine/rl_controls.json)
// overrides single actions. The menu of G1 will edit the bindings through this class. No window, no input device in here: the game
// hands over a snapshot of the devices (RlRawInput) and gets the input of the car back.

#pragma once

#include <rl/RlTypes.h>

#include <cstdint>
#include <string>
#include <vector>

namespace rl
{
    // One frame of the devices. Key codes are Windows virtual-key codes (what the window messages carry).
    struct RlRawInput
    {
        bool keys[256] = {};
        bool mouse[3] = {};          // left, right, middle
        uint32_t padButtons = 0;     // bit i = RlPadButton i
        float padAxes[6] = {};       // left X, left Y (up +), right X, right Y (up +), left trigger, right trigger (0..1)
        bool padConnected = false;
    };

    enum RlPadButton : int
    {
        PadA = 0, PadB, PadX, PadY, PadLB, PadRB, PadBack, PadStart, PadLeftStick, PadRightStick, PadDpadUp, PadDpadDown, PadDpadLeft, PadDpadRight,
        PadButtonCount
    };

    enum RlPadAxis : int
    {
        PadLeftX = 0, PadLeftY, PadRightX, PadRightY, PadLeftTrigger, PadRightTrigger, PadAxisCount
    };

    enum class RlButtonAction : int
    {
        Boost = 0,
        Jump,
        BallCam,
        Powerslide,   // the handbrake on the ground, the air roll in the air
        Scoreboard,
        Menu,
        Count
    };

    enum class RlAxisAction : int
    {
        Throttle = 0,  // +1 forward
        Steer,         // +1 right (on the ground) / yaw (in the air)
        Pitch,         // +1 nose up (stick back)
        Roll,          // +1 roll right
        Count
    };

    // What the player holds this frame.
    struct RlControlState
    {
        RlCarInput car;               // the input of the car (the powerslide also sets handbrake and airRoll)
        bool buttons[static_cast<int>(RlButtonAction::Count)] = {};
        bool Down(RlButtonAction a) const { return buttons[static_cast<int>(a)]; }
    };

    class RlControls
    {
    public:
        static RlControls Defaults();

        // Reads json; actions that are not in it keep their bindings. Returns false with a message that names the action and the
        // binding when something is wrong (the object is left as it was).
        bool LoadFromText(const std::string& text, std::string* error = nullptr);
        bool LoadFromFile(const std::string& path, std::string* error = nullptr);
        // The defaults, then the shipped file, then the file of the player; a missing file is not an error, a broken one is.
        bool LoadLayered(const std::string& shippedPath, const std::string& userPath, std::string* error = nullptr);
        bool SaveToFile(const std::string& path, std::string* error = nullptr) const;
        std::string ToJson() const;

        // Replaces the bindings of an action by tokens such as "MouseLeft", "W", "-S", "PadRT" ("-" turns an axis binding round).
        bool SetButton(RlButtonAction action, const std::vector<std::string>& tokens, std::string* error = nullptr);
        bool SetAxis(RlAxisAction action, const std::vector<std::string>& tokens, std::string* error = nullptr);
        std::vector<std::string> ButtonTokens(RlButtonAction action) const;
        std::vector<std::string> AxisTokens(RlAxisAction action) const;

        RlControlState Evaluate(const RlRawInput& raw) const;

        float padDeadzone = 0.2f;

    private:
        struct Binding
        {
            enum Kind : int { Key, Mouse, PadButton, PadAxis };
            Kind kind = Key;
            int code = 0;
            float sign = 1.0f;
            std::string token;
        };

        static bool ParseToken(const std::string& token, Binding& out, std::string* error);
        float Value(const Binding& b, const RlRawInput& raw) const;

        std::vector<Binding> buttons_[static_cast<int>(RlButtonAction::Count)];
        std::vector<Binding> axes_[static_cast<int>(RlAxisAction::Count)];
    };

    // The frames are longer than a tick at a high refresh rate and shorter at a low one, and a key can go down and up again inside one
    // frame: what is pressed in a frame is kept until a tick takes it, so a tap is never lost. Press() takes what was pressed in a frame
    // (the controls evaluated on the "went down since the last frame" events), Take() gives the input of one tick: what is held, with
    // everything pressed since the last tick added (the larger value of an axis, any button). It is empty again after a Take().
    class RlInputLatch
    {
    public:
        void Press(const RlCarInput& tapped);
        RlCarInput Take(const RlCarInput& held);
        bool Pending() const { return pending_; }
        void Clear()
        {
            latched_ = RlCarInput{};
            pending_ = false;
        }

    private:
        RlCarInput latched_{};
        bool pending_ = false;
    };
}
