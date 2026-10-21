#include "player_two/switch_volume_two.h"

#include "player_two/world_rules.h"
#include "platform/build_value.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "player_two/charapop_facts.h"
#include "player_two/describe.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/object_fields.h"
#include "players/unreal_access.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

namespace lss::player_two::switch_volume_two {

namespace {

// 1.004 addresses (1.005 through replaced-shop-volumes.json). RE-p2-shop-npcs-near-p2.md §2.1, §10.1.
constexpr std::uintptr_t kVolumeVtable  = 0x145888590;  // AEndCharaPopSwitchVolume (1.005 0x1456F2240)
constexpr std::uintptr_t kEnterOverride = 0x1417FD038;  // ..._OnTriggerEnter_SetInside (1.005 0x14189CA14)
constexpr std::uintptr_t kLeaveOverride = 0x1409DD160;  // ..._OnTriggerLeave_ClearInside (1.005 0x14189E758)
inline constexpr platform::PerBuild<std::uint32_t> kEnterSlot{0x668, 0x670};
inline constexpr platform::PerBuild<std::uint32_t> kLeaveSlot{0x670, 0x678};
inline constexpr platform::PerBuild<std::uint32_t> kInside{0x3B0, 0x3D0};  // u8, CharaPop's only reader of it
constexpr std::uint32_t kCounts = 0x270;  // u8: the parent counts entries per key (volumes with extra shapes)
// bool(volume, FName entrant by value); the result in al.
using OverrideFn = std::uint8_t (*)(void* volume, std::uint64_t entrant);

// The on/off counters the filter tests (§10.2): [[game root]+0x170], two int32 counts per id, on while their sum is not
// zero (as views/whole_screen.cpp reads them).
constexpr std::uint32_t kStateManager    = 0x170;
constexpr std::uint32_t kOnOffCountA     = 0x1A1238C;
constexpr std::uint32_t kOnOffCountB     = 0x1A1263C;
constexpr std::int32_t  kDisableBox      = 18;   // DisableTriggerBox: the filter skips the enter
constexpr std::int32_t  kDisableBoxInOut = 110;  // DisableTriggerBox_InOut: the filter skips the enter and the leave

// CharaPop's switch-volume records (§2.3): TArray at P+0x6248 (count +0x6250), 48 bytes each: the volume (weak) +0x00,
// its show list (TArray<FName>) +0x10, its hide list +0x20. P = [[game root]+0x180]+0x10.
constexpr std::uint32_t kRecords      = 0x6248;
constexpr std::uint32_t kRecordCount  = 0x6250;
constexpr std::uint32_t kRecordSize   = 48;
constexpr std::uint32_t kRecordTag    = 0x08;  // the volume's Tags[0], the data row key
constexpr std::uint32_t kRecordShow   = 0x10;
constexpr std::uint32_t kRecordHide   = 0x20;
constexpr std::int32_t  kMaxRecords   = 512;
constexpr std::size_t   kMaxHeld      = 16;

struct Held
{
    void*         volume    = nullptr;
    std::uint64_t weak      = 0;
    std::uint64_t tag       = 0;      // the record's tag, for the log
    bool          counts    = false;  // the game counts entries (+0x270)
    std::int32_t  enters    = 0;      // player 2's enters called and not yet left (a counting volume's share)
    bool          playerOne = false;  // a brush-only volume player 1 is inside too
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
std::uint64_t         g_trigger   = 0;  // weak of player 2's trigger the held volumes belong to
std::uint64_t         g_entrant   = 0;  // that trigger's Tags[0]
Counts                g_counts{};
std::chrono::steady_clock::time_point g_summaryAt{};

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
    const void* tags = ReadPointer(actor, facts::kActorTagsData);
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

/// Calls the volume's own override; false if it faulted. `result` gets the parent's answer.
bool Call(bool enter, void* volume, std::uint8_t& result) noexcept
{
    result = 0;
    return platform::CallGuarded(enter ? g_enter : g_leave, result, volume, g_entrant);
}

std::uint8_t Inside(const void* volume) noexcept
{
    return ReadField<std::uint8_t>(volume, kInside);
}

Held* Find(const void* volume) noexcept
{
    for (Held& held : g_held)
    {
        if (held.volume != nullptr && held.volume == volume && leader_scope::StillNames(held.weak, volume))
        {
            return &held;
        }
    }
    return nullptr;
}

/// Leaves every volume player 2 is inside, by the same rules as player 2's own leave (without the on/off test: the
/// player is gone, so nothing may stay inside for them).
void LeaveAll(const wchar_t* why) noexcept
{
    for (Held& held : g_held)
    {
        if (held.volume == nullptr)
        {
            continue;
        }
        if (leader_scope::StillNames(held.weak, held.volume))
        {
            const std::uint8_t before = Inside(held.volume);
            std::int32_t       calls  = held.counts ? held.enters : (held.playerOne ? 0 : 1);
            std::uint8_t       result = 0;
            for (; calls > 0; --calls)
            {
                Call(false, held.volume, result);
            }
            try
            {
                platform::LogDebug(L"switch volume: player 2 left {} ({}): inside {} -> {}", Text(held.tag), why, before,
                                   Inside(held.volume));
            }
            catch (...)
            {
            }
        }
        held = {};
    }
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const auto vtable = image.Rebase(kVolumeVtable, std::max<std::uint32_t>(kEnterSlot, kLeaveSlot) + 8);
    const auto enter  = image.Rebase(kEnterOverride);
    const auto leave  = image.Rebase(kLeaveOverride);
    const auto root   = image.Rebase(charapop::kGameRoot, sizeof(std::uintptr_t));
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
    g_gameRoot = reinterpret_cast<const std::uintptr_t*>(*root);
    g_ready    = true;
    platform::Log(L"switch volume: installed (player 2 inside a CharaPop switch volume with a show list counts as inside, "
                  L"the union of both players; volumes with a hide list stay player 1's)");
}

bool Is(const void* actor) noexcept
{
    return g_ready && actor != nullptr && ReadField<std::uintptr_t>(actor, 0) == g_vtable;
}

void PlayerTwoEvent(bool enter, void* volume, const void* trigger) noexcept
{
    if (!g_ready || volume == nullptr || trigger == nullptr || !generation::OnGameThread())
    {
        return;
    }
    try
    {
        const auto triggerWeak = leader_scope::WeakOf(const_cast<void*>(trigger));
        if (triggerWeak != g_trigger)
        {
            LeaveAll(L"player 2's trigger changed");
            g_trigger = triggerWeak;
        }
        g_entrant = TagOf(trigger);
        const auto   weak   = leader_scope::WeakOf(volume);
        const Record record = RecordOf(volume, weak);
        if (!record.found || record.hides > 0 || record.shows <= 0)
        {
            ++g_counts.refused;
            platform::LogDebug(L"switch volume: player 2 {} {} ({}): held back, {}", enter ? L"enters" : L"leaves",
                               Text(record.tag), NameOf(volume),
                               !record.found     ? L"CharaPop has no record of it"
                               : record.hides > 0 ? std::format(L"its hide list ({} entries) would despawn them near "
                                                                L"player 1 too",
                                                                record.hides)
                                                  : L"it has no show list");
            return;
        }
        const bool         counts = ReadField<std::uint8_t>(volume, kCounts) != 0;
        const std::uint8_t before = Inside(volume);
        Held*              held   = Find(volume);
        std::uint8_t       result = 0;
        std::wstring       what;
        if (enter)
        {
            const auto verdict = world_rules::EnterVerdict(OnOff(kDisableBox), OnOff(kDisableBoxInOut), counts, held != nullptr);
            if (verdict == world_rules::Enter::SkippedOnOff)
            {
                ++g_counts.skippedOnOff;
                what = L"not called: the game's DisableTriggerBox is on (as for player 1)";
            }
            else if (verdict == world_rules::Enter::AlreadyInside)
            {
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
                        platform::LogDebug(L"switch volume: player 2 enters {}: held back, already inside {} volumes",
                                           Text(record.tag), kMaxHeld);
                        return;
                    }
                    *slot = {volume, weak, record.tag, counts, 0, !counts && before != 0};
                    held  = &*slot;
                }
                Call(true, volume, result);
                ++held->enters;
                ++g_counts.enters;
                what = std::format(L"the volume's enter called (returned {}){}", result,
                                   held->playerOne ? L"; player 1 is inside too" : L"");
            }
        }
        else if (const auto verdict = world_rules::LeaveVerdict(held != nullptr, OnOff(kDisableBoxInOut),
                                                                held != nullptr && held->counts,
                                                                held != nullptr && held->playerOne);
                 verdict == world_rules::Leave::NotEntered)
        {
            what = L"not called: player 2 was not entered here";
        }
        else if (verdict == world_rules::Leave::SkippedOnOff)
        {
            ++g_counts.skippedOnOff;
            *held = {};
            what  = L"not called: the game's DisableTriggerBox_InOut is on (as for player 1)";
        }
        else if (verdict == world_rules::Leave::KeptForPlayerOne)
        {
            ++g_counts.keptForPlayerOne;
            *held = {};
            what  = L"not called: player 1 is inside (a brush-only volume counts no one)";
        }
        else
        {
            Call(false, volume, result);
            ++g_counts.leaves;
            if (--held->enters <= 0)
            {
                *held = {};
            }
            what = std::format(L"the volume's leave called (returned {})", result);
        }
        platform::LogDebug(L"switch volume: player 2 {} {} ({} {}, shows {} first {}): inside {} -> {}; {}",
                           enter ? L"enters" : L"leaves", Text(record.tag), counts ? L"counting" : L"brush-only",
                           NameOf(volume), record.shows, Text(record.firstShow), before, Inside(volume), what);
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
    Held* held = actor != nullptr && generation::OnGameThread() ? Find(actor) : nullptr;
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
            std::uint8_t result = 0;
            Call(true, actor, result);
            ++g_counts.reentered;
            what = std::format(L"the volume's enter called again for player 2 (returned {})", result);
        }
        platform::LogDebug(L"switch volume: player 1 {} {} while player 2 is inside: inside {} -> {}; {}",
                           enter ? L"enters" : L"leaves", Text(held->tag), before, Inside(actor), what);
    }
    catch (...)
    {
    }
}

