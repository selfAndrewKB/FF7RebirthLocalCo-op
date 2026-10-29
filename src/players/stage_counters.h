#pragma once

// What each player-2 wrapper did, counted for the periodic status line.
//
// A wrapper runs every frame, so it never logs; it counts. `acted` is a call where it changed
// something for player 2, `passed` a call it left to the engine unchanged, and `refused` a call
// where it would have acted but a precondition did not hold (the reason is counted by the stage).

#include "platform/every.h"

#include <atomic>
#include <cstdint>

namespace lss::players {

struct StageCounters
{
    std::atomic<std::uint32_t> acted{0};
    std::atomic<std::uint32_t> passed{0};
    std::atomic<std::uint32_t> refused{0};

    void Act() noexcept { acted.fetch_add(1, std::memory_order_relaxed); }
    void Pass() noexcept { passed.fetch_add(1, std::memory_order_relaxed); }
    void Refuse() noexcept { refused.fetch_add(1, std::memory_order_relaxed); }

    /// The counts since the last call, which also resets them.
    struct Snapshot
    {
        std::uint32_t acted;
        std::uint32_t passed;
        std::uint32_t refused;
    };
    Snapshot Take() noexcept
    {
        return {platform::Take(acted), platform::Take(passed), platform::Take(refused)};
    }
};

}  // namespace lss::players
