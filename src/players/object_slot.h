#pragma once

// One GUObjectArray slot, read through the loader. UE4SS c0a8d4c (the build players get from Nexus Mods) exports no
// index-to-slot call, so a slot is read as the one-element range walk it does export: the walk hands over the
// object the slot holds, and skips an empty slot or one whose object is unreachable. The slot's record comes from
// the object itself (GetObjectItem), and "live" adds the loader's own validity test, which refuses a pending-kill
// slot. A stale pointer the mod kept is never read here: the slot's object is compared with it, nothing more. Game
// thread.

#include <Unreal/UObject.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include <cstdint>
#include <cstring>

namespace lss::players {

/// The reachable object slot `index` holds, or null (an empty slot, one being collected, or no such slot).
inline RC::Unreal::UObject* ObjectInSlot(std::int32_t index) noexcept
{
    if (index < 0)
    {
        return nullptr;
    }
    RC::Unreal::UObject* held = nullptr;
    try
    {
        // One slot: the walk from `index` to `index` calls back for that slot's object (none for an empty or
        // unreachable one; for an unreachable one it goes on to the next held slot, which the caller's compare
        // with its own pointer then refuses).
        RC::Unreal::UObjectGlobals::ForEachUObjectInRange(index, index,
                                                          [&](RC::Unreal::UObject* object, std::int32_t, std::int32_t) {
                                                              held = object;
                                                              return RC::LoopAction::Break;
                                                          });
    }
    catch (...)
    {
        return nullptr;
    }
    return held;
}

/// Slot `index` still holds `object`, reachable and not pending kill.
inline bool LiveInSlot(std::int32_t index, const void* object) noexcept
{
    RC::Unreal::UObject* held = ObjectInSlot(index);
    return held != nullptr && held == object && RC::Unreal::UObjectArray::IsValid(held->GetObjectItem(), false);
}

/// A slot record's serial number (FUObjectItem: Object, Flags, ClusterRootIndex, SerialNumber; stock 4.26, both
/// builds), as the engine's weak pointers keep it.
inline std::int32_t SlotSerial(const RC::Unreal::FUObjectItem* item) noexcept
{
    std::int32_t serial = 0;
    std::memcpy(&serial, reinterpret_cast<const std::uint8_t*>(item) + 0x10, sizeof(serial));
    return serial;
}

}  // namespace lss::players
