#include "player_two/feedback/rumble_list_two.h"

#include "platform/every.h"
#include "platform/log.h"
#include "player_two/feedback/rumble_list_shared.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"
#include "players/seats.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

// The shared ride's copy (player_two/feedback/rumble_list_two.h): while player 1 drives the buggy or the bronco and the
// seats stand aside, every add and stop on the game's list is copied onto the list of each seat with a force-feedback
// manager of its own, in seat order; the loops already playing are seeded when a seat's copy turns on, and what was
// copied and not stopped is stopped on the seat's list when its copy ends. The flag and the copied keys are per seat.

namespace lss::player_two::rumble_list_two {

namespace {

// A list entry (0x30 bytes, both builds; RE-p2-ffb-manager-per-pc.md §11, S-RUMSEED) and the add's params (0x20).
constexpr std::uint32_t kEntrySize     = 0x30;
constexpr std::uint32_t kEntryFlags    = 0x00;  // u8: 0x02 loop, 0x04 finished, 0x08 stop requested, 0x10 fading out
constexpr std::uint32_t kEntryState    = 0x01;  // u8: 6 stop, 7 fade-out
constexpr std::uint32_t kEntrySlot     = 0x02;  // u8: the channel slot
constexpr std::uint32_t kEntryDuration = 0x10;  // f32
constexpr std::uint32_t kEntryEffect   = 0x18;  // FName
constexpr std::uint32_t kEntryHold     = 0x20;  // FName: the hold-point key
constexpr std::uint32_t kEntryKey      = 0x28;  // FName
constexpr std::int32_t  kMaxEntries    = 64;    // a sanity cap for the walk (a ride holds a handful)

struct AddParams
{
    std::uint64_t effect   = 0;
    std::uint64_t hold     = 0;
    std::uint8_t  slot     = 0;
    std::uint8_t  padSlot[3]{};     // named, so the struct handed to the game is zeroed whole
    float         duration = 0.0f;  // +0x14
    std::uint8_t  loop     = 0;     // +0x18
    std::uint8_t  padLoop[7]{};
};
static_assert(offsetof(AddParams, slot) == 0x10 && offsetof(AddParams, duration) == 0x14 &&
              offsetof(AddParams, loop) == 0x18 && sizeof(AddParams) == 0x20);

/// A list's entries (data, count capped), or none.
std::span<const std::uint8_t> Entries(const void* list) noexcept
{
    const auto* data  = static_cast<const std::uint8_t*>(ReadPointer(list, kListEntries));
    const auto  count = list != nullptr ? ReadField<std::int32_t>(list, kListEntries + kArrayNum) : 0;
    if (data == nullptr || count <= 0 || count > kMaxEntries)
    {
        return {};
    }
    return {data, static_cast<std::size_t>(count) * kEntrySize};
}

bool HasKey(const void* list, std::uint64_t key) noexcept
{
    const auto entries = Entries(list);
    for (std::size_t at = 0; at < entries.size(); at += kEntrySize)
    {
        if (ReadField<std::uint64_t>(entries.data() + at, kEntryKey) == key)
        {
            return true;
        }
    }
    return false;
}

/// `seat`'s copy turns on (game thread, under the game's lock, with the flag set inside it so no add falls between):
/// the loops already playing on the game's list are added to the seat's with the game's own add, params rebuilt from
/// the entry, flag 1 (the bronco loop's own). A loop added before the copy began (the seat joined into a ride) would
/// otherwise stay off its pad until the game re-adds it. Skipped: one-shots (they would replay from the start), entries
/// already stopping or ending (their stop has run; a copy would never stop), and keys the seat's list has. Returns how
/// many were seeded; their names go to `names`.
std::size_t SeedTwo(int seat, std::array<std::uint64_t, 16>& names) noexcept
{
    const ListLock lock;
    void* const    two   = g_listTwo[seat].load(std::memory_order_acquire);
    void* const    owner = ReadPointer(reinterpret_cast<const void*>(g_listOwner), 0);
    void* const    game  = owner != nullptr ? ReadPointer(owner, kOwnerList) : nullptr;
    g_copyOne[seat].store(two != nullptr, std::memory_order_release);
    std::size_t seeded = 0;
    if (two == nullptr || game == nullptr || game == two)
    {
        return seeded;
    }
    const auto entries = Entries(game);
    for (std::size_t at = 0; at < entries.size() && seeded < names.size(); at += kEntrySize)
    {
        const std::uint8_t* entry = entries.data() + at;
        const auto          flags = ReadField<std::uint8_t>(entry, kEntryFlags);
        const auto          state = ReadField<std::uint8_t>(entry, kEntryState);
        const auto          key   = ReadField<std::uint64_t>(entry, kEntryKey);
        if (!rumble_rules::SeedsLoop(flags, state) || HasKey(two, key))
        {
            continue;
        }
        AddParams params;
        params.effect   = ReadField<std::uint64_t>(entry, kEntryEffect);
        params.hold     = ReadField<std::uint64_t>(entry, kEntryHold);
        params.slot     = ReadField<std::uint8_t>(entry, kEntrySlot);
        params.duration = ReadField<float>(entry, kEntryDuration);
        params.loop     = 1;
        g_addOrUpdate(two, key, &params, 1);
        names[seeded++] = key;
        g_copiedKeys[seat].Insert(key);
    }
    return seeded;
}

/// The key leaves `seat`'s copied keys (its stop was copied).
void ForgetCopied(int seat, std::uint64_t key) noexcept
{
    const ListLock lock;
    g_copiedKeys[seat].Erase(key);
}

}  // namespace

namespace copy {

/// A shared ride's copy of an add to the game's list, into each copying seat's list in seat order (any thread, under
/// the game's lock). The key is kept until its stop is copied too, so the ride's end can stop what is left.
void CopyTwo(std::uint64_t key, const void* params, std::uint8_t flag) noexcept
{
    const ListLock lock;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        void* const list = g_listTwo[seat].load(std::memory_order_acquire);
        // Read again under the lock: the ride's end clears the flag before it takes the lock to stop what was copied,
        // so a copy that saw the flag set outside the lock cannot land after that cleanup (and play on with nothing to
        // stop it).
        if (list == nullptr || !g_copyOne[seat].load(std::memory_order_acquire))
        {
            continue;
        }
        g_addOrUpdate(list, key, params, flag);
        g_counts[seat].copiesTwo.fetch_add(1, std::memory_order_relaxed);
        if (g_copiedKeys[seat].Insert(key))
        {
            platform::LogDebug(L"rumble list: shared ride: {} copied into {}'s list", KeyText(key),
                               players::SeatName(seat));
        }
    }
}

/// A shared ride's copy of a stop on the game's list, on each copying seat's list in seat order (game thread: the
/// owner's list is named only there).
void CopyStopTwo(StopNamedFn original, std::uint64_t key, bool game) noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (!g_copyOne[seat].load(std::memory_order_acquire))
        {
            continue;
        }
        if (!game)
        {
            g_counts[seat].copyStopsLeft.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (StopTwo(seat, original, key))
        {
            g_counts[seat].copyStopsTwo.fetch_add(1, std::memory_order_relaxed);
            ForgetCopied(seat, key);
            platform::LogDebug(L"rumble list: shared ride: {} stopped on {}'s list too", KeyText(key),
                               players::SeatName(seat));
        }
    }
}

}  // namespace copy

