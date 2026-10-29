#pragma once

// The coins enemies hold (battle/target_coin.h): a lock-free table keyed by the enemy's controller, each
// coin the player it names (1 for player 1, seat + 1 for a seat's player: 1 to 3, players/seats.h) and when it was
// tossed. A controller's coin is in one of four neighbouring slots; a new one takes its own slot, else an empty one,
// else the one tossed longest ago (a live enemy tosses at each decision, every few seconds; the oldest is a gone
// enemy's). Two threads placing coins in the same slot at once leave one of them: the other tosses again at its next
// question. Pure over its arrays with the clock passed in, so it is tested on the host (tests/coin_table_test.cpp).
// Any thread.

#include "players/seats.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace lss::battle::coin_table {

inline constexpr std::size_t    kBits   = 10;
inline constexpr std::size_t    kSlots  = std::size_t{1} << kBits;
inline constexpr std::size_t    kProbe  = 4;
inline constexpr std::uintptr_t kMask   = 3;  // the coin: the key with the player in its low bits (1 to 3), 0 empty
inline constexpr std::size_t    kNoSlot = kSlots;
static_assert(players::kMaxPlayers <= kMask, "every player's number fits in a key's low bits");

/// The number a coin holds for `seat`'s player (seat 0, player 1: 1).
constexpr int PlayerNumber(int seat) noexcept
{
    return seat + 1;
}

/// The seat a coin's player number names.
constexpr int SeatOfNumber(int number) noexcept
{
    return number - 1;
}

/// The `probe`th slot a key may sit in.
constexpr std::size_t SlotOf(std::uintptr_t key, std::size_t probe) noexcept
{
    const auto base = static_cast<std::size_t>(((key >> 4) * 0x9E3779B97F4A7C15ull) >> (64 - kBits));
    return (base + probe) & (kSlots - 1);
}

class CoinTable
{
public:
    /// The slot holding `key`'s coin, or kNoSlot.
    std::size_t Find(std::uintptr_t key) const noexcept
    {
        for (std::size_t probe = 0; probe < kProbe; ++probe)
        {
            const std::size_t slot = SlotOf(key, probe);
            if ((coins_[slot].load(std::memory_order_relaxed) & ~kMask) == key)
            {
                return slot;
            }
        }
        return kNoSlot;
    }

    /// The player number `key`'s coin names: 1 to 3; 0 with no coin.
    int PlayerOf(std::uintptr_t key) const noexcept
    {
        const std::size_t slot = Find(key);
        const auto        held = slot == kNoSlot ? 0 : coins_[slot].load(std::memory_order_relaxed);
        return (held & ~kMask) != key ? 0 : static_cast<int>(held & kMask);
    }

    /// `key` counts player number `player` (1 to 3) from `now` (seconds): in its own slot, else an empty one, else the
    /// one tossed longest ago among its four (never `keepSlot`, another key's coin that must stay).
    void Set(std::uintptr_t key, int player, std::uint32_t now, std::size_t keepSlot = kNoSlot) noexcept
    {
        const std::size_t slot = Place(key, now, keepSlot);
        tossedAt_[slot].store(now, std::memory_order_relaxed);
        coins_[slot].store(key | static_cast<std::uintptr_t>(player), std::memory_order_relaxed);
    }

    /// Forgets `key`'s coin, if it still holds one (a compare-and-swap: a coin placed there meanwhile stays).
    void Clear(std::uintptr_t key) noexcept
    {
        if (const std::size_t slot = Find(key); slot != kNoSlot)
        {
            std::uintptr_t held = coins_[slot].load(std::memory_order_relaxed);
            if ((held & ~kMask) == key)
            {
                coins_[slot].compare_exchange_strong(held, 0, std::memory_order_relaxed);
            }
        }
    }

    /// `key`'s coin becomes player 1 if it names a seat's player now (with one seat built: player 2). The player
    /// number it named when it changed (2 or 3), from the one read the change is made on; 0 if it did not change.
    int FallBack(std::uintptr_t key) noexcept
    {
        const std::size_t slot = Find(key);
        if (slot == kNoSlot)
        {
            return 0;
        }
        std::uintptr_t held = coins_[slot].load(std::memory_order_relaxed);
        const int      from = static_cast<int>(held & kMask);
        return (held & ~kMask) == key && from > 1 &&
                       coins_[slot].compare_exchange_strong(held, key | 1u, std::memory_order_relaxed)
                   ? from
                   : 0;
    }

private:
    /// Where a new coin for `key` goes: its own slot, else an empty one, else the one tossed longest ago (never `keep`).
    std::size_t Place(std::uintptr_t key, std::uint32_t now, std::size_t keep) const noexcept
    {
        if (const std::size_t own = Find(key); own != kNoSlot)
        {
            return own;
        }
        std::size_t   oldest = kNoSlot;
        std::uint32_t age    = 0;
        for (std::size_t probe = 0; probe < kProbe; ++probe)
        {
            const std::size_t slot = SlotOf(key, probe);
            if (slot == keep)
            {
                continue;
            }
            if (coins_[slot].load(std::memory_order_relaxed) == 0)
            {
                return slot;
            }
            const std::uint32_t slotAge = now - tossedAt_[slot].load(std::memory_order_relaxed);
            if (oldest == kNoSlot || slotAge >= age)
            {
                age    = slotAge;
                oldest = slot;
            }
        }
        return oldest;
    }

    std::atomic<std::uintptr_t> coins_[kSlots]{};
    std::atomic<std::uint32_t>  tossedAt_[kSlots]{};  // seconds on the steady clock
};

}  // namespace lss::battle::coin_table
