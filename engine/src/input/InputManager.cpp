// InputManager.cpp

#include <myengine/input/InputManager.h>

namespace myengine::input
{
    void InputManager::BeginFrame()
    {
        keyPressed_.fill(false);
        keyReleased_.fill(false);
        mouseButtonsPressed_.fill(false);
        mouseButtonsReleased_.fill(false);
    }

    void InputManager::SetActiveWindow(const core::WindowId windowId)
    {
        activeWindowId_ = windowId;
    }

    void InputManager::OnKeyDown(const std::uint32_t key)
    {
        if (key < keys_.size())
        {
            if (!keys_[key])
            {
                keyPressed_[key] = true;
            }
            keys_[key] = true;
        }
    }

    void InputManager::OnKeyUp(const std::uint32_t key)
    {
        if (key < keys_.size())
        {
            if (keys_[key])
            {
                keyReleased_[key] = true;
            }
            keys_[key] = false;
        }
    }

    void InputManager::OnMouseDown(const core::MouseButton button)
    {
        const auto index = static_cast<std::size_t>(button);
        if (index < mouseButtons_.size())
        {
            if (!mouseButtons_[index])
            {
                mouseButtonsPressed_[index] = true;
            }
            mouseButtons_[index] = true;
        }
    }

    void InputManager::OnMouseUp(const core::MouseButton button)
    {
        const auto index = static_cast<std::size_t>(button);
        if (index < mouseButtons_.size())
        {
            if (mouseButtons_[index])
            {
                mouseButtonsReleased_[index] = true;
            }
            mouseButtons_[index] = false;
        }
    }

    void InputManager::OnMouseWheel(const int delta)
    {
        mouseWheelAccumulated_ += delta;
    }

    void InputManager::OnMouseMove(const int x, const int y)
    {
        if (hasMousePosition_)
        {
            mouseDeltaX_ += x - lastMouseX_;
            mouseDeltaY_ += y - lastMouseY_;
        }

        lastMouseX_ = x;
        lastMouseY_ = y;
        hasMousePosition_ = true;
    }

    void InputManager::AddMouseDelta(const int deltaX, const int deltaY)
    {
        mouseDeltaX_ += deltaX;
        mouseDeltaY_ += deltaY;
    }

    void InputManager::SetMousePositionReference(const int x, const int y)
    {
        lastMouseX_ = x;
        lastMouseY_ = y;
        hasMousePosition_ = true;
    }

    bool InputManager::IsKeyDown(const std::uint32_t key) const {
        if (key >= keys_.size())
        {
            return false;
        }
        return keys_[key];
    }

    bool InputManager::WasKeyPressed(const std::uint32_t key) const
    {
        if (key >= keyPressed_.size())
        {
            return false;
        }

        return keyPressed_[key];
    }

    bool InputManager::WasKeyReleased(const std::uint32_t key) const
    {
        if (key >= keyReleased_.size())
        {
            return false;
        }

        return keyReleased_[key];
    }

    bool InputManager::IsMouseDown(const core::MouseButton button) const {
        const auto index = static_cast<std::size_t>(button);
        if (index >= mouseButtons_.size())
        {
            return false;
        }
        return mouseButtons_[index];
    }

    bool InputManager::WasMousePressed(const core::MouseButton button) const
    {
        const auto index = static_cast<std::size_t>(button);
        if (index >= mouseButtonsPressed_.size())
        {
            return false;
        }

        return mouseButtonsPressed_[index];
    }

    bool InputManager::WasMouseReleased(const core::MouseButton button) const
    {
        const auto index = static_cast<std::size_t>(button);
        if (index >= mouseButtonsReleased_.size())
        {
            return false;
        }

        return mouseButtonsReleased_[index];
    }

    int InputManager::ConsumeMouseWheelSteps()
    {
        const int steps = mouseWheelAccumulated_ / kMouseWheelDelta;
        mouseWheelAccumulated_ -= steps * kMouseWheelDelta;
        return steps;
    }

    std::pair<int, int> InputManager::ConsumeMouseDelta()
    {
        const std::pair<int, int> delta{mouseDeltaX_, mouseDeltaY_};
        mouseDeltaX_ = 0;
        mouseDeltaY_ = 0;
        return delta;
    }

    void InputManager::ResetMouseTracking()
    {
        hasMousePosition_ = false;
        mouseDeltaX_ = 0;
        mouseDeltaY_ = 0;
    }

    void InputManager::BindAction(std::string action, const std::uint32_t key)
    {
        auto& bindings = actionBindings_[std::move(action)];
        bindings.push_back(ActionBinding{BindingDevice::Key, key});
    }

    void InputManager::BindMouseAction(std::string action, const core::MouseButton button)
    {
        auto& bindings = actionBindings_[std::move(action)];
        bindings.push_back(ActionBinding{BindingDevice::MouseButton, static_cast<std::uint32_t>(button)});
    }

    bool InputManager::IsActionDown(const std::string_view action) const
    {
        const auto it = actionBindings_.find(std::string(action));
        if (it == actionBindings_.end())
        {
            return false;
        }

        for (const auto& binding : it->second)
        {
            if (MatchesBindingDown(binding))
            {
                return true;
            }
        }

        return false;
    }

    bool InputManager::WasActionPressed(const std::string_view action) const
    {
        const auto it = actionBindings_.find(std::string(action));
        if (it == actionBindings_.end())
        {
            return false;
        }

        for (const auto& binding : it->second)
        {
            if (MatchesBindingPressed(binding))
            {
                return true;
            }
        }

        return false;
    }

    bool InputManager::WasActionReleased(const std::string_view action) const
    {
        const auto it = actionBindings_.find(std::string(action));
        if (it == actionBindings_.end())
        {
            return false;
        }

        for (const auto& binding : it->second)
        {
            if (MatchesBindingReleased(binding))
            {
                return true;
            }
        }

        return false;
    }

    core::WindowId InputManager::GetActiveWindowId() const
    {
        return activeWindowId_;
    }

    bool InputManager::MatchesBindingPressed(const ActionBinding& binding) const
    {
        if (binding.device == BindingDevice::Key)
        {
            return WasKeyPressed(binding.code);
        }

        return WasMousePressed(static_cast<core::MouseButton>(binding.code));
    }

    bool InputManager::MatchesBindingReleased(const ActionBinding& binding) const
    {
        if (binding.device == BindingDevice::Key)
        {
            return WasKeyReleased(binding.code);
        }

        return WasMouseReleased(static_cast<core::MouseButton>(binding.code));
    }

    bool InputManager::MatchesBindingDown(const ActionBinding& binding) const
    {
        if (binding.device == BindingDevice::Key)
        {
            return IsKeyDown(binding.code);
        }

        return IsMouseDown(static_cast<core::MouseButton>(binding.code));
    }
}