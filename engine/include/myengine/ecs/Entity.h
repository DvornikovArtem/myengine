// Entity.h

#pragma once

#include <cstdint>

namespace myengine::ecs
{
    using EntityId = std::uint32_t;

    inline constexpr EntityId kInvalidEntity = 0;
}