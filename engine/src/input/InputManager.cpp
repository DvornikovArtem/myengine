// InputManager.cpp

#include <myengine/input/InputManager.h>

namespace myengine::input
{
    void InputManager::OnKeyDown(const std::uint32_t key)
    {
        if (key < keys_.size())
        {
            keys_[key] = true;
        }
    }

    void InputManager::OnKeyUp(const std::uint32_t key)
    {
        if (key < keys_.size())
        {
            keys_[key] = false;
        }
    }

    void InputManager::OnMouseDown(const core::MouseButton button)
    {
        const auto index = static_cast<std::size_t>(button);
        if (index < mouseButtons_.size())
        {
            mouseButtons_[index] = true;
        }
    }

    void InputManager::OnMouseUp(const core::MouseButton button)
    {
        const auto index = static_cast<std::size_t>(button);
        if (index < mouseButtons_.size())
        {
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

    bool InputManager::IsMouseDown(const core::MouseButton button) const {
        const auto index = static_cast<std::size_t>(button);
        if (index >= mouseButtons_.size())
        {
            return false;
        }
        return mouseButtons_[index];
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
}
