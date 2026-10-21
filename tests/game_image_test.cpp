// Host-side tests for platform/image's RIP-relative read, run without the game.
//
// A byte buffer stands in for the game's image (GameImage::At): a RIP-relative instruction at a research address
// is read through the image, its opcode checked, its displacement taken from its end; a site whose bytes are not the
// opcode, or that lies outside the image, reads empty.

#include "platform/image.h"
#include "platform/log.h"

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

using lss::platform::GameImage;

constexpr std::uintptr_t kIdaBase  = 0x140000000;
constexpr std::uint8_t   kLea[]    = {0x48, 0x8D, 0x05};  // lea rax, [rip+disp32]
constexpr std::uint8_t   kCmpEbx[] = {0x39, 0x1D};        // cmp dword [rip+disp32], ebx

void TestRipTarget()
{
    std::puts("image: a RIP-relative instruction's target is read from its displacement at its end, in the image's space");
    alignas(16) std::array<std::uint8_t, 0x40> code{};
    code.fill(0x90);
    // +0x08: lea rax, [rip + 0x10] -> reaches +0x08 + 7 + 0x10 = +0x1F
    std::memcpy(code.data() + 0x08, kLea, sizeof(kLea));
    const std::int32_t forward = 0x10;
    std::memcpy(code.data() + 0x0B, &forward, sizeof(forward));
    // +0x20: cmp dword [rip - 0x30], ebx -> reaches +0x20 + 6 - 0x30 = -0x0A (before the buffer, still an address)
    std::memcpy(code.data() + 0x20, kCmpEbx, sizeof(kCmpEbx));
    const std::int32_t back = -0x30;
    std::memcpy(code.data() + 0x22, &back, sizeof(back));

    const auto      base  = reinterpret_cast<std::uintptr_t>(code.data());
    const GameImage image = GameImage::At(base, code.size());
    EXPECT(image.Rebase(kIdaBase + 0x08) == base + 0x08);

    EXPECT(image.RipTarget(kIdaBase + 0x08, kLea) == base + 0x1F);
    EXPECT(image.RipTarget(kIdaBase + 0x20, kCmpEbx) == base - 0x0A);
    EXPECT(!image.RipTarget(kIdaBase + 0x08, kCmpEbx).has_value());  // not that opcode
    EXPECT(!image.RipTarget(kIdaBase + 0x09, kLea).has_value());     // off by one
    EXPECT(!image.RipTarget(kIdaBase + 0x3C, kLea).has_value());     // the displacement would run past the image
    EXPECT(!image.RipTarget(kIdaBase - 0x10, kLea).has_value());     // before the image
}

}  // namespace

int main()
{
    TestRipTarget();

    return lss::test::Report();
}
