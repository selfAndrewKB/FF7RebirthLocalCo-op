#include "hud/field/marker_timers.h"

#include "facts/hud.h"
#include "hud/hud_rules.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/sparse_array.h"
#include "players/game_globals.h"
#include "players/seats.h"
#include "views/columns.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <vector>

namespace lss::hud::marker_timers {

namespace {

// Facts read from the database (research/RE-p2-enemy-markers.md; this build). The list is a TSet like
// the enemy-status manager's: its element TSparseArray at +0x10 (platform/sparse_array.h); each
// element 0x30 bytes: +0 the weak widget, +0x14 T.
constexpr std::uint32_t  kMarkerList  = 0x150;
constexpr std::uint32_t  kElements    = 0x10;
constexpr std::uint32_t  kElement     = 0x30;
constexpr std::uint32_t  kTimer       = 0x14;

// Game thread.
std::uintptr_t       g_endUISystem = 0;
// By seat ([0] unused): player 1's set aside during the seat's run; the seat's own between frames.
std::array<hud_rules::TimerSwap, players::kMaxPlayers> g_timers;
// By column ([0] unused): a mirror's column's own, while a mirror pass runs there each frame.
std::array<hud_rules::TimerSwap, views::columns::kMaxColumns> g_mirrorTimers;
std::array<bool, views::columns::kMaxColumns>                 g_mirrorRan{};

/// The set for a run: the seat's, or (seat 0) the mirror's column's; null for player 1's own run.
hud_rules::TimerSwap* SetFor(int seat, int column) noexcept
{
    if (players::SeatIndex(seat))
    {
        return &g_timers[static_cast<std::size_t>(seat)];
    }
    if (seat == 0 && column >= 1 && column < views::columns::kMaxColumns)
    {
        return &g_mirrorTimers[static_cast<std::size_t>(column)];
    }
    return nullptr;
}

using platform::Read;  // the guarded reads: platform/guarded.h

/// Calls `visit(elementAddress, key)` for each allocated element of the list.
template <class Visit>
void ForEachElement(Visit&& visit)  // may throw what `visit` throws
{
    std::uintptr_t list = 0;
    if (g_endUISystem == 0 || !Read(g_endUISystem + kMarkerList, list) || list == 0)
    {
        return;
    }
    platform::ForEachSparseElement(list + kElements, kElement, [&visit](std::uintptr_t element) {
        std::uint64_t key = 0;
        if (Read(element, key))
        {
            visit(element, key);
        }
        return true;
    });
}

void Write(std::uintptr_t element, float t) noexcept
{
    platform::WriteGuarded(reinterpret_cast<void*>(element + kTimer), &t, sizeof(t));
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto system = players::game_globals::Address(players::game_globals::Global::UiSystem);
    g_endUISystem     = system;
    return g_endUISystem != 0;
}

void SwapIn(int seat, int column) noexcept
{
    hud_rules::TimerSwap* set = SetFor(seat, column);
    if (set == nullptr)
    {
        return;
    }
    if (seat == 0)
    {
        g_mirrorRan[static_cast<std::size_t>(column)] = true;  // its set is kept at this frame's EndFrame
    }
    hud_rules::TimerSwap& timers = *set;
    try
    {
        timers.BeginIn();
        ForEachElement([&timers](std::uintptr_t element, std::uint64_t key) {
            float mine = 0.0f;
            Read(element + kTimer, mine);
            Write(element, timers.In(key, mine));
        });
    }
    catch (...)  // only the vectors allocate: player 1's timers are then left in place
    {
        timers.Abandon();
    }
}

void SwapOut(int seat, int column) noexcept
{
    hud_rules::TimerSwap* set = SetFor(seat, column);
    if (set == nullptr)
    {
        return;
    }
    hud_rules::TimerSwap& timers = *set;
    try
    {
        timers.BeginOut();
        int pinned = 0;
        ForEachElement([&pinned, &timers](std::uintptr_t element, std::uint64_t key) {
            float theirs = 0.0f;
            Read(element + kTimer, theirs);
            if (const auto mine = timers.Out(key, theirs); mine)
            {
                Write(element, *mine);  // an element new in the column's run keeps what it has
                pinned += theirs != *mine;
            }
        });
        timers.EndOut();  // the set's own; arrows no longer in the list are dropped
        if (pinned > 0 && platform::Enabled(platform::Level::Trace))  // every HUD pass: Trace (platform/log.h)
        {
            platform::LogTrace(L"marker timers: {} of {} arrows have their own timer in {}", pinned,
                               timers.SeatTimerCount(),
                               seat != 0 ? std::format(L"{}'s view", players::SeatName(seat))
                                         : std::format(L"the mirror view in column {}", column + 1));
        }
    }
    catch (...)
    {
    }
    timers.Abandon();  // player 1's are back in place, or were never set aside
}

void Forget() noexcept
{
    for (hud_rules::TimerSwap& timers : g_timers)
    {
        timers.Forget();
    }
}

void EndFrame() noexcept
{
    for (std::size_t column = 1; column < g_mirrorRan.size(); ++column)
    {
        if (!g_mirrorRan[column])
        {
            g_mirrorTimers[column].Forget();  // no mirror pass in that column this frame: its arrows start again at 0
        }
        g_mirrorRan[column] = false;
    }
}

void Forget(int seat) noexcept
{
    if (players::SeatIndex(seat))
    {
        g_timers[static_cast<std::size_t>(seat)].Forget();
    }
}

}  // namespace lss::hud::marker_timers
