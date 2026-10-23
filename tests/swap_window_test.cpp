// Host-side tests for player_two::SwapWindow, run without the game.
//
// The slot stands in for the party manager's primary-controller field. A window replaces it only
// from the value the caller expects, restores it only if the replacement is still there, and a
// window opened inside another one leaves both the slot and the outer window alone.

#include "player_two/scopes/swap_window.h"

#include "expect.h"

#include <cstdint>
#include <cstdio>

namespace {

using lss::player_two::SwapWindow;
using Opened = SwapWindow::Opened;

constexpr std::uint64_t kPlayerOne = 0x0000000700001234;  // {serial 7, index 0x1234}
constexpr std::uint64_t kPlayerTwo = 0x0000000900005678;
constexpr std::uint64_t kSomeoneElse = 0x0000000B00009ABC;

void TestSwapAndRestore()
{
    std::puts("window: swaps from the expected value and restores it");
    alignas(8) std::uint64_t slot = kPlayerOne;
    SwapWindow window;
    EXPECT(window.Open(&slot, kPlayerOne, kPlayerTwo) == Opened::Swapped);
    EXPECT(window.IsOpen());
    EXPECT(slot == kPlayerTwo);
    EXPECT(window.Close());
    EXPECT(!window.IsOpen());
    EXPECT(slot == kPlayerOne);
    EXPECT(window.Close());  // closing twice changes nothing
    EXPECT(slot == kPlayerOne);
}

void TestUnexpectedValueIsNotReplaced()
{
    std::puts("window: a slot holding anything but the expected value is left alone");
    alignas(8) std::uint64_t slot = kSomeoneElse;
    SwapWindow window;
    EXPECT(window.Open(&slot, kPlayerOne, kPlayerTwo) == Opened::Changed);
    EXPECT(!window.IsOpen());
    EXPECT(slot == kSomeoneElse);
    EXPECT(window.Close());
    EXPECT(slot == kSomeoneElse);
}

void TestWriteInsideTheWindowWins()
{
    std::puts("window: a value written inside the window is newer than ours and is kept");
    alignas(8) std::uint64_t slot = kPlayerOne;
    SwapWindow window;
    EXPECT(window.Open(&slot, kPlayerOne, kPlayerTwo) == Opened::Swapped);
    slot = kSomeoneElse;  // the engine re-assigned the field during the call
    EXPECT(!window.Close());
    EXPECT(slot == kSomeoneElse);
    EXPECT(!window.IsOpen());
}

void TestNestedWindowLeavesTheOuterOneAlone()
{
    std::puts("window: a nested window over the same slot does nothing, and the outer one restores");
    alignas(8) std::uint64_t slot = kPlayerOne;
    SwapWindow outer;
    SwapWindow inner;
    EXPECT(outer.Open(&slot, kPlayerOne, kPlayerTwo) == Opened::Swapped);
    EXPECT(inner.Open(&slot, kPlayerOne, kPlayerTwo) == Opened::AlreadyHeld);
    EXPECT(!inner.IsOpen());
    EXPECT(inner.Close());
    EXPECT(slot == kPlayerTwo);  // the inner close must not restore under the outer window
    EXPECT(outer.Close());
    EXPECT(slot == kPlayerOne);
}

void TestMisalignedAndNullSlotsAreRefused()
{
    std::puts("window: null and misaligned slots are refused without a write");
    SwapWindow window;
    EXPECT(window.Open(nullptr, kPlayerOne, kPlayerTwo) == Opened::Misaligned);
    alignas(8) unsigned char bytes[16]{};
    auto* misaligned = reinterpret_cast<std::uint64_t*>(bytes + 4);
    EXPECT(window.Open(misaligned, 0, kPlayerTwo) == Opened::Misaligned);
    for (const unsigned char byte : bytes)
    {
        EXPECT(byte == 0);
    }
}

}  // namespace

int main()
{
    TestSwapAndRestore();
    TestUnexpectedValueIsNotReplaced();
    TestWriteInsideTheWindowWins();
    TestNestedWindowLeavesTheOuterOneAlone();
    TestMisalignedAndNullSlotsAreRefused();

    return lss::test::Report();
}
