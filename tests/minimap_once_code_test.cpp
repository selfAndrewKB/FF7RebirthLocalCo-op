// Host-side tests for the minimap skip code, run without the game.
//
// The bytes the mod writes near the game (hud/minimap_once_code.h) are EXECUTED here, in a harness that
// recreates what they rely on at each site: r15 = the circle widget (its +0x1C0 read by the replaced
// instruction), stack room at [rsp+70h..7Fh] for the octagon's stores, and xmm13 non-zero beforehand.
// The harness returns 0 when the code fell through to the replaced instruction (the stock path) and
// 0x100 when it jumped past the submit with xmm13 cleared (anything else: the skip path is wrong).

#include "hud/minimap_once_code.h"
#include "platform/x64_jump.h"

#include "exec_page.h"
#include "expect.h"

#include <cstdio>
#include <cstring>
#include <iterator>
#include <span>
#include <vector>

namespace {

using lss::hud::MinimapOnceFields;

/// std::uint64_t harness(const void* widget /*rcx*/)
using Harness = std::uint64_t (*)(const void* widget);

constexpr std::uint8_t kPrologue[] = {
    0x41, 0x57,                                      // push    r15
    0x48, 0x81, 0xEC, 0x98, 0x00, 0x00, 0x00,        // sub     rsp, 98h
    0xC5, 0x7A, 0x7F, 0xAC, 0x24, 0x80, 0, 0, 0,     // vmovdqu [rsp+80h], xmm13   (callee-saved)
    0xC4, 0x41, 0x11, 0x76, 0xED,                    // vpcmpeqd xmm13, xmm13, xmm13  (all ones)
    0x49, 0x89, 0xCF};                               // mov     r15, rcx           the widget
constexpr std::uint8_t kStockEnd[] = {
    0x31, 0xC0,                                      // xor     eax, eax           stock path: 0
    0xEB, 0x0B};                                     // jmp     out
constexpr std::uint8_t kSkipped[] = {
    0xC4, 0x61, 0xF9, 0x7E, 0xE8,                    // vmovq   rax, xmm13         0 if the code cleared it
    0x48, 0x0D, 0x00, 0x01, 0x00, 0x00};             // or      rax, 100h
constexpr std::uint8_t kEpilogue[] = {
    0xC5, 0x7A, 0x6F, 0xAC, 0x24, 0x80, 0, 0, 0,     // out: vmovdqu xmm13, [rsp+80h]
    0x48, 0x81, 0xC4, 0x98, 0x00, 0x00, 0x00,        // add     rsp, 98h
    0x41, 0x5F,                                      // pop     r15
    0xC3};                                           // ret

// Game version 1.005's circle also stores 1.0f to [rsp+40h] on the skip path: the slot is cleared before the site and
// returned in the result's high half (0x3F800000'00000100 for a correct skip).
constexpr std::uint8_t kClearSlot1005[] = {
    0xC7, 0x44, 0x24, 0x40, 0x00, 0x00, 0x00, 0x00};  // mov     dword [rsp+40h], 0
constexpr std::uint8_t kStockEnd1005[] = {
    0x31, 0xC0,                                      // xor     eax, eax           stock path: 0
    0xEB, 0x16};                                     // jmp     out
constexpr std::uint8_t kSkipped1005[] = {
    0xC4, 0x61, 0xF9, 0x7E, 0xE8,                    // vmovq   rax, xmm13         0 if the code cleared it
    0x48, 0x0D, 0x00, 0x01, 0x00, 0x00,              // or      rax, 100h
    0x8B, 0x54, 0x24, 0x40,                          // mov     edx, [rsp+40h]     the slot read after the submit
    0x48, 0xC1, 0xE2, 0x20,                          // shl     rdx, 20h
    0x48, 0x09, 0xD0};                               // or      rax, rdx
static_assert(sizeof(kSkipped1005) == 0x16);

/// The harness around one site's code: `before` runs just before the site, `stockEnd` ends the stock path by jumping
/// over `skipped`, the skip path's landing point.
struct Tail
{
    std::span<const std::uint8_t> before;
    std::span<const std::uint8_t> stockEnd;
    std::span<const std::uint8_t> skipped;
};
constexpr Tail kTail{{}, kStockEnd, kSkipped};
constexpr Tail kTail1005{kClearSlot1005, kStockEnd1005, kSkipped1005};

template <class Build>
std::vector<std::uint8_t> HarnessCode(std::uintptr_t base, std::size_t siteSize, Build&& build, const Tail& tail)
{
    std::vector<std::uint8_t> code(std::begin(kPrologue), std::end(kPrologue));
    code.insert(code.end(), tail.before.begin(), tail.before.end());
    const std::size_t siteAt  = code.size();
    const std::size_t skipped = siteAt + siteSize + lss::platform::kJumpLength + tail.stockEnd.size();
    const auto        site    = build(base + skipped);  // the skip path's jump target
    code.insert(code.end(), site.begin(), site.end());
    const auto from = base + code.size();
    const auto back = lss::platform::EncodeJump(from, from + lss::platform::kJumpLength);  // the redirect's jump back
    code.insert(code.end(), back->begin(), back->end());
    code.insert(code.end(), tail.stockEnd.begin(), tail.stockEnd.end());
    code.insert(code.end(), tail.skipped.begin(), tail.skipped.end());
    code.insert(code.end(), std::begin(kEpilogue), std::end(kEpilogue));
    return code;
}

template <class Build>
std::uint64_t Run(std::size_t siteSize, Build&& build, const Tail& tail = kTail)
{
    lss::test::ExecutablePage page;
    const auto code = HarnessCode(page.At(), siteSize, build, tail);
    page.Write(0, code);
    page.Seal();
    std::uint8_t widget[0x200]{};
    const std::uint64_t result = page.Function<Harness>()(widget);
    return result;
}

void TestCircle()
{
    std::puts("circle: the stock path runs the replaced load; the skip path clears xmm13 and jumps past the submit");
    MinimapOnceFields fields;
    auto build = [&fields](std::uintptr_t target) { return lss::hud::CircleSkipCode(fields, target); };
    EXPECT(Run(lss::hud::kCircleSkipCodeSize, build) == 0);
    EXPECT(fields.circlePainted == 1 && fields.circleSkipped == 0);
    fields.skip = 1;
    EXPECT(Run(lss::hud::kCircleSkipCodeSize, build) == 0x100);
    EXPECT(fields.circlePainted == 1 && fields.circleSkipped == 1);
    EXPECT(fields.octagonPainted == 0 && fields.octagonSkipped == 0);
}

void TestCircle1005()
{
    std::puts("circle (1.005): the stock path runs the replaced vmovq; the skip path also stores 1.0f to [rsp+40h]");
    MinimapOnceFields fields;
    auto build = [&fields](std::uintptr_t target) { return lss::hud::CircleSkipCode1005(fields, target); };
    EXPECT(Run(lss::hud::kCircleSkipCodeSize1005, build, kTail1005) == 0);
    EXPECT(fields.circlePainted == 1 && fields.circleSkipped == 0);
    fields.skip = 1;
    EXPECT(Run(lss::hud::kCircleSkipCodeSize1005, build, kTail1005) == 0x3F80000000000100ull);
    EXPECT(fields.circlePainted == 1 && fields.circleSkipped == 1);
    EXPECT(fields.octagonPainted == 0 && fields.octagonSkipped == 0);
}

void TestOctagon()
{
    std::puts("octagon: the same, with the site's two stores done before the jump");
    MinimapOnceFields fields;
    auto build = [&fields](std::uintptr_t target) { return lss::hud::OctagonSkipCode(fields, target); };
    EXPECT(Run(lss::hud::kOctagonSkipCodeSize, build) == 0);
    EXPECT(fields.octagonPainted == 1 && fields.octagonSkipped == 0);
    fields.skip = 1;
    EXPECT(Run(lss::hud::kOctagonSkipCodeSize, build) == 0x100);
    EXPECT(fields.octagonPainted == 1 && fields.octagonSkipped == 1);
    EXPECT(fields.circlePainted == 0 && fields.circleSkipped == 0);
}

void TestSizes()
{
    std::puts("code: each ends with the instruction it replaces");
    const MinimapOnceFields fields;
    const auto circle  = lss::hud::CircleSkipCode(fields, 0);
    const auto octagon = lss::hud::OctagonSkipCode(fields, 0);
    EXPECT(std::memcmp(circle.data() + circle.size() - sizeof(lss::hud::kCircleSiteBytes), lss::hud::kCircleSiteBytes,
                       sizeof(lss::hud::kCircleSiteBytes)) == 0);
    EXPECT(std::memcmp(octagon.data() + octagon.size() - sizeof(lss::hud::kOctagonSiteBytes), lss::hud::kOctagonSiteBytes,
                       sizeof(lss::hud::kOctagonSiteBytes)) == 0);
    const auto circle1005 = lss::hud::CircleSkipCode1005(fields, 0);
    EXPECT(std::memcmp(circle1005.data() + circle1005.size() - sizeof(lss::hud::kCircleSiteBytes1005),
                       lss::hud::kCircleSiteBytes1005, sizeof(lss::hud::kCircleSiteBytes1005)) == 0);
}

}  // namespace

int main()
{
    TestCircle();
    TestCircle1005();
    TestOctagon();
    TestSizes();

    return lss::test::Report();
}
