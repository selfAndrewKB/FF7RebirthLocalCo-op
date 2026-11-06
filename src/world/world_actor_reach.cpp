#include "world/world_actor_reach.h"

#include "facts/controller.h"
#include "facts/world.h"
#include "platform/every.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "players/unreal_access.h"
#include "world/charapop_centre.h"
#include "world/charapop_facts.h"
#include "world/field_pickup.h"
#include "world/world_rules.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <unordered_map>

namespace lss::world::world_actor_reach {

namespace {

using Clock = std::chrono::steady_clock;

// RE-p2-world-streaming.md §9.12.3 (this build). mgr = *(root+0x190).
constexpr std::uint32_t  kRootWorldActors = 0x190;
constexpr std::uint32_t  kItemsData       = 0x20;   // mgr: the item TSet's sparse array: data
constexpr std::uint32_t  kItemsNum        = 0x28;   // mgr: i32 ArrayNum
constexpr std::uint32_t  kItemsBits       = 0x30;   // mgr: allocation bits, inline (4 dwords); heap pointer at +0x10
constexpr std::uint32_t  kOctrees         = 0x70;   // mgr: field FName -> octree (TMap): data
constexpr std::uint32_t  kOctreesNum      = 0x78;   // mgr: i32 ArrayNum
constexpr std::uint32_t  kOctreesBits     = 0x80;   // mgr: allocation bits, inline; heap pointer at +0x10
constexpr std::uint32_t  kBitsHeap        = 0x10;   // the bit array's heap pointer, after its 16 inline bytes
constexpr std::int32_t   kBitsInline      = 128;
constexpr std::size_t    kItemElement     = 0x120;  // element: key FName +0, the item object at +0x10
constexpr std::size_t    kItemAt          = 0x10;
constexpr std::size_t    kOctreeElement   = 0x20;   // element: field FName +0, the octree object pointer +8
constexpr std::uint32_t  kOctreeObject    = 0x08;
constexpr std::uint32_t  kItemOctreeId    = 0x08;   // i32, -1: not in an octree
constexpr std::uint32_t  kItemField       = 0x10;   // FName: the field whose octree holds it
constexpr std::uint32_t  kItemRadius      = 0x18;   // float R: the pop radius (the tick's add and remove test)
constexpr std::uint32_t  kItemState       = 0x1C;   // i32: 6 armed, 0 requested, 1 spawned, 4, 2, 3 disabled, 5 pending
constexpr std::uint32_t  kItemExtent      = 0x20;   // float E: the octree half-extent (the add's box query)
constexpr std::uint32_t  kItemKey         = 0x30;   // FName: the CharaPop key
constexpr std::uint32_t  kItemLocation    = 0x50;   // FVector
constexpr std::uintptr_t kItemVtable      = 0x1457A6C90;  // the world-actor item's vtable (only these are written)
constexpr std::uintptr_t kOctreeVtable    = 0x1457F0988;  // WorldActorOctree's vtable: +0x28 AddItem, +0x30 RemoveItem
constexpr std::uint32_t  kOctreeAdd       = 0x28;
constexpr std::uint32_t  kOctreeRemove    = 0x30;
constexpr std::int32_t   kStateDisabled   = 3;
constexpr std::int32_t   kStatePending    = 5;
constexpr std::int32_t   kMaxItems        = 65536;
constexpr std::int32_t   kMaxOctrees      = 64;
constexpr float          kStepCm          = 1000.0f;  // the widening moves in 10 m steps (each step reinserts in the octree)
constexpr float          kCentreSlackCm   = 500.0f;   // the leader's bounding-box centre is not its root
constexpr float          kLeaveSlackCm    = 500.0f;   // a seat leaves an item 5 m past its radius
constexpr std::uint32_t  kEvery           = 10;       // ticks between passes
constexpr float          kChestLogCm      = 15000.0f; // Debug: chests within 150 m of either player log their state
constexpr auto           kSummaryEvery    = std::chrono::seconds(60);

using OctreeFn = void (*)(void* octree, void* item);

// Set once at install.
std::uintptr_t g_rootGlobal   = 0;
std::uintptr_t g_octreeVtable = 0;
std::uintptr_t g_itemVtable   = 0;
bool           g_installed    = false;

// Game thread.
/// An item the mod widened, by its key: the game's values and what the mod last wrote.
struct Widened
{
    float radius        = 0.0f;
    float extent        = 0.0f;
    float writtenRadius = 0.0f;
    float writtenExtent = 0.0f;
};
std::unordered_map<std::uint64_t, Widened> g_items;
std::unordered_map<std::uint64_t, std::int32_t> g_chestStates;  // Debug: each chest's last logged state
bool              g_on           = false;
bool              g_vtableWarned = false;
std::uint32_t     g_ticks        = 0;
std::uint32_t     g_reinserts    = 0;  // since the last summary
platform::Every   g_summary{kSummaryEvery};

/// The playing seats' members' locations this pass, by seat (players/seats.h; index 0, player 1's, unused).
struct Seats
{
    float        at[players::kMaxPlayers][3] = {};
    std::uint8_t read = 0;  // bit s: seat s's member was read

