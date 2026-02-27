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
}