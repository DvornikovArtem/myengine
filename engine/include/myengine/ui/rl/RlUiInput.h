// RlUiInput.h
//
// The input of the menu: the devices in, the navigation events out (§5 of the spec). The focus model is the kit's own (not ImGui Nav):
// keyboard, mouse and the pad give the same few events, and the kit repeats a held direction (0.35 s delay, then 12 Hz). The
// Host fills RawInput from the window (keys, mouse, typed text) and XInput (the pad); a screen only reads what Frame offers.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <myengine/ui/rl/RlUiCommon.h>

namespace myengine::rlui
{
    // One frame of the devices. Key codes are Windows virtual-key codes. The pad part has the layout of rl::RlRawInput (the bits
    // of RlPadButton, the order of RlPadAxis), so the game can copy it field by field.
    struct RawInput
    {
        bool keys[256] = {};
        bool mouse[3] = {};          // left, right, middle
        std::uint32_t padButtons = 0;
        float padAxes[6] = {};       // left X, left Y (up +), right X, right Y (up +), left trigger, right trigger (0..1)
        bool padConnected = false;
        Vec2 mousePosition{};        // window pixels
        float mouseWheel = 0.0f;     // notches this frame, up +
        std::string typed;           // the text typed this frame (UTF-8): the field of the address
        bool windowActive = true;
    };

    enum PadButtonBit : int
    {
        kPadA = 0, kPadB, kPadX, kPadY, kPadLB, kPadRB, kPadBack, kPadStart, kPadLeftStick, kPadRightStick,
        kPadDpadUp, kPadDpadDown, kPadDpadLeft, kPadDpadRight
    };

    // The events of the navigation. Pressed(nav) is true on the frame of the press and on every repeat of a held direction.
    enum class Nav : std::uint8_t
    {
        Up,
        Down,
        Left,
        Right,
        Confirm,    // Enter, Space, A
        Back,       // Esc, B
        TabPrev,    // Q, LB
        TabNext,    // E, RB
        ResetRow,   // R, Y
        Clear,      // Del, X
        Count
    };

    class InputMapper
    {
    public:
        // Once per frame, before any widget. `textEditing`: a field of text has the focus: the letters W A S D Q E R are text then, not navigation.
        void Update(const RawInput& raw, float deltaTime, bool textEditing = false);

        bool Pressed(Nav nav) const { return pressed_[static_cast<int>(nav)]; }
        bool Held(Nav nav) const { return held_[static_cast<int>(nav)]; }
        // A widget that used an event takes it, so that nobody else (the focus move, a tab switch) acts on it too
        void Consume(Nav nav) { pressed_[static_cast<int>(nav)] = false; }

        Device LastDevice() const { return device_; }

        Vec2 MousePosition() const { return mouse_; }
        bool MouseMoved() const { return mouseMoved_; }       // the pointer moved since the last frame (hover moves the focus)
        bool MousePressed() const { return mousePressed_; }   // left button went down this frame
        bool MouseDown() const { return mouseDown_; }
        bool MouseReleased() const { return mouseReleased_; }
        float Wheel() const { return wheel_; }
        const std::string& Typed() const { return typed_; }   // text typed this frame (UTF-8)
        bool KeyPressed(int virtualKey) const { return virtualKey >= 0 && virtualKey < 256 && keyPressed_[virtualKey]; }  // an edge, no repeat
        const RawInput& Raw() const { return raw_; }
        const RawInput& PreviousRaw() const { return previous_; }

    private:
        RawInput raw_;
        RawInput previous_;
        bool pressed_[static_cast<int>(Nav::Count)] = {};
        bool held_[static_cast<int>(Nav::Count)] = {};
        float repeat_[static_cast<int>(Nav::Count)] = {};
        bool keyPressed_[256] = {};
        Device device_ = Device::KeyboardMouse;
        Vec2 mouse_{};
        bool mouseMoved_ = false;
        bool mousePressed_ = false;
        bool mouseDown_ = false;
        bool mouseReleased_ = false;
        float wheel_ = 0.0f;
        std::string typed_;
    };

    // ---- the names of the controls (the rebind screen)

    // What a binding looks like on a plate: "MouseRight" -> "Right mouse", "PadRT" -> "RT", "-S" -> "S", "Esc" -> "Esc"
    std::string TokenDisplayName(std::string_view token);

    // The control the player pressed between two frames, as a token of rl::RlControls ("W", "Space", "MouseLeft", "PadA", "PadRT"), or an
    // empty string when nothing new was pressed. `padOnly` listens to the pad (a stick or a trigger past the half counts), otherwise to the keys
    // and mouse buttons. Esc and Del are not returned here: the dialog of the rebind handles them.
    std::string CaptureToken(const RawInput& previous, const RawInput& now, bool padOnly);
}
