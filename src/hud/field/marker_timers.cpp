#include "hud/field/marker_timers.h"

#include "facts/hud.h"
#include "hud/hud_rules.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/sparse_array.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace lss::hud::marker_timers {

namespace {

// Facts read from the database (RE-p2-enemy-markers.md §7.5; this build). The list is a TSet like
// the enemy-status manager's: its element TSparseArray at +0x10 (platform/sparse_array.h); each
// element 0x30 bytes: +0 the weak widget, +0x14 T.
constexpr std::uint32_t  kMarkerList  = 0x150;
constexpr std::uint32_t  kElements    = 0x10;
constexpr std::uint32_t  kElement     = 0x30;
constexpr std::uint32_t  kTimer       = 0x14;

// Game thread.
std::uintptr_t       g_endUISystem = 0;
hud_rules::TimerSwap g_timers;  // player 1's set aside during player 2's run; player 2's own between frames

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
    const auto system = image.Rebase(facts::kEndUISystem, kMarkerList + sizeof(void*));
    g_endUISystem     = system ? *system : 0;
    return g_endUISystem != 0;
}

void SwapIn() noexcept
{
    try
    {
        g_timers.BeginIn();
        ForEachElement([](std::uintptr_t element, std::uint64_t key) {
            float mine = 0.0f;
            Read(element + kTimer, mine);
            Write(element, g_timers.In(key, mine));
        });
    }
    catch (...)  // only the vectors allocate: player 1's timers are then left in place
    {
        g_timers.Abandon();
    }
}

void SwapOut() noexcept
{
    try
    {
        g_timers.BeginOut();
        int pinned = 0;
        ForEachElement([&pinned](std::uintptr_t element, std::uint64_t key) {
            float theirs = 0.0f;
            Read(element + kTimer, theirs);
            if (const auto mine = g_timers.Out(key, theirs); mine)
            {
                Write(element, *mine);  // an element new in player 2's run keeps what it has
                pinned += theirs != *mine;
            }
        });
        g_timers.EndOut();  // player 2's own; arrows no longer in the list are dropped
        if (pinned > 0 && platform::Enabled(platform::Level::Trace))  // every HUD pass: Trace (platform/log.h)
        {
            platform::LogTrace(L"marker timers: {} of {} arrows have their own timer in player 2's view", pinned,
                               g_timers.TwoCount());
        }
    }
    catch (...)
    {
    }
    g_timers.Abandon();  // player 1's are back in place, or were never set aside
}

void Forget() noexcept
{
    g_timers.Forget();
}

}  // namespace lss::hud::marker_timers
