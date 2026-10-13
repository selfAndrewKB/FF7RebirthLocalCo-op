#include "player_two/leave_watch.h"

#include "platform/log.h"
#include "player_two/describe.h"
#include "player_two/object_fields.h"

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include <chrono>
#include <cstdint>
#include <format>
#include <iterator>
#include <string>

namespace lss::player_two::leave_watch {

namespace {

// RE-p2-controller-after-leave.md, "Offsets" (the same in both builds; stock UE 4.26 unless noted).
constexpr std::uint32_t kObjectFlags   = 0x08;  // UObject::ObjectFlags
constexpr std::uint32_t kObjectIndex   = 0x0C;  // UObject::InternalIndex
constexpr std::uint32_t kObjectClass   = 0x10;
constexpr std::uint32_t kObjectName    = 0x18;  // FName (8 bytes)
constexpr std::uint32_t kObjectOuter   = 0x20;
constexpr std::uint32_t kItemFlags     = 0x08;  // FUObjectItem: internal flags, ClusterRootIndex, SerialNumber
constexpr std::uint32_t kItemCluster   = 0x0C;
constexpr std::uint32_t kItemSerial    = 0x10;
constexpr std::uint32_t kLocalPlayerPc = 0x30;   // ULocalPlayer::PlayerController, as RemoveLocalPlayer reads it
constexpr std::uint32_t kActorBits     = 0x5C;   // AActor: 0x40 bActorIsBeingDestroyed
constexpr std::uint32_t kActorRole     = 0xF8;
constexpr std::uint32_t kPcInput       = 0x350;  // APlayerController::PlayerInput (Destroyed nulls it)
constexpr std::uint32_t kLevelActors   = 0x98;   // ULevel::Actors (TArray)
constexpr std::uint32_t kInputCache    = 0x120;  // UInputComponent::CachedKeyToActionInfo (TArray; both builds)
constexpr std::uint32_t kInputCacheEntry = 0x70;  // its entry; +0 the UPlayerInput*
constexpr std::int32_t  kListCap         = 64;    // cache entries read per component, at most
constexpr std::uint8_t  kBeingDestroyed = 0x40;
constexpr std::int32_t  kPendingKill    = 0x20000000;
constexpr std::int32_t  kUnreachable    = 0x10000000;
constexpr std::int32_t  kBeginDestroyed = 0x8000;   // EObjectFlags RF_BeginDestroyed
constexpr std::int32_t  kFinishDestroyed = 0x10000;  // RF_FinishDestroyed
struct FlagName
{
    std::int32_t   bit;
    const wchar_t* name;
};
constexpr FlagName kItemFlagNames[] = {{0x40000000, L"RootSet"},      {kPendingKill, L"PendingKill"},
                                       {kUnreachable, L"Unreachable"}, {0x08000000, L"AsyncLoading"},
                                       {0x04000000, L"Async"},         {0x02000000, L"Native"},
                                       {0x01000000, L"ClusterRoot"},   {0x00800000, L"ReachableInCluster"}};
constexpr std::size_t kListed     = 24;     // holders listed by the walk, at most
constexpr std::int32_t kActorsCap = 200000;  // a level's actor list longer than this is not read
constexpr int          kOuterDepth = 32;

using Clock = std::chrono::steady_clock;
constexpr auto kFastFor   = std::chrono::minutes(2);
constexpr auto kFastEvery = std::chrono::seconds(1);
constexpr auto kSlowEvery = std::chrono::seconds(10);
constexpr auto kWatchFor  = std::chrono::minutes(30);

/// One object followed after the leave: alive while its object slot still holds it. A new object can take the freed
/// slot at the same address (a rejoin's controller), so the class and FName must match too, and the serial when one
/// was set (UE4 sets it only once a weak pointer is made, and clears it when the slot is freed).
struct Watched
{
    const wchar_t* what   = L"";
    void*          object = nullptr;
    std::int32_t   index  = -1;
    std::int32_t   serial = 0;
    const void*    cls    = nullptr;
    std::uint64_t  fname  = 0;
    std::wstring   name;
    bool           gone   = true;  // nothing to follow, or freed
};

struct Leave
{
    bool              active = false;
    bool              walked = false;
    std::uint32_t     number = 0;
    Clock::time_point at{}, next{};
    Watched           controller, input;
    Watched           actors[4];
};
Leave g_leave;  // game thread

void* ItemOf(const Watched& watched) noexcept
{
    return watched.index >= 0 ? static_cast<void*>(RC::Unreal::UObjectArray::IndexToObject(watched.index)) : nullptr;
}

bool Alive(const Watched& watched) noexcept
{
    const void* item = ItemOf(watched);
    return item != nullptr && ReadPointer(item, 0) == watched.object &&
           (watched.serial == 0 || ReadField<std::int32_t>(item, kItemSerial) == watched.serial) &&
           ReadPointer(watched.object, kObjectClass) == watched.cls &&
           ReadField<std::uint64_t>(watched.object, kObjectName) == watched.fname;
}

Watched Watch(const wchar_t* what, void* object) noexcept
{
    Watched watched{what, object};
    if (object != nullptr)
    {
        watched.index      = ReadField<std::int32_t>(object, kObjectIndex);
        watched.cls        = ReadPointer(object, kObjectClass);
        watched.fname      = ReadField<std::uint64_t>(object, kObjectName);
        const void* item   = ItemOf(watched);
        watched.serial     = item != nullptr ? ReadField<std::int32_t>(item, kItemSerial) : 0;
        watched.name       = NameOf(object);
        watched.gone       = !Alive(watched);
    }
    return watched;
}

std::wstring FlagsText(std::int32_t flags)
{
    std::wstring text;
    for (const auto& flag : kItemFlagNames)
    {
        if ((flags & flag.bit) != 0)
        {
            text += (text.empty() ? L"" : L"|") + std::wstring(flag.name);
        }
    }
    return text.empty() ? L"none" : text;
}

/// The internal flags of an object's slot (0 if unread).
std::int32_t ItemFlags(const Watched& watched) noexcept
{
    const void* item = ItemOf(watched);
    return item != nullptr ? ReadField<std::int32_t>(item, kItemFlags) : 0;
}

/// Whether `actor` is in its level's actor list (its outer is the level).
const wchar_t* InLevel(const void* actor) noexcept
{
    const void* level  = ReadPointer(actor, kObjectOuter);
    const void* data   = ReadPointer(level, kLevelActors);
    const auto  num    = level != nullptr ? ReadField<std::int32_t>(level, kLevelActors + 8) : 0;
    if (data == nullptr || num <= 0 || num > kActorsCap)
    {
        return L"unread";
    }
    for (std::int32_t i = 0; i < num; ++i)
    {
        if (ReadPointer(data, static_cast<std::uint32_t>(i) * sizeof(void*)) == actor)
        {
            return L"yes";
        }
    }
    return L"no";
}

/// C: every object whose outer chain holds the controller, live ones first, and every input component whose key-map
/// cache still names the controller's PlayerInput (one walk of the object array).
void ListHolders() noexcept
{
    const void* controller = g_leave.controller.object;
    const void* input      = g_leave.input.object;
    std::wstring live, pending, caching;
    std::size_t  liveCount = 0, pendingCount = 0, rooted = 0, cachingCount = 0, components = 0;
    const auto   started   = Clock::now();
    try
    {
        auto* inputComponent = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UClass*>(
            nullptr, nullptr, STR("/Script/Engine.InputComponent"));
        RC::Unreal::UObjectGlobals::ForEachUObject([&](RC::Unreal::UObject* object, std::int32_t, std::int32_t) {
            if (object == nullptr || object == controller)
            {
                return RC::LoopAction::Continue;
            }
            // RE-p2-controller-after-leave.md S-PC2LEAK2: UInputComponent::CachedKeyToActionInfo at +0x120 (0x70-B
            // entries, +0 the UPlayerInput*), dropped only when the collector nulls the PlayerInput.
            if (input != nullptr && inputComponent != nullptr && object->IsA(inputComponent))
            {
                ++components;
                const void* cache = ReadPointer(object, kInputCache);
                const auto  count = ReadField<std::int32_t>(object, kInputCache + 8);
                for (std::int32_t i = 0; cache != nullptr && i < count && i < kListCap; ++i)
                {
                    if (ReadPointer(cache, static_cast<std::uint32_t>(i) * kInputCacheEntry) == input)
                    {
                        if (++cachingCount <= kListed)
                        {
                            caching += std::format(L"{}{} (outer {}; entry {} of {})", caching.empty() ? L"" : L", ",
                                                   NameOf(object), NameOf(ReadPointer(object, kObjectOuter)), i, count);
                        }
                        break;
                    }
                }
            }
            const void* outer = ReadPointer(object, kObjectOuter);
            for (int depth = 0; outer != nullptr && outer != controller && depth < kOuterDepth; ++depth)
            {
                outer = ReadPointer(outer, kObjectOuter);
            }
            if (outer != controller)
            {
                return RC::LoopAction::Continue;
            }
            // The object's own index: the walk's index argument may be within its chunk.
            const void*  item  = RC::Unreal::UObjectArray::IndexToObject(ReadField<std::int32_t>(object, kObjectIndex));
            const std::int32_t flags = item != nullptr ? ReadField<std::int32_t>(item, kItemFlags) : 0;
            const bool         dying = (flags & (kPendingKill | kUnreachable)) != 0;
            rooted += (flags & 0x40000000) != 0 ? 1 : 0;
            std::size_t&  count = dying ? pendingCount : liveCount;
            std::wstring& list  = dying ? pending : live;
            if (++count <= kListed)
            {
                list += std::format(L"{}{} ({}; outer {}; flags {})", list.empty() ? L"" : L", ", NameOf(object),
                                    NameOf(ReadPointer(object, kObjectClass)), NameOf(ReadPointer(object, kObjectOuter)),
                                    FlagsText(flags));
            }
            return RC::LoopAction::Continue;
        });
    }
    catch (...)
    {
        platform::Log(L"leave watch: the object walk raised an exception; no holder list");
        return;
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    platform::Log(L"leave watch: leave {}: objects outered to the old controller ({} ms walk): {} live{}{}; {} pending kill or "
                  L"unreachable{}{}; {} in the root set",
                  g_leave.number, ms, liveCount, live.empty() ? L"" : L": ", live, pendingCount,
                  pending.empty() ? L"" : L": ", pending, rooted);
    platform::Log(L"leave watch: leave {}: input components whose key-map cache still names its PlayerInput: {} of {}{}{}",
                  g_leave.number, cachingCount, components, caching.empty() ? L"" : L": ", caching);
    if (liveCount == 0)
    {
        platform::Log(L"leave watch: leave {}: no live object is outered to it; the holder is a root-set flag or a "
                      L"reference the outer walk cannot see (a property scan is the next step)",
                      g_leave.number);
    }
}

/// The controller is still in its slot while every leftover actor is gone: the collector ran and kept it.
void ReportHeld(Clock::duration since) noexcept
{
    const Watched& c       = g_leave.controller;
    const void*    item    = ItemOf(c);
    const auto     flags   = ItemFlags(c);
    const auto     objects = ReadField<std::int32_t>(c.object, kObjectFlags);
    platform::Log(L"leave watch: leave {}: HELD: {} s after the leave the leftover actors are freed and the old controller "
                  L"{} is not; slot flags {}, cluster root index {}, object flags {:#x} (BeginDestroyed {}, "
                  L"FinishDestroyed {})",
                  g_leave.number, std::chrono::duration_cast<std::chrono::seconds>(since).count(), c.name, FlagsText(flags),
                  item != nullptr ? ReadField<std::int32_t>(item, kItemCluster) : -1, objects,
                  YesNo((objects & kBeginDestroyed) != 0), YesNo((objects & kFinishDestroyed) != 0));
    ListHolders();
    g_leave.walked = true;
}

}  // namespace

void BeforeRemove(void* localPlayer, void* controller, const leftover_actors::Actors& actors) noexcept
{
    if (g_leave.active && !g_leave.controller.gone && Alive(g_leave.controller))
    {
        platform::Log(L"leave watch: leave {}'s controller {} is still not freed at the next leave (slot flags {})",
                      g_leave.number, g_leave.controller.name, FlagsText(ItemFlags(g_leave.controller)));
    }
    const std::uint32_t number = g_leave.number + 1;
    g_leave                    = Leave{};
    g_leave.number             = number;
    if (controller == nullptr)
    {
        return;
    }
    g_leave.controller = Watch(L"the controller", controller);
    g_leave.input      = Watch(L"its PlayerInput", ReadPointer(controller, kPcInput));
    void* const leftovers[] = {actors.trigger, actors.battleTalk, actors.cameraActor, actors.collisionActor};
    constexpr const wchar_t* kNames[] = {L"the trigger", L"the battle-talk actor", L"the camera actor", L"the collision actor"};
    for (std::size_t i = 0; i < std::size(leftovers); ++i)
    {
        g_leave.actors[i] = Watch(kNames[i], leftovers[i]);
    }
    const void* item = ItemOf(g_leave.controller);
    platform::Log(L"leave watch: leave {}: before RemoveLocalPlayer: the local player names {} (the controller {} is "
                  L"index {}, serial {}); role {}; slot flags {}; PlayerInput {}",
                  number, ReadPointer(localPlayer, kLocalPlayerPc) == controller ? L"the controller" : L"ANOTHER controller or none",
                  g_leave.controller.name, g_leave.controller.index,
                  item != nullptr ? ReadField<std::int32_t>(item, kItemSerial) : -1,
                  ReadField<std::uint8_t>(controller, kActorRole), FlagsText(ItemFlags(g_leave.controller)), g_leave.input.name);
}

void AfterRemove(bool removed) noexcept
{
    Watched& c = g_leave.controller;
    if (c.object == nullptr)
    {
        return;
    }
    if (!removed || !Alive(c))
    {
        platform::Log(L"leave watch: leave {}: after RemoveLocalPlayer: {}", g_leave.number,
                      removed ? L"the controller's slot no longer holds it (freed at once)" : L"player 2 was not removed");
        c.gone = true;
        return;
    }
    const auto flags = ItemFlags(c);
    platform::Log(L"leave watch: leave {}: after RemoveLocalPlayer: Destroy ran {}; PendingKill {}; still in its level's "
                  L"actor list {}; slot flags {}",
                  g_leave.number, YesNo((ReadField<std::uint8_t>(c.object, kActorBits) & kBeingDestroyed) != 0),
                  YesNo((flags & kPendingKill) != 0), InLevel(c.object), FlagsText(flags));
    g_leave.active = true;
    g_leave.at     = Clock::now();
    g_leave.next   = g_leave.at + kFastEvery;
}

void Tick() noexcept
{
    if (!g_leave.active)
    {
        return;
    }
    const auto now = Clock::now();
    if (now < g_leave.next)
    {
        return;
    }
    const auto since = now - g_leave.at;
    g_leave.next     = now + (since < kFastFor ? kFastEvery : kSlowEvery);
    const auto secs  = std::chrono::duration_cast<std::chrono::seconds>(since).count();
    bool       sentinelsGone = true, anySentinel = false;
    for (Watched* watched : {&g_leave.input, &g_leave.actors[0], &g_leave.actors[1], &g_leave.actors[2], &g_leave.actors[3]})
    {
        if (watched->object == nullptr)
        {
            continue;
        }
        if (watched != &g_leave.input)
        {
            anySentinel = true;
        }
        if (!watched->gone && !Alive(*watched))
        {
            watched->gone = true;
            platform::Log(L"leave watch: leave {}: {} ({}) freed, {} s after the leave", g_leave.number, watched->what,
                          watched->name, secs);
        }
        if (watched != &g_leave.input && !watched->gone)
        {
            sentinelsGone = false;
        }
    }
    if (!Alive(g_leave.controller))
    {
        platform::Log(L"leave watch: leave {}: the old controller {} freed, {} s after the leave", g_leave.number,
                      g_leave.controller.name, secs);
        g_leave.controller.gone = true;
        g_leave.active          = false;
        return;
    }
    if (anySentinel && sentinelsGone && !g_leave.walked)
    {
        ReportHeld(since);
    }
    if (since >= kWatchFor)
    {
        platform::Log(L"leave watch: leave {}: the old controller {} is still not freed after {} min; the watch ends "
                      L"(slot flags {})",
                      g_leave.number, g_leave.controller.name,
                      std::chrono::duration_cast<std::chrono::minutes>(kWatchFor).count(), FlagsText(ItemFlags(g_leave.controller)));
        g_leave.active = false;
    }
}

}  // namespace lss::player_two::leave_watch
