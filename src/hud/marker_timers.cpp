#include "hud/marker_timers.h"

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
constexpr std::uintptr_t kEndUISystem = 0x1493496E0;
constexpr std::uint32_t  kMarkerList  = 0x150;
constexpr std::uint32_t  kElements    = 0x10;
constexpr std::uint32_t  kElement     = 0x30;
constexpr std::uint32_t  kTimer       = 0x14;
constexpr int            kMaxLines    = 40;

struct Timer
{
    std::uint64_t key = 0;  // the element's FWeakObjectPtr, as stored
    float         t   = 0.0f;
};

// Game thread.
std::uintptr_t     g_endUISystem = 0;
std::vector<Timer> g_playerOne;  // set aside during player 2's run
std::vector<Timer> g_playerTwo;  // player 2's own, between frames
std::vector<Timer> g_seen;       // scratch, reused
int                g_lines = 0;

template <class T>
bool Read(std::uintptr_t address, T& out) noexcept
{
    return platform::CopyGuarded(&out, reinterpret_cast<const void*>(address), sizeof(T));
}

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

const Timer* Find(const std::vector<Timer>& timers, std::uint64_t key) noexcept
{
    const auto found = std::find_if(timers.begin(), timers.end(), [key](const Timer& timer) { return timer.key == key; });
    return found != timers.end() ? &*found : nullptr;
}

void Write(std::uintptr_t element, float t) noexcept
{
    platform::WriteGuarded(reinterpret_cast<void*>(element + kTimer), &t, sizeof(t));
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto system = image.Rebase(kEndUISystem, kMarkerList + sizeof(void*));
    g_endUISystem     = system ? *system : 0;
    return g_endUISystem != 0;
}

void SwapIn() noexcept
{
    try
    {
        g_playerOne.clear();
        ForEachElement([](std::uintptr_t element, std::uint64_t key) {
            float mine = 0.0f;
            Read(element + kTimer, mine);
            g_playerOne.push_back({key, mine});
            const Timer* theirs = Find(g_playerTwo, key);
            Write(element, theirs != nullptr ? theirs->t : 0.0f);
        });
    }
    catch (...)  // only the vectors allocate: player 1's timers are then left in place
    {
        g_playerOne.clear();
    }
}

void SwapOut() noexcept
{
    try
    {
        g_seen.clear();
        int pinned = 0;
        ForEachElement([&pinned](std::uintptr_t element, std::uint64_t key) {
            float theirs = 0.0f;
            Read(element + kTimer, theirs);
            g_seen.push_back({key, theirs});
            if (const Timer* mine = Find(g_playerOne, key); mine != nullptr)
            {
                Write(element, mine->t);  // an element new in player 2's run keeps what it has
                pinned += theirs != mine->t;
            }
        });
        g_playerTwo.swap(g_seen);  // player 2's own; arrows no longer in the list are dropped
        if (pinned > 0 && g_lines < kMaxLines)
        {
            ++g_lines;
            platform::Log(L"marker timers: {} of {} arrows have their own timer in player 2's view", pinned,
                          g_playerTwo.size());
        }
    }
    catch (...)
    {
    }
    g_playerOne.clear();
}

void Forget() noexcept
{
    g_playerOne.clear();
    g_playerTwo.clear();
}

}  // namespace lss::hud::marker_timers
