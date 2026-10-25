#pragma once

// A UObject the mod remembers across frames, identified so that a reused address is not taken for
// it: its GUObjectArray slot must still hold this address, not collected. Game thread.

#include <Unreal/UObject.hpp>

#include <cstdint>

#include "platform/guarded.h"
#include "players/object_slot.h"

namespace lss::players {

/// Whether `object` is a live UObject now: its internal index read under a guard (the pointer may
/// be stale), and that GUObjectArray slot holding this address, not unreachable or pending kill.
inline bool IsLiveObject(RC::Unreal::UObject* object) noexcept
{
    if (object == nullptr)
    {
        return false;
    }
    std::int32_t index = -1;
    __try
    {
        index = static_cast<std::int32_t>(object->GetInternalIndex());
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
    return LiveInSlot(index, object);
}

struct TrackedObject
{
    RC::Unreal::UObject* object = nullptr;
    std::int32_t         index  = -1;

    /// Only for an object known to be live now (e.g. IsLiveObject).
    static TrackedObject Of(RC::Unreal::UObject* live) noexcept
    {
        return {live, static_cast<std::int32_t>(live->GetInternalIndex())};
    }

    bool Alive() const noexcept { return object != nullptr && LiveInSlot(index, object); }
};

}  // namespace lss::players
