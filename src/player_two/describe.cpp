#include "player_two/describe.h"

#include "facts/controller.h"
#include "facts/engine.h"
#include "platform/guarded.h"
#include "players/object_slot.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"

#include <Unreal/UObject.hpp>
#include <Unreal/UObjectArray.hpp>

#include <cstddef>
#include <cstdint>

namespace lss::player_two {

namespace {

/// Whether `object` is a live UObject: the GUObjectArray slot its index field names holds this very address,
/// reachable (an unreachable one, waiting for the collector's purge, counts as gone; a pending-kill one is still
/// named). A pointer read out of a game field may name an object freed since (the buggy body's controller field
/// right after a save reload named freed memory, and its name read faulted, 2026-10-21); the index is read under a
/// guard since the memory may be unmapped by now.
bool Live(const void* object) noexcept
{
    std::int32_t index = -1;
    const auto*  field = static_cast<const std::byte*>(object) + facts::kObjectInternalIndex;
    if (!platform::CopyGuarded(&index, field, sizeof(index)))
    {
        return false;
    }
    return players::ObjectInSlot(index) == object;  // the slot's object, while it is reachable
}

}  // namespace

std::wstring NameOf(const void* object) noexcept
{
    if (object == nullptr)
    {
        return L"(none)";
    }
    if (!Live(object))
    {
        return L"(not a live object)";
    }
    try
    {
        return static_cast<const RC::Unreal::UObject*>(object)->GetName();
    }
    catch (...)
    {
        return L"(unnamed)";
    }
}

std::wstring Who(const void* object) noexcept
{
    if (object == nullptr)
    {
        return L"none";
    }
    const void* const playerOne = generation::PlayerOneController();
    if (object == playerOne)
    {
        return L"player 1's controller";
    }
    if (object == generation::PlayerTwoController())
    {
        return L"player 2's controller";
    }
    if (object == generation::Member())
    {
        return L"player 2's member";
    }
    if (object == ReadPointer(playerOne, facts::kControllerPawn))
    {
        return L"player 1's character";
    }
    return NameOf(object);
}

}  // namespace lss::player_two
