#include "world/seat_switch_volumes.h"

#include "facts/engine.h"
#include "facts/world.h"
#include "platform/build_value.h"
#include "platform/every.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "players/actor_fields.h"
#include "players/describe.h"
#include "players/game_globals.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "players/unreal_access.h"
#include "scopes/leader_scope.h"
#include "world/charapop_facts.h"
#include "world/world_rules.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

namespace lss::world::seat_switch_volumes {

namespace {

// 1.004 addresses (1.005 through replaced-shop-volumes.json). research/RE-p2-shop-npcs-near-p2.md.
constexpr std::uintptr_t kVolumeVtable  = 0x1456F2240;  // AEndCharaPopSwitchVolume
constexpr std::uintptr_t kEnterOverride = 0x14189CA14;  // ..._OnTriggerEnter_SetInside
constexpr std::uintptr_t kLeaveOverride = 0x14189E758;  // ..._OnTriggerLeave_ClearInside
inline constexpr platform::PerBuild<std::uint32_t> kEnterSlot{0x670, 0x668};
inline constexpr platform::PerBuild<std::uint32_t> kLeaveSlot{0x678, 0x670};
inline constexpr platform::PerBuild<std::uint32_t> kInside{0x3D0, 0x3B0};  // u8, CharaPop's only reader of it
constexpr std::uint32_t kCounts = 0x270;  // u8: the parent counts entries per key (volumes with extra shapes)
// bool(volume, FName entrant by value); the result in al.
using OverrideFn = std::uint8_t (*)(void* volume, std::uint64_t entrant);

// The on/off counters the filter tests: [[game root]+0x170], two int32 counts per id, on while their sum is not
// zero (as views/whole_screen.cpp reads them).
constexpr std::uint32_t kStateManager    = 0x170;
constexpr std::uint32_t kOnOffCountA     = 0x1A1238C;
constexpr std::uint32_t kOnOffCountB     = 0x1A1263C;
constexpr std::int32_t  kDisableBox      = 18;   // DisableTriggerBox: the filter skips the enter
constexpr std::int32_t  kDisableBoxInOut = 110;  // DisableTriggerBox_InOut: the filter skips the enter and the leave

// CharaPop's switch-volume records: TArray at P+0x6248 (count +0x6250), 48 bytes each: the volume (weak) +0x00,
// its show list (TArray<FName>) +0x10, its hide list +0x20. P = [[game root]+0x180]+0x10.
constexpr std::uint32_t kRecords      = 0x6248;
constexpr std::uint32_t kRecordCount  = 0x6250;
constexpr std::uint32_t kRecordSize   = 48;
constexpr std::uint32_t kRecordTag    = 0x08;  // the volume's Tags[0], the data row key
constexpr std::uint32_t kRecordShow   = 0x10;
constexpr std::uint32_t kRecordHide   = 0x20;
constexpr std::int32_t  kMaxRecords   = 512;
constexpr std::size_t   kMaxHeld      = 16;

/// A volume one or more seats are inside: one record per volume, the seats in it as a mask (the volume's inside byte is
/// one, shared by every player).
struct Held
{
    void*         volume    = nullptr;
    std::uint64_t weak      = 0;
    std::uint64_t tag       = 0;      // the record's tag, for the log
    bool          counts    = false;  // the game counts entries (+0x270)
    std::uint8_t  seats     = 0;      // bit s: seat s is inside (players/seats.h)
    std::int32_t  enters[players::kMaxPlayers] = {};  // by seat: its enters called and not yet left (a counting share)
    bool          playerOne = false;  // a brush-only volume player 1 is inside too

    bool Has(int seat) const noexcept { return (seats & Bit(seat)) != 0; }
    static std::uint8_t Bit(int seat) noexcept { return world_rules::VolumeSeatBit(seat); }

