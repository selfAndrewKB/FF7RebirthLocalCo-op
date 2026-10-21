// Host-side tests for player_two::call_site, run without the game.
//
// A page with one `call rel32` stands in for a caller in the game: the check recognises the call to its target and
// nothing else (another target, another instruction, a null, memory that cannot be read), and the one-block rule is
// the 16-byte arithmetic the call redirect applies.

#include "player_two/call_site.h"

#include "exec_page.h"
#include "expect.h"

#include <cstdint>
#include <cstdio>

namespace {

using lss::player_two::call_site::Calls;
using lss::player_two::call_site::FitsOneBlock;

constexpr std::size_t kCallOffset   = 0x07;
constexpr std::size_t kTargetOffset = 0x40;

/// page+0x07: call target ; page+0x40: target: mov eax, 1 ; ret (the rest int3).
void FillCaller(lss::test::ExecutablePage& page)
{
    const std::uint8_t call[]   = {0xE8, 0x00, 0x00, 0x00, 0x00};
    const std::uint8_t target[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};
    page.Write(kCallOffset, call);
    page.Write(kTargetOffset, target);
    page.WriteRel32(kCallOffset, kTargetOffset);
    page.Seal();
}

void TestCalls()
{
    std::puts("call site: a call rel32 to the target is recognised; another target, another instruction, nulls and "
              "unreadable memory are not");
    lss::test::ExecutablePage page;
    FillCaller(page);
    EXPECT(Calls(page.At(kCallOffset), page.At(kTargetOffset)));
    EXPECT(!Calls(page.At(kCallOffset), page.At(kTargetOffset + 1)));
    EXPECT(!Calls(page.At(kCallOffset + 1), page.At(kTargetOffset)));  // the displacement bytes, not a call
    EXPECT(!Calls(page.At(0), page.At(kTargetOffset)));                 // int3
    EXPECT(!Calls(0, page.At(kTargetOffset)));
    EXPECT(!Calls(page.At(kCallOffset), 0));

    const lss::test::NoAccessPage unreadable;
    EXPECT(!Calls(unreadable.At(0x10), page.At(kTargetOffset)));
}

void TestFitsOneBlock()
{
    std::puts("call site: five bytes fit one aligned 16-byte block from offsets 0 to 11 only");
    lss::test::ExecutablePage page;  // page-aligned, so its offsets are the block offsets
    FillCaller(page);
    for (std::size_t offset = 0; offset < 32; ++offset)
    {
        EXPECT(FitsOneBlock(page.At(offset)) == (offset % 16 <= 11));
    }
}

}  // namespace

int main()
{
    TestCalls();
    TestFitsOneBlock();

    return lss::test::Report();
}