void CopyPlayerOne(int seat, bool on) noexcept
{
    if (!g_installed || !generation::OnGameThread() || !players::SeatIndex(seat))
    {
        return;
    }
    const wchar_t* const name = players::SeatName(seat);
    on = on && g_listTwo[seat].load(std::memory_order_acquire) != nullptr;
    if (g_copyOne[seat].load(std::memory_order_relaxed) == on)
    {
        return;
    }
    if (on)
    {
        std::array<std::uint64_t, 16> seeded{};
        const std::size_t             count = SeedTwo(seat, seeded);
        try
        {
            std::wstring names;
            for (std::size_t i = 0; i < count; ++i)
            {
                names += (i == 0 ? L"" : L", ") + KeyText(seeded[i]);
            }
            platform::Log(L"rumble list: shared ride: player 1's rumble list is copied into {}'s (both pads); "
                          L"loops already playing, added to {}'s now: {}",
                          name, name, names.empty() ? L"none" : names);
        }
        catch (...)
        {
        }
        return;
    }
    g_copyOne[seat].store(false, std::memory_order_release);
    // The ride is over: what was copied and not stopped stops on the seat's list now, as the game stops it on player
    // 1's.
    std::array<std::uint64_t, 16> left{};
    std::size_t                   count = 0;
    {
        const ListLock lock;
        left  = g_copiedKeys[seat].Keys();
        count = g_copiedKeys[seat].Count();
        g_copiedKeys[seat].Clear();
    }
    const StopNamedFn original = StopNamedOriginal();
    for (std::size_t i = 0; i < count; ++i)
    {
        StopTwo(seat, original, left[i]);
    }
    try
    {
        std::wstring names;
        for (std::size_t i = 0; i < count; ++i)
        {
            names += (i == 0 ? L"" : L", ") + KeyText(left[i]);
        }
        platform::Log(L"rumble list: shared ride over: copies {}, their stops {} (on a worker, not copied {}); stopped "
                      L"now on {}'s list: {}",
                      platform::Take(g_counts[seat].copiesTwo), platform::Take(g_counts[seat].copyStopsTwo),
                      platform::Take(g_counts[seat].copyStopsLeft), name, names.empty() ? L"none" : names);
    }
    catch (...)
    {
    }
}

}  // namespace lss::player_two::rumble_list_two