    bool Read(int seat) const noexcept { return (read & (1u << seat)) != 0; }

    /// The nearest read seat's distance to `point` (a large value with none). A NaN distance passes through, as the
    /// one-seat distance did.
    float Nearest(const float (&point)[3]) const noexcept
    {
        float nearest = 1e30f;
        for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
        {
            const float to = Read(seat) ? charapop::Distance(point, at[seat]) : 1e30f;
            if (!(nearest <= to))
            {
                nearest = to;
            }
        }
        return nearest;
    }

    /// ", player 2 {d} m" for each seat in seat order (-1 m unread, or for every seat when `point` is null).
    std::wstring Text(const float (*point)[3]) const
    {
        std::wstring text;
        for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
        {
            const float to = point != nullptr && Read(seat) ? charapop::Distance(*point, at[seat]) : -100.0f;
            text += std::format(L", {} {:.1f} m", players::SeatName(seat), to / 100.0f);
        }
        return text;
    }
};

/// "around player 2" with one seat; the seats' wording otherwise.
constexpr const wchar_t* kAroundSeats = players::kSeatsBuilt == 1 ? L"player 2" : L"players 2 and 3";

const std::byte* Manager() noexcept
{
    const void* root = ReadPointer(reinterpret_cast<const void*>(g_rootGlobal), 0);
    return static_cast<const std::byte*>(ReadPointer(root, kRootWorldActors));
}

bool Allocated(const std::byte* bitArray, std::int32_t index) noexcept
{
    const auto* heap = static_cast<const std::byte*>(ReadPointer(bitArray, kBitsHeap));
    if (heap == nullptr && index >= kBitsInline)
    {
        return false;
    }
    const auto word = ReadField<std::uint32_t>(heap != nullptr ? heap : bitArray,
                                               static_cast<std::size_t>(index / 32) * sizeof(std::uint32_t));
    return (word >> (index % 32) & 1) != 0;
}

/// The octree holding the field's items (the game's own lookup is the map at mgr+0x70); checked by its vtable.
void* OctreeOf(const std::byte* manager, std::uint64_t field) noexcept
{
    const auto* data = static_cast<const std::byte*>(ReadPointer(manager, kOctrees));
    const auto  num  = std::clamp(ReadField<std::int32_t>(manager, kOctreesNum), 0, kMaxOctrees);
    for (std::int32_t i = 0; data != nullptr && i < num; ++i)
    {
        const std::byte* element = data + static_cast<std::size_t>(i) * kOctreeElement;
        if (!Allocated(manager + kOctreesBits, i) || ReadField<std::uint64_t>(element, 0) != field)
        {
            continue;
        }
        void* octree = const_cast<void*>(ReadPointer(element, kOctreeObject));
        if (octree != nullptr && ReadField<std::uintptr_t>(octree, 0) == g_octreeVtable)
        {
            return octree;
        }
        if (!g_vtableWarned)
        {
            g_vtableWarned = true;
            platform::Log(L"world actor reach: WARNING: a field's octree has another vtable; its items keep their box extent "
                          L"(the pop radius is still widened)");
        }
        return nullptr;
    }
    return nullptr;
}

bool WriteFloat(std::byte* at, float value) noexcept
{
    return platform::WriteGuarded(at, &value, sizeof(value));
}

/// Writes the item's radius and extent; the extent through the game's own octree remove and add. False if the item
/// could not take the extent (no octree): its radius is written anyway.
bool Set(const std::byte* manager, std::byte* item, float radius, float extent) noexcept
{
    if (ReadField<float>(item, kItemRadius) != radius)
    {
        WriteFloat(item + kItemRadius, radius);
    }
    if (ReadField<float>(item, kItemExtent) == extent)
    {
        return true;
    }
    void* octree = ReadField<std::int32_t>(item, kItemOctreeId) >= 0
                       ? OctreeOf(manager, ReadField<std::uint64_t>(item, kItemField))
                       : nullptr;
    if (octree == nullptr)
    {
        return false;
    }
    const auto* vtable = static_cast<const std::byte*>(ReadPointer(octree, 0));
    auto        remove = reinterpret_cast<OctreeFn>(ReadField<std::uintptr_t>(vtable, kOctreeRemove));
    auto        add    = reinterpret_cast<OctreeFn>(ReadField<std::uintptr_t>(vtable, kOctreeAdd));
    if (remove == nullptr || add == nullptr || !platform::CallGuarded(remove, octree, static_cast<void*>(item)))
    {
        return false;
    }
    const float before = ReadField<float>(item, kItemExtent);
    WriteFloat(item + kItemExtent, extent);
    bool added = platform::CallGuarded(add, octree, static_cast<void*>(item));
    ++g_reinserts;
    if (!added)
    {
        // Out of the octree the game's tick never sees the item: back in with its previous extent.
        WriteFloat(item + kItemExtent, before);
        const bool back = platform::CallGuarded(add, octree, static_cast<void*>(item));
        platform::Log(L"world actor reach: WARNING: the octree's add faulted for {}; {}",
                      players::unreal::NameText(ReadField<std::uint64_t>(item, kItemKey)),
                      back ? L"added back with its previous extent" : L"adding it back faulted too (it returns at the next field load)");
    }
    return added;
}

/// `at`: the item's location (null: the distances read -1 m).
void LogItem(std::uint64_t key, const wchar_t* what, float radius, float extent, float toOne, const Seats& seats,
             const float (*at)[3]) noexcept
{
    const bool chest = field_pickup::IsChestEntry(key);
    if (!chest && !platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    try
    {
        platform::LogAt(chest ? platform::Level::Info : platform::Level::Debug,
                        L"world actor reach: {}{}: {} (pop radius {:.0f} m, box half-extent {:.0f} m); player 1 {:.1f} "
                        L"m{}",
                        chest ? L"chest " : L"", players::unreal::NameText(key), what, radius / 100.0f, extent / 100.0f,
                        toOne / 100.0f, seats.Text(at));
    }
    catch (...)
    {
    }
}

const wchar_t* StateText(std::int32_t state) noexcept
{
    switch (state)
    {
    case 6:  return L"armed";
    case 0:  return L"spawn requested";
    case 1:  return L"spawned";
    case 4:  return L"despawned by CharaPop while requested";
    case 2:  return L"removal requested";
    case 3:  return L"disabled";
    case 5:  return L"waiting for the field octree";
    default: return L"another state";
    }
}

/// Debug: a chest item's state as the game's tick moves it (armed, requested, spawned, removal), within 150 m of any
/// player, when it changes.
void LogChestState(std::uint64_t key, std::int32_t state, float toOne, const Seats& seats, const float (&at)[3]) noexcept
{
    if ((toOne > kChestLogCm && seats.Nearest(at) > kChestLogCm) || !field_pickup::IsChestEntry(key))
    {
        return;
    }
    const auto known = g_chestStates.find(key);
    if (known != g_chestStates.end() && known->second == state)
    {
        return;
    }
    g_chestStates[key] = state;
    try
    {
        platform::LogDebug(L"world actor reach: chest {}: {} (state {}); player 1 {:.1f} m{}",
                           players::unreal::NameText(key), StateText(state), state, toOne / 100.0f, seats.Text(&at));
    }
    catch (...)
    {
    }
}

/// One pass over the live items: an item near any read seat is widened by `by` (one write per item). `by` < 0 puts
/// every widened item back. Returns the number widened now.
std::size_t Apply(const std::byte* manager, const float (&one)[3], const Seats& seats, float by) noexcept
{
    auto*       data    = static_cast<std::byte*>(const_cast<void*>(ReadPointer(manager, kItemsData)));
    const auto  num     = std::clamp(ReadField<std::int32_t>(manager, kItemsNum), 0, kMaxItems);
    std::size_t widened   = 0;
    const bool  logChests = by >= 0.0f && platform::Enabled(platform::Level::Debug);
    if (!logChests)
    {
        g_chestStates.clear();  // the next Debug pass logs every chest near afresh
    }
    for (std::int32_t i = 0; data != nullptr && i < num; ++i)
    {
        if (!Allocated(manager + kItemsBits, i))
        {
            continue;
        }
        const std::byte* element = data + static_cast<std::size_t>(i) * kItemElement;
        std::byte*       item    = data + static_cast<std::size_t>(i) * kItemElement + kItemAt;
        const auto       key     = ReadField<std::uint64_t>(element, 0);  // the set's own key: unique among live items
        const auto       state   = ReadField<std::int32_t>(item, kItemState);
        if (key == 0 || ReadField<std::uintptr_t>(item, 0) != g_itemVtable)
        {
            continue;
        }
        const float radius = ReadField<float>(item, kItemRadius);
        const float extent = ReadField<float>(item, kItemExtent);
        auto        known  = g_items.find(key);
        if (known != g_items.end() &&
            world_rules::Rewritten(known->second.writtenRadius, known->second.writtenExtent, radius, extent))
        {
            g_items.erase(known);  // the game rewrote it (registered again): its values are the game's
            known = g_items.end();
        }
        const bool  widenedNow = known != g_items.end();
        const float baseRadius = widenedNow ? known->second.radius : radius;
        const float baseExtent = widenedNow ? known->second.extent : extent;
        const auto  logKey     = ReadField<std::uint64_t>(item, kItemKey);  // the CharaPop key (chests are known by it)
        if (by < 0.0f)
        {
            if (widenedNow)  // putting back needs no location
            {
                Set(manager, item, baseRadius, baseExtent);
                g_items.erase(key);
                LogItem(logKey, L"the game's values back", baseRadius, baseExtent, -100.0f, seats, nullptr);
            }
            continue;
        }
        float at[3]{};
        if (!platform::CopyGuarded(at, item + kItemLocation, sizeof(at)))
        {
            continue;
        }
        const float toSeat = seats.Nearest(at);
        if (logChests)
        {
            LogChestState(logKey, state, charapop::Distance(at, one), seats, at);
        }
        const bool  close = world_rules::ItemClose(state, kStateDisabled, kStatePending, toSeat, baseRadius, widenedNow,
                                                   kLeaveSlackCm);
        if (close)
        {
            const float wantRadius = baseRadius + by;
            const float wantExtent = std::max(baseExtent, baseRadius) + by;
            const bool  boxed      = Set(manager, item, wantRadius, wantExtent);
            // What memory now holds (a faulted write or octree add leaves the earlier value): the next pass compares it.
            g_items[key] = Widened{baseRadius, baseExtent, ReadField<float>(item, kItemRadius), ReadField<float>(item, kItemExtent)};
            if (!widenedNow)
            {
                try
                {
                    LogItem(logKey,
                            std::format(L"widened around {}{}", kAroundSeats, boxed ? L"" : L" (radius only: no octree)")
                                .c_str(),
                            wantRadius, boxed ? wantExtent : extent, charapop::Distance(at, one), seats, &at);
                }
                catch (...)
                {
                }
            }
            ++widened;
        }
        else if (widenedNow)
        {
            Set(manager, item, baseRadius, baseExtent);
            g_items.erase(key);
            LogItem(logKey,
                    players::kSeatsBuilt == 1 ? L"player 2 left it: the game's values back"
                                              : L"players 2 and 3 left it: the game's values back",
                    baseRadius, baseExtent, charapop::Distance(at, one), seats, &at);
        }
    }
    return widened;
}

bool CharacterAt(const void* character, float (&out)[3]) noexcept
{
    return character != nullptr && charapop::ActorLocation(const_cast<void*>(character), out);
}

void Stop(const wchar_t* why) noexcept
{
    if (g_items.empty())
    {
        if (g_on)
        {
            g_on = false;
            platform::Log(L"world actor reach: back to the game's own (world actors pop around the leader only): {}", why);
        }
        return;
    }
    // Without the manager (a field load) the items cannot be walked: the bookkeeping stays and the next tick tries again.
    const auto* manager = Manager();
    if (manager == nullptr)
    {
        return;
    }
    float none[3]{};
    Apply(manager, none, Seats{}, -1.0f);
    // The walk put back every live item it widened (by its set key and the item vtable); a key it did not meet is no
    // longer in the set: it left with its field.
    g_items.clear();
    g_on = false;
    platform::Log(L"world actor reach: back to the game's own (world actors pop around the leader only): {}", why);
}

void Pass(const std::array<bool, players::kMaxPlayers>& playing) noexcept
{
    const auto* manager = Manager();
    float       one[3]{};
    const void* playerOne = ReadPointer(players::generation::PlayerOneController(), facts::kControllerPawn);
    if (manager == nullptr || !CharacterAt(playerOne, one))
    {
        return;
    }
    Seats seats;
    float apart = 0.0f;  // the widest distance between player 1 and a read seat
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (playing[seat] && CharacterAt(players::generation::MemberOf(seat), seats.at[seat]))
        {
            seats.read |= static_cast<std::uint8_t>(1u << seat);
            // The widest; a NaN distance passes through, as the one-seat distance did.
            const float to = charapop::Distance(one, seats.at[seat]);
            if (!(to <= apart))
            {
                apart = to;
            }
        }
    }
    if (seats.read == 0)
    {
        return;
    }
    const float by      = world_rules::WidenBy(apart, kCentreSlackCm, kStepCm);
    const auto  widened = Apply(manager, one, seats, by);
    if (!g_on)
    {
        g_on = true;
        platform::Log(L"world actor reach: world actors near {} pop in by the game's own test from the leader, their "
                      L"radius widened by the players' separation ({:.0f} m now, {} items)",
                      kAroundSeats, by / 100.0f, widened);
    }
    if (platform::Enabled(platform::Level::Debug) && g_summary.Due(Clock::now()))
    {
        platform::LogDebug(L"world actor reach: players {:.0f} m apart; {} items widened by {:.0f} m; {} octree reinsert(s) since "
                           L"the last line",
                           apart / 100.0f, widened, by / 100.0f, g_reinserts);
        g_reinserts = 0;
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto root   = image.Rebase(facts::kEndGameRoot, sizeof(void*));
    const auto vtable = image.Rebase(kOctreeVtable, sizeof(void*));
    const auto items  = image.Rebase(kItemVtable, sizeof(void*));
    // The octree vtable is where the research said only if the two slots the tick calls hold the game's own code.
    const auto add    = image.ReadPointer(kOctreeVtable + kOctreeAdd);
    const auto remove = image.ReadPointer(kOctreeVtable + kOctreeRemove);
    const bool slots  = add && remove && image.Contains(*add) && image.Contains(*remove);
    g_rootGlobal      = root ? *root : 0;
    g_octreeVtable    = vtable && slots ? *vtable : 0;
    g_itemVtable      = items ? *items : 0;
    g_installed       = g_rootGlobal != 0 && g_octreeVtable != 0 && g_itemVtable != 0;
    platform::Log(L"world actor reach: {}", g_installed ? L"installed (world actors such as chests pop around players 2 and 3 too, "
                                                          L"with the NPC centre's switch)"
                                                        : L"UNAVAILABLE (a check failed; world actors pop around player 1 only)");
    return g_installed;
}

void Tick(const std::array<bool, players::kMaxPlayers>& plays) noexcept
{
    if (!g_installed || !players::generation::OnGameThread())
    {
        return;
    }
    const bool                             enabled = charapop_centre::Enabled();
    std::array<bool, players::kMaxPlayers> playing{};  // by seat: plays its member, its generation active
    bool                                   anyPlays = false;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        playing[seat] = plays[seat] && players::generation::Active(seat);
        anyPlays      = anyPlays || playing[seat];
    }
    if (!anyPlays || !enabled)
    {
        Stop(!enabled ? L"switched off"
                      : (players::kSeatsBuilt == 1 ? L"player 2 stopped playing" : L"no other player plays"));
        return;
    }
    if (++g_ticks % kEvery == 0)
    {
        Pass(playing);
    }
}

void Release() noexcept
{
    if (!g_installed)
    {
        return;
    }
    if (players::generation::OnGameThread())
    {
        Stop(L"the mod is unloading");
    }
    else if (!g_items.empty())
    {
        platform::Log(L"world actor reach: unloading off the game thread; {} widened items keep their values until their "
                      L"field reloads",
                      g_items.size());
    }
}

}  // namespace lss::world::world_actor_reach
