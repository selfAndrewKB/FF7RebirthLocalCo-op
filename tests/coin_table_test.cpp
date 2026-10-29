// Host-side tests for battle::coin_table, run without the game.
//
// The enemies' coins: a key's own slot is reused; an empty slot is preferred; within a key's four slots the oldest
// coin is evicted, never the one asked to keep; a clear forgets only the matching key; the fall-back turns player 2
// into player 1 only while the coin says player 2; keys share the low bits with the player.

#include "battle/coin_table.h"

#include "expect.h"

#include <atomic>
#include <cstdio>
#include <vector>

namespace {

using namespace lss::battle::coin_table;

/// Five keys (16-aligned, as controllers are) whose first slot is the same, so they compete for one probe window.
std::vector<std::uintptr_t> Colliding(std::size_t count)
{
    std::vector<std::uintptr_t> keys;
    const std::uintptr_t        first = 0x1000;
    const std::size_t           slot  = SlotOf(first, 0);
    for (std::uintptr_t key = first; keys.size() < count; key += 16)
    {
        if (SlotOf(key, 0) == slot)
        {
            keys.push_back(key);
        }
    }
    return keys;
}

void TestSetAndFind()
{
    std::puts("coin table: a coin names its player; a key's own slot is reused; an unknown key has none");
    CoinTable table;
    EXPECT(table.PlayerOf(0x1000) == 0);
    EXPECT(table.Find(0x1000) == kNoSlot);
    table.Set(0x1000, 2, 100);
    EXPECT(table.PlayerOf(0x1000) == 2);
    const std::size_t slot = table.Find(0x1000);
    EXPECT(slot != kNoSlot && slot == SlotOf(0x1000, 0));
    table.Set(0x1000, 1, 200);  // tossed again: the same slot
    EXPECT(table.PlayerOf(0x1000) == 1);
    EXPECT(table.Find(0x1000) == slot);
    EXPECT(table.PlayerOf(0x1010) == 0);
    EXPECT(table.PlayerOf(0x1001) == 0);  // the low bits are the player's, never a key's
}

void TestEviction()
{
    std::puts("coin table: an empty slot is preferred; then the oldest of the four is evicted; a kept slot never is");
    const auto keys = Colliding(6);
    CoinTable  table;
    for (std::size_t i = 0; i < kProbe; ++i)
    {
        table.Set(keys[i], 1, static_cast<std::uint32_t>(10 + i));  // keys[0] is the oldest
        EXPECT(table.Find(keys[i]) == SlotOf(keys[0], i));          // each took the next empty slot
    }
    table.Set(keys[4], 2, 100);  // no empty slot: the oldest (keys[0]) goes
    EXPECT(table.PlayerOf(keys[0]) == 0);
    EXPECT(table.PlayerOf(keys[4]) == 2);
    EXPECT(table.Find(keys[4]) == SlotOf(keys[0], 0));
    for (std::size_t i = 1; i < kProbe; ++i)
    {
        EXPECT(table.PlayerOf(keys[i]) == 1);
    }

    // keys[1] is the oldest now, but its slot is kept: keys[2] goes instead.
    table.Set(keys[5], 1, 101, table.Find(keys[1]));
    EXPECT(table.PlayerOf(keys[1]) == 1);
    EXPECT(table.PlayerOf(keys[2]) == 0);
    EXPECT(table.PlayerOf(keys[5]) == 1);

    // Ages are unsigned seconds: a coin "tossed in the future" is the youngest.
    CoinTable later;
    for (std::size_t i = 0; i < kProbe; ++i)
    {
        later.Set(keys[i], 1, i == 2 ? 5 : 50);  // keys[2] tossed longest ago
    }
    later.Set(keys[4], 1, 60);
    EXPECT(later.PlayerOf(keys[2]) == 0);
    EXPECT(later.PlayerOf(keys[0]) == 1 && later.PlayerOf(keys[1]) == 1 && later.PlayerOf(keys[3]) == 1);
}

void TestClearAndFallBack()
{
    std::puts("coin table: a clear forgets only the matching key; the fall-back changes player 2 to 1 only while it is 2");
    CoinTable table;
    table.Clear(0x2000);  // nothing to forget
    EXPECT(table.PlayerOf(0x2000) == 0);
    table.Set(0x2000, 2, 1);
    table.Set(0x3000, 1, 1);
    table.Clear(0x3000);
    EXPECT(table.PlayerOf(0x3000) == 0);
    EXPECT(table.PlayerOf(0x2000) == 2);

    EXPECT(table.FallBack(0x2000) == 2);  // it named player 2
    EXPECT(table.PlayerOf(0x2000) == 1);
    EXPECT(!table.FallBack(0x2000));  // player 1 already
    EXPECT(table.PlayerOf(0x2000) == 1);
    EXPECT(!table.FallBack(0x3000));  // no coin
    table.Clear(0x2000);
    EXPECT(table.PlayerOf(0x2000) == 0);
    EXPECT(!table.FallBack(0x2000));
}

/// The fall-back as it stood at v0.91.0, verbatim but for the table it reads: player 2 becomes player 1.
bool OldFallBack(std::atomic<std::uintptr_t>& coin, std::uintptr_t key)
{
    std::uintptr_t held = coin.load(std::memory_order_relaxed);
    return held == (key | 2u) && coin.compare_exchange_strong(held, key | 1u, std::memory_order_relaxed);
}

void TestPlayerThree()
{
    std::puts("coin table: player 3's number fits beside the key; the fall-back turns any seat's player into player 1, "
              "as before for player 2");
    static_assert(PlayerNumber(0) == 1 && PlayerNumber(lss::players::kPlayerTwoSeat) == 2 && PlayerNumber(2) == 3);
    static_assert(SeatOfNumber(PlayerNumber(2)) == 2);
    CoinTable table;
    table.Set(0x4000, 3, 1);
    EXPECT(table.PlayerOf(0x4000) == 3);
    EXPECT(table.PlayerOf(0x4010) == 0);
    EXPECT(table.FallBack(0x4000) == 3);  // it named player 3
    EXPECT(table.PlayerOf(0x4000) == 1);
    EXPECT(!table.FallBack(0x4000));

    // Two players: the same answer and the same coin afterwards as the old rule, for each value a coin can hold.
    for (int held = 1; held <= 2; ++held)
    {
        std::atomic<std::uintptr_t> old{0x5000u | static_cast<std::uintptr_t>(held)};
        CoinTable                   now;
        now.Set(0x5000, held, 1);
        EXPECT(OldFallBack(old, 0x5000) == (now.FallBack(0x5000) != 0));
        EXPECT(static_cast<int>(old.load() & kMask) == now.PlayerOf(0x5000));
    }
}

}  // namespace

int main()
{
    TestSetAndFind();
    TestEviction();
    TestClearAndFallBack();
    TestPlayerThree();

    return lss::test::Report();
}
