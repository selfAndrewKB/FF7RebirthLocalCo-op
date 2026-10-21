#pragma once

// A module's call sites placed at install, or from the game thread when the build needs it.
//
// In game version 1.005 some call sites' five bytes cross a 16-byte block (platform/call_redirect.h,
// InstallFromOwnThread): such a call may be changed only from the one thread that runs it, while that
// thread is not inside the function holding it. A module whose sites are such keeps what its Install
// found, queues its placement for the session's tick (platform/game_thread_sites.h) and makes its install
// line when the placement runs; on the research build the same placement runs at install. This is that
// protocol, once: the image kept for the placement (set before it is queued and never reset, so a Remove
// on another thread cannot pull it from under a running placement), the pending flag Remove clears so a
// placement still queued does nothing, and the choice of Install or InstallFromOwnThread per site.

#include "platform/call_redirect.h"
#include "platform/image.h"

#include <atomic>
#include <cstdint>
#include <optional>

namespace lss::platform {

class DeferredSites
{
public:
    /// A module's placement: its sites from what Install found, each through Place, then its install line. Its
    /// result is the install's result when it runs at install.
    using PlaceFn = bool (*)(const GameImage& image) noexcept;

    /// Runs `place` now and gives its result, or, when `deferred`, queues it for the game thread's next session
    /// tick: true when queued, false (nothing placed) when the queue had no room. Deferred on 1.005 by default.
    bool Install(const GameImage& image, PlaceFn place, bool deferred = Deferred()) noexcept;

    /// One call site: InstallFromOwnThread when the placement runs on the game thread, Install at install.
    bool Place(CallRedirect& call, std::uintptr_t site, std::uintptr_t target, std::uintptr_t replacement) const noexcept
    {
        return fromGameThread_ ? call.InstallFromOwnThread(site, target, replacement)
                               : call.Install(site, target, replacement);
    }

    /// Remove: a placement still queued does nothing. The sites placed are the module's to restore.
    void Cancel() noexcept
    {
        pending_ = false;
    }

    /// The placement runs (or ran) on the game thread: the module's install line says so, and a site that
    /// crosses a block is restored only from there.
    bool FromGameThread() const noexcept
    {
        return fromGameThread_;
    }

    /// Queued and not yet run.
    bool Pending() const noexcept
    {
        return pending_;
    }

    /// The running build's call sites cross 16-byte blocks: 1.005.
    static bool Deferred() noexcept;

private:
    static void RunQueued(void* self) noexcept;

    std::optional<GameImage> image_;
    PlaceFn                  place_          = nullptr;
    std::atomic<bool>        pending_{false};
    bool                     fromGameThread_ = false;
};

}  // namespace lss::platform
