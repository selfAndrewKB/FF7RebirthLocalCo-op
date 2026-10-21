// Host-side tests for platform/block_patch, run without the game.
//
// The change every redirect and in-place patch makes, on a page standing in for the game's code: bytes inside one
// block change in one swap and come back; a change crossing into the next block is refused unless allowed, then
// takes both swaps; bytes that are not the expected ones, lengths that differ, a second change while one is applied,
// and a revert after someone else changed the block are refused; a reverted change is applied again from its own
// bytes.

#include "platform/block_patch.h"
#include "platform/log.h"

#include "exec_page.h"
#include "expect.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace lss::platform {
void WriteLine(std::wstring_view line) noexcept  // the mod's log, to the console here
{
    std::printf("  log: %ls\n", std::wstring(line).c_str());
}
namespace detail {
std::atomic<std::uint8_t> g_level{static_cast<std::uint8_t>(Level::Trace)};  // every level, to the console
}
const wchar_t* LevelName(Level) noexcept
{
    return L"level";
}
}  // namespace lss::platform

namespace {

using lss::platform::BlockPatch;

/// A sealed page of 64 nops: four blocks of code nothing runs.
class Page
{
public:
    Page()
    {
        std::array<std::uint8_t, 0x40> nops{};
        nops.fill(0x90);
        page_.Write(0, nops);
        page_.Seal();
    }
    std::uintptr_t At(std::size_t offset) const noexcept
    {
        return page_.At(offset);
    }
    std::uint8_t Byte(std::size_t offset) const noexcept
    {
        std::uint8_t byte = 0;
        std::memcpy(&byte, reinterpret_cast<const void*>(At(offset)), 1);
        return byte;
    }

private:
    lss::test::ExecutablePage page_;
};

constexpr std::uint8_t kNops3[]   = {0x90, 0x90, 0x90};
constexpr std::uint8_t kBytes3[]  = {0x01, 0x02, 0x03};
constexpr std::uint8_t kWrong3[]  = {0x90, 0xCC, 0x90};
constexpr std::uint8_t kBytes2[]  = {0x01, 0x02};
constexpr std::uint8_t kNops5[]   = {0x90, 0x90, 0x90, 0x90, 0x90};
constexpr std::uint8_t kBytes5[]  = {0x01, 0x02, 0x03, 0x04, 0x05};
constexpr std::uint8_t kNop[]     = {0x90};
constexpr std::uint8_t kByteA[]   = {0xAA};
constexpr std::uint8_t kByteB[]   = {0xBB};

void TestApplyRevertReapply()
{
    std::puts("block patch: bytes inside a block change in one swap, come back, and change again from the same bytes");
    const Page page;
    BlockPatch patch;
    EXPECT(!patch.IsApplied() && patch.Block() == 0);
    EXPECT(!patch.Reapply());  // nothing ever applied
    EXPECT(patch.Apply(page.At(0x05), kNops3, kBytes3));
    EXPECT(patch.IsApplied() && !patch.Crosses() && patch.Block() == page.At(0));
    EXPECT(page.Byte(0x05) == 1 && page.Byte(0x06) == 2 && page.Byte(0x07) == 3);
    EXPECT(page.Byte(0x04) == 0x90 && page.Byte(0x08) == 0x90);
    EXPECT(!patch.Reapply());  // applied already
    EXPECT(patch.Revert());
    EXPECT(!patch.IsApplied());
    EXPECT(page.Byte(0x05) == 0x90 && page.Byte(0x06) == 0x90 && page.Byte(0x07) == 0x90);
    EXPECT(patch.Revert());  // nothing applied: nothing to do
    EXPECT(patch.Reapply());
    EXPECT(patch.IsApplied() && page.Byte(0x06) == 2);
    EXPECT(patch.Revert());
    EXPECT(page.Byte(0x06) == 0x90);
}

void TestRefusals()
{
    std::puts("block patch: refuses other bytes, another length, a change over two blocks unless allowed, and a second "
              "change while one is applied");
    const Page page;
    BlockPatch patch;
    EXPECT(!patch.Apply(page.At(0x05), kWrong3, kBytes3));
    EXPECT(!patch.Apply(page.At(0x05), kNops3, kBytes2));
    EXPECT(!patch.Apply(page.At(0x0E), kNops3, kBytes3));  // 0x0E..0x10 crosses into the second block
    EXPECT(!patch.IsApplied() && page.Byte(0x05) == 0x90 && page.Byte(0x0E) == 0x90 && page.Byte(0x10) == 0x90);
    EXPECT(patch.Apply(page.At(0x05), kNops3, kBytes3));
    EXPECT(!patch.Apply(page.At(0x20), kNops3, kBytes3));  // one change at a time
    EXPECT(page.Byte(0x20) == 0x90);
    EXPECT(patch.Revert());
}

void TestCrossing()
{
    std::puts("block patch: a change over two blocks, allowed, takes both swaps, comes back whole, and goes in again");
    const Page page;
    BlockPatch patch;
    EXPECT(patch.Apply(page.At(0x0D), kNops5, kBytes5, true));
    EXPECT(patch.IsApplied() && patch.Crosses() && patch.Block() == page.At(0));
    EXPECT(page.Byte(0x0D) == 1 && page.Byte(0x0F) == 3 && page.Byte(0x10) == 4 && page.Byte(0x11) == 5);
    EXPECT(page.Byte(0x0C) == 0x90 && page.Byte(0x12) == 0x90);
    EXPECT(patch.Revert());
    EXPECT(page.Byte(0x0D) == 0x90 && page.Byte(0x10) == 0x90 && page.Byte(0x11) == 0x90);
    EXPECT(patch.Reapply());
    EXPECT(page.Byte(0x0F) == 3 && page.Byte(0x10) == 4);
    EXPECT(patch.Revert());
}

void TestChangedAfterUs()
{
    std::puts("block patch: a block someone else changed after us is left alone: the revert says false, the change "
              "counts as applied");
    const Page page;
    BlockPatch ours, theirs;
    EXPECT(ours.Apply(page.At(0x02), kNop, kByteA));
    EXPECT(theirs.Apply(page.At(0x03), kNop, kByteB));  // the same block, another byte
    EXPECT(!ours.Revert());                              // the block is no longer as ours left it
    EXPECT(ours.IsApplied() && page.Byte(0x02) == 0xAA);
    EXPECT(theirs.Revert());  // theirs read the block with ours in: it is as theirs left it
    EXPECT(ours.Revert());
    EXPECT(page.Byte(0x02) == 0x90 && page.Byte(0x03) == 0x90);
}

}  // namespace

int main()
{
    TestApplyRevertReapply();
    TestRefusals();
    TestCrossing();
    TestChangedAfterUs();

    return lss::test::Report();
}