    /// Forgets `seat`; the record goes with its last seat.
    void Drop(int seat) noexcept
    {
        seats &= static_cast<std::uint8_t>(~Bit(seat));
        enters[seat] = 0;
        if (seats == 0)
        {
            *this = {};
        }
    }
};

struct Record
{
    bool          found = false;
    std::uint64_t tag   = 0;
    std::int32_t  shows = 0;
    std::int32_t  hides = 0;
    std::uint64_t firstShow = 0;
};

struct Counts
{
    unsigned enters = 0, leaves = 0, keptForPlayerOne = 0, reentered = 0, refused = 0, skippedOnOff = 0;
};

bool                  g_ready     = false;
std::uintptr_t        g_vtable    = 0;
OverrideFn            g_enter     = nullptr;
OverrideFn            g_leave     = nullptr;
const std::uintptr_t* g_gameRoot  = nullptr;  // the pointer global holding the game root
std::array<Held, kMaxHeld> g_held{};
// By seat (index 0, player 1's, unused): the weak of the seat's trigger its held volumes belong to, and its Tags[0].
std::uint64_t         g_trigger[players::kMaxPlayers] = {};
std::uint64_t         g_entrant[players::kMaxPlayers] = {};
Counts                g_counts{};
platform::Every       g_summary{std::chrono::seconds(10)};

/// "player 2" for one seat's name, the seats inside a record for the log ("player 2 and player 3").
std::wstring SeatsText(std::uint8_t seats)
{
    std::wstring text;
    for (int seat = 1; seat < players::kMaxPlayers; ++seat)
    {
        if ((seats & Held::Bit(seat)) != 0)
        {
            text += std::format(L"{}{}", text.empty() ? L"" : L" and ", players::SeatName(seat));
        }
    }
    return text;
}

void* Root() noexcept
{
    std::uintptr_t root = 0;
    return g_gameRoot != nullptr && platform::CopyGuarded(&root, g_gameRoot, sizeof(root)) ? reinterpret_cast<void*>(root)
                                                                                           : nullptr;
}

bool OnOff(std::int32_t id) noexcept
{
    const void* states = ReadPointer(Root(), kStateManager);
    if (states == nullptr)
    {
        return false;
    }
    const auto offset = static_cast<std::uint32_t>(id) * sizeof(std::int32_t);
    return ReadField<std::int32_t>(states, kOnOffCountA + offset) + ReadField<std::int32_t>(states, kOnOffCountB + offset) != 0;
}

std::uint64_t TagOf(const void* actor) noexcept
{
    if (ReadField<std::int32_t>(actor, facts::kActorTagsCount) <= 0)
    {
        return 0;  // NAME_None, as the filter passes for an untagged owner
    }
    const void* tags = players::actor_fields::Tags(actor);
    return tags != nullptr ? ReadField<std::uint64_t>(tags, 0) : 0;
}

/// CharaPop's record of `volume` (by its weak value).
Record RecordOf(const void* volume, std::uint64_t weak) noexcept
{
    Record      record;
    const void* manager = ReadPointer(Root(), charapop::kRootCharaPop);
    const void* pop     = ReadPointer(manager, charapop::kManagerPop);
    const auto* data    = static_cast<const std::byte*>(ReadPointer(pop, kRecords));
    const auto  count   = std::min(ReadField<std::int32_t>(pop, kRecordCount), kMaxRecords);
    if (volume == nullptr || weak == 0 || data == nullptr)
    {
        return record;
    }
    for (std::int32_t i = 0; i < count; ++i)
    {
        const std::byte* row = data + static_cast<std::size_t>(i) * kRecordSize;
        if (ReadField<std::uint64_t>(row, 0) != weak)
        {
            continue;
        }
        record.found  = true;
        record.tag    = ReadField<std::uint64_t>(row, kRecordTag);
        record.shows  = ReadField<std::int32_t>(row, kRecordShow + 8);
        record.hides  = ReadField<std::int32_t>(row, kRecordHide + 8);
        const void* shows = ReadPointer(row, kRecordShow);
        record.firstShow  = record.shows > 0 && shows != nullptr ? ReadField<std::uint64_t>(shows, 0) : 0;
        break;
    }
    return record;
}

std::wstring Text(std::uint64_t name)
{
    return name != 0 ? players::unreal::NameText(name) : std::wstring(L"(no tag)");
}

/// Calls the volume's own override with `entrant` (a trigger's tag); false if it faulted. `result` gets the parent's
/// answer.
bool Call(bool enter, void* volume, std::uint64_t entrant, std::uint8_t& result) noexcept
{
    result = 0;
    return platform::CallGuarded(enter ? g_enter : g_leave, result, volume, entrant);
}

std::uint8_t Inside(const void* volume) noexcept
{
    return ReadField<std::uint8_t>(volume, kInside);
}

Held* Find(const void* volume) noexcept
{
    for (Held& held : g_held)
    {
        if (held.volume != nullptr && held.volume == volume && scopes::leader_scope::StillNames(held.weak, volume))
        {
            return &held;
        }
    }
    return nullptr;
}

/// `seat` is inside a volume.
bool SeatInside(int seat) noexcept
{
    return std::any_of(g_held.begin(), g_held.end(), [seat](const Held& held) { return held.Has(seat); });
}

/// Leaves every volume `seat` is inside, by the same rules as the seat's own leave (without the on/off test: the
/// player is gone, so nothing may stay inside for them).
void LeaveAll(int seat, const wchar_t* why) noexcept
{
    for (Held& held : g_held)
    {
        if (!held.Has(seat))
        {
            continue;
        }
        if (scopes::leader_scope::StillNames(held.weak, held.volume))
        {
            const std::uint8_t before = Inside(held.volume);
            std::int32_t       calls  = world_rules::LeaveAllCalls(held.counts, held.enters[seat], held.seats, seat,
                                                                   held.playerOne);
            std::uint8_t       result = 0;
            for (; calls > 0; --calls)
            {
                Call(false, held.volume, g_entrant[seat], result);
            }
            try
            {
                platform::LogDebug(L"switch volume: {} left {} ({}): inside {} -> {}", players::SeatName(seat),
                                   Text(held.tag), why, before, Inside(held.volume));
            }
            catch (...)
            {
            }
            held.Drop(seat);
        }
        else
        {
            held = {};  // destroyed with its level: nothing to leave, for any seat
        }
    }
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const auto vtable = image.Rebase(kVolumeVtable, std::max<std::uint32_t>(kEnterSlot, kLeaveSlot) + 8);
    const auto enter  = image.Rebase(kEnterOverride);
    const auto leave  = image.Rebase(kLeaveOverride);
    const auto root   = players::game_globals::Address(players::game_globals::Global::GameRoot);
    const bool slots  = vtable && enter && leave &&
                       ReadField<std::uintptr_t>(reinterpret_cast<const void*>(*vtable), kEnterSlot) == *enter &&
                       ReadField<std::uintptr_t>(reinterpret_cast<const void*>(*vtable), kLeaveSlot) == *leave;
    if (!slots || !root)
    {
        platform::Log(L"switch volume: UNAVAILABLE (the class's enter or leave slot differs); shopkeepers on a switch "
                      L"volume appear only where player 1 is, as before");
        return;
    }
    g_vtable   = *vtable;
    g_enter    = reinterpret_cast<OverrideFn>(*enter);
    g_leave    = reinterpret_cast<OverrideFn>(*leave);
    g_gameRoot = reinterpret_cast<const std::uintptr_t*>(root);
    g_ready    = true;
    platform::Log(L"switch volume: installed ({} inside a CharaPop switch volume with a show list counts as inside, "
                  L"the union of {}; volumes with a hide list stay player 1's)",
                  players::kSeatsBuilt == 1 ? L"player 2" : L"player 2 or 3",
                  players::kSeatsBuilt == 1 ? L"both players" : L"every player");
}

bool Is(const void* actor) noexcept
{
    return g_ready && actor != nullptr && ReadField<std::uintptr_t>(actor, 0) == g_vtable;
}

void SeatEvent(int seat, bool enter, void* volume, const void* trigger) noexcept
{
    if (!g_ready || volume == nullptr || trigger == nullptr || !players::SeatBuilt(seat) || !players::generation::OnGameThread())
    {
        return;
    }
    try
    {
        const wchar_t* const who         = players::SeatName(seat);
        const auto           triggerWeak = scopes::leader_scope::WeakOf(const_cast<void*>(trigger));
        if (triggerWeak != g_trigger[seat])
        {
            LeaveAll(seat, std::format(L"{}'s trigger changed", who).c_str());
            g_trigger[seat] = triggerWeak;
        }
        g_entrant[seat]     = TagOf(trigger);
        const auto   weak   = scopes::leader_scope::WeakOf(volume);
        const Record record = RecordOf(volume, weak);
        if (!record.found || record.hides > 0 || record.shows <= 0)
        {
            ++g_counts.refused;
            platform::LogDebug(L"switch volume: {} {} {} ({}): held back, {}", who, enter ? L"enters" : L"leaves",
                               Text(record.tag), players::NameOf(volume),
                               !record.found     ? L"CharaPop has no record of it"
                               : record.hides > 0 ? std::format(L"its hide list ({} entries) would despawn them near "
                                                                L"player 1 too",
                                                                record.hides)
                                                  : L"it has no show list");
            return;
        }
        const bool         counts = ReadField<std::uint8_t>(volume, kCounts) != 0;
        const std::uint8_t before = Inside(volume);
        Held*              held   = Find(volume);  // any seat's record of this volume
        std::uint8_t       result = 0;
        std::wstring       what;
        if (enter)
        {
            const auto verdict = world_rules::SeatEnterVerdict(OnOff(kDisableBox), OnOff(kDisableBoxInOut), counts,
                                                               held != nullptr ? held->seats : std::uint8_t{0});
            if (verdict == world_rules::Enter::SkippedOnOff)
            {
                ++g_counts.skippedOnOff;
                what = L"not called: the game's DisableTriggerBox is on (as for player 1)";
            }
            else if (verdict == world_rules::Enter::AlreadyInside)
            {
                // A brush-only volume a seat is inside already: this seat is noted inside too, nothing is called.
                held->seats |= Held::Bit(seat);
                held->enters[seat] = std::max(held->enters[seat], 1);
                what = L"already inside";
            }
            else
            {
                if (held == nullptr)
                {
                    auto slot = std::find_if(g_held.begin(), g_held.end(), [](const Held& h) { return h.volume == nullptr; });
                    if (slot == g_held.end())
                    {
                        ++g_counts.refused;
                        platform::LogDebug(L"switch volume: {} enters {}: held back, already inside {} volumes", who,
                                           Text(record.tag), kMaxHeld);
                        return;
                    }
                    // No seat inside yet: the byte set means player 1 is inside (only player 1's trigger sets it then).
                    *slot           = {};
                    slot->volume    = volume;
                    slot->weak      = weak;
                    slot->tag       = record.tag;
                    slot->counts    = counts;
                    slot->playerOne = !counts && before != 0;
                    held            = &*slot;
                }
                Call(true, volume, g_entrant[seat], result);
                held->seats |= Held::Bit(seat);
                ++held->enters[seat];
                ++g_counts.enters;
                what = std::format(L"the volume's enter called (returned {}){}", result,
                                   held->playerOne ? L"; player 1 is inside too" : L"");
            }
        }
        else
        {
            const auto verdict = world_rules::SeatLeaveVerdict(held != nullptr ? held->seats : std::uint8_t{0}, seat,
                                                               OnOff(kDisableBoxInOut), held != nullptr && held->counts,
                                                               held != nullptr && held->playerOne);
            if (verdict == world_rules::Leave::NotEntered)
            {
                what = std::format(L"not called: {} was not entered here", who);
            }
            else if (verdict == world_rules::Leave::SkippedOnOff)
            {
                ++g_counts.skippedOnOff;
                held->Drop(seat);
                what = L"not called: the game's DisableTriggerBox_InOut is on (as for player 1)";
            }
            else if (verdict == world_rules::Leave::KeptForPlayerOne)
            {
                ++g_counts.keptForPlayerOne;
                what = held->playerOne
                           ? std::wstring(L"not called: player 1 is inside (a brush-only volume counts no one)")
                           : std::format(L"not called: {} is inside (a brush-only volume counts no one)",
                                         SeatsText(held->seats & static_cast<std::uint8_t>(~Held::Bit(seat))));
                held->Drop(seat);
            }
            else
            {
                Call(false, volume, g_entrant[seat], result);
                ++g_counts.leaves;
                if (--held->enters[seat] <= 0)
                {
                    held->Drop(seat);
                }
                what = std::format(L"the volume's leave called (returned {})", result);
            }
        }
        platform::LogDebug(L"switch volume: {} {} {} ({} {}, shows {} first {}): inside {} -> {}; {}", who,
                           enter ? L"enters" : L"leaves", Text(record.tag), counts ? L"counting" : L"brush-only",
                           players::NameOf(volume), record.shows, Text(record.firstShow), before, Inside(volume), what);
    }
    catch (...)
    {
    }
}

bool Watching() noexcept
{
    return std::any_of(g_held.begin(), g_held.end(), [](const Held& held) { return held.volume != nullptr; });
}

void AfterPlayerOne(bool enter, void* actor) noexcept
{
    Held* held = actor != nullptr && players::generation::OnGameThread() ? Find(actor) : nullptr;
    if (held == nullptr || held->counts)
    {
        return;  // a counting volume's union is the game's own count
    }
    try
    {
        const std::uint8_t before = Inside(actor);
        std::wstring       what;
        held->playerOne = enter;
        const auto change = world_rules::PlayerOneChangeOf(enter, before != 0, OnOff(kDisableBox), OnOff(kDisableBoxInOut));
        if (change == world_rules::PlayerOneChange::Entered)
        {
            what = L"player 1 is inside too";
        }
        else if (change == world_rules::PlayerOneChange::StillInside)
        {
            what = L"still inside";
        }
        else if (change == world_rules::PlayerOneChange::NotReentered)
        {
            what = L"not entered again: the game's DisableTriggerBox is on";
        }
        else
        {
            // Once for the seats inside, with the lowest seat's tag (every trigger's tag is PlayerTrigger).
            const int    first  = world_rules::ReenterSeat(held->seats);
            std::uint8_t result = 0;
            Call(true, actor, players::SeatIndex(first) ? g_entrant[first] : 0, result);
            ++g_counts.reentered;
            what = std::format(L"the volume's enter called again for {} (returned {})", SeatsText(held->seats), result);
        }
        platform::LogDebug(L"switch volume: player 1 {} {} while {} is inside: inside {} -> {}; {}",
                           enter ? L"enters" : L"leaves", Text(held->tag), SeatsText(held->seats), before, Inside(actor),
                           what);
    }
    catch (...)
    {
    }
}

void Tick() noexcept
{
    if (!g_ready || !players::generation::OnGameThread())
    {
        return;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (!players::generation::Active(seat))
        {
            if (SeatInside(seat))
            {
                try
                {
                    LeaveAll(seat, std::format(L"{} left the session", players::SeatName(seat)).c_str());
                }
                catch (...)
                {
                }
            }
            g_trigger[seat] = 0;
        }
    }
    for (Held& held : g_held)
    {
        if (held.volume != nullptr && !scopes::leader_scope::StillNames(held.weak, held.volume))
        {
            held = {};  // destroyed with its level: nothing to leave
        }
    }
    const auto clock = std::chrono::steady_clock::now();
    if (!g_summary.Due(clock))
    {
        return;
    }
    const Counts c = std::exchange(g_counts, {});
    if (c.enters + c.leaves + c.keptForPlayerOne + c.reentered + c.refused + c.skippedOnOff == 0 && !Watching())
    {
        return;
    }
    const auto inside = std::count_if(g_held.begin(), g_held.end(), [](const Held& held) { return held.volume != nullptr; });
    const wchar_t* const who = players::kSeatsBuilt == 1 ? L"player 2" : L"players 2 and 3";
    platform::LogDebug(L"switch volume (10 s): {}'s enters {}, leaves {}, leaves kept for player 1 {}, enters again "
                       L"after player 1's leave {}, held back (no show list or a hide list) {}, skipped by the game's "
                       L"on/off {}; {} inside {} now",
                       who, c.enters, c.leaves, c.keptForPlayerOne, c.reentered, c.refused, c.skippedOnOff, who, inside);
}

}  // namespace lss::world::seat_switch_volumes
