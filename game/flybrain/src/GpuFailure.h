#pragma once

#include <cstdint>
#include <stdexcept>
#include <sstream>

namespace flybrain::detail
{
    // Used only after submitted work can no longer be proven complete. The process owns this extra reference until exit.
    template <typename T>
    inline void RetainForProcess(T* object)
    {
        if (object) object->AddRef();
    }

    template <typename Retain, typename Work>
    inline void ProtectSubmitted(Retain&& retain, Work&& work)
    {
        try { work(); }
        catch (...) { retain(); throw; }
    }

    inline void CheckGpuResult(int32_t result, const char* operation, int32_t removedReason = 0)
    {
        if (result >= 0) return;
        std::ostringstream message;
        message << "Fly brain GPU: " << operation << " failed (0x" << std::hex << static_cast<uint32_t>(result) << ")";
        if (removedReason < 0) message << "; graphics device lost (0x" << static_cast<uint32_t>(removedReason) << ")";
        throw std::runtime_error(message.str());
    }

    inline void CheckMappedPointer(const void* mapped)
    {
        if (!mapped) throw std::runtime_error("Fly brain GPU: Map returned no data");
    }

    inline bool FenceDeviceLost(uint64_t completed)
    {
        return completed == UINT64_MAX;
    }
}
