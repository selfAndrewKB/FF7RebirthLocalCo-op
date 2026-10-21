#pragma once

// Call sites placed from the game thread instead of at install: the ones whose five bytes cross a 16-byte
// block in game version 1.005 (platform/call_redirect.h, InstallFromOwnThread).
//
// Such a call may be changed only from the one thread that runs it, while that thread is not inside the
// function holding it. The battle and HUD call sites that need this run on the game thread only, under the
// game mode's tick (BattleSystem_Tick, MasterHUD_Tick, the command controller's tick, the party switch) or
// its input handlers (RE-1005-battle-2.md). The session's tick runs on that thread before the engine's own
// tick, outside all of them, so it runs the queued placements there, once each.
//
// A module queues its placement at install (before the engine-tick callback is live) and makes its own
// install line when the placement runs; most do so through platform/deferred_sites.h, which queues itself
// with a context. On the research build nothing is queued: its sites fit one block and are installed at
// startup, as they always were.

#include <array>
#include <atomic>
#include <cstddef>

namespace lss::platform::game_thread_sites {

/// A module's placement: installs its queued sites and logs the outcome. Runs once, on the game thread.
using PlaceFn = void (*)() noexcept;
/// The same with a context: an object that queued itself (platform/deferred_sites.h).
using PlaceWithFn = void (*)(void* context) noexcept;

namespace detail {
inline constexpr std::size_t kCapacity = 16;

struct Entry
{
    PlaceFn     place     = nullptr;
    PlaceWithFn placeWith = nullptr;  // used when `place` is null
    void*       context   = nullptr;
};

inline std::atomic_flag              g_busy;  // a spin lock: never throws (std::mutex can, in noexcept code), held for a copy
inline std::array<Entry, kCapacity> g_queue{};
inline std::size_t                  g_count = 0;

/// Holds the queue for one short copy. Contention is only a module's install racing the session's tick.
class Lock
{
public:
    Lock() noexcept
    {
        while (g_busy.test_and_set(std::memory_order_acquire))
        {
            g_busy.wait(true, std::memory_order_relaxed);
        }
    }
    ~Lock()
    {
        g_busy.clear(std::memory_order_release);
        g_busy.notify_one();
    }
    Lock(const Lock&)            = delete;
    Lock& operator=(const Lock&) = delete;
};

/// Appends `entry`. False (nothing queued) if the queue is full.
inline bool Push(const Entry& entry) noexcept
{
    const Lock lock;
    if (g_count == kCapacity)
    {
        return false;
    }
    g_queue[g_count++] = entry;
    return true;
}
}  // namespace detail

/// Queues `place` for the session's next tick. False (nothing queued) if it is null or the queue is full.
inline bool Queue(PlaceFn place) noexcept
{
    return place != nullptr && detail::Push({place, nullptr, nullptr});
}

/// Queues `place(context)` for the session's next tick. False (nothing queued) if `place` is null or the queue
/// is full.
inline bool Queue(PlaceWithFn place, void* context) noexcept
{
    return place != nullptr && detail::Push({nullptr, place, context});
}

/// Runs and empties the queue. Game thread, from the session's tick only.
inline void Run() noexcept
{
    std::array<detail::Entry, detail::kCapacity> queued{};
    std::size_t                                  count = 0;
    {
        const detail::Lock lock;
        if (detail::g_count == 0)
        {
            return;
        }
        queued          = detail::g_queue;
        count           = detail::g_count;
        detail::g_count = 0;
    }
    for (std::size_t i = 0; i < count; ++i)
    {
        if (queued[i].place != nullptr)
        {
            queued[i].place();
        }
        else
        {
            queued[i].placeWith(queued[i].context);
        }
    }
}

}  // namespace lss::platform::game_thread_sites
