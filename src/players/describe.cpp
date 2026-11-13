#include "players/describe.h"

#include "facts/controller.h"
#include "facts/engine.h"
#include "platform/guarded.h"
#include "players/actor_fields.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/object_slot.h"

#include <Unreal/UObject.hpp>
#include <Unreal/UObjectArray.hpp>

#include <cstddef>
#include <cstdint>

namespace lss::players {

namespace {

/// Whether `object` is a live UObject: the GUObjectArray slot its index field names holds this very address,
/// reachable (an unreachable one, waiting for the collector's purge, counts as gone; a pending-kill one is still
/// named). A pointer read out of a game field may name an object freed since (the buggy body's controller field
/// right after a save reload named freed memory, and its name read faulted); the index is read under a
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
    if (const int seat = generation::SeatOfController(object); SeatIndex(seat))
    {
        return std::wstring(SeatName(seat)) + L"'s controller";
    }
    if (const int seat = generation::SeatOfMember(object); SeatIndex(seat))
    {
        return std::wstring(SeatName(seat)) + L"'s member";
    }
    if (object == players::actor_fields::Pawn(playerOne))
    {
        return L"player 1's character";
    }
    return NameOf(object);
}

}  // namespace lss::players