void Tick() noexcept
{
    if (!g_ready || !generation::OnGameThread())
    {
        return;
    }
    if (!generation::Active())
    {
        if (Watching())
        {
            LeaveAll(L"player 2 left the session");
        }
        g_trigger = 0;
    }
    for (Held& held : g_held)
    {
        if (held.volume != nullptr && !leader_scope::StillNames(held.weak, held.volume))
        {
            held = {};  // destroyed with its level: nothing to leave
        }
    }
    const auto clock = std::chrono::steady_clock::now();
    if (clock - g_summaryAt < std::chrono::seconds(10))
    {
        return;
    }
    g_summaryAt = clock;
    const Counts c = g_counts;
    g_counts       = {};
    if (c.enters + c.leaves + c.keptForPlayerOne + c.reentered + c.refused + c.skippedOnOff == 0 && !Watching())
    {
        return;
    }
    const auto inside = std::count_if(g_held.begin(), g_held.end(), [](const Held& held) { return held.volume != nullptr; });
    platform::LogDebug(L"switch volume (10 s): player 2's enters {}, leaves {}, leaves kept for player 1 {}, enters "
                       L"again after player 1's leave {}, held back (no show list or a hide list) {}, skipped by the "
                       L"game's on/off {}; player 2 inside {} now",
                       c.enters, c.leaves, c.keptForPlayerOne, c.reentered, c.refused, c.skippedOnOff, inside);
}

}  // namespace lss::player_two::switch_volume_two
