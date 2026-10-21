// Host-side tests for the overlay's layout-scale code (hud/layout_scale_code.h), run without the game.
//
// The bytes the mod places over the overlay's layout-scale instruction are EXECUTED here: a harness takes the rect's
// width in ecx and height in edx, as the game has them at the site, runs the code and returns xmm0. The result must be
// the contain rule, min(H/1080, W/1920), with the game's constants, and for the 16:9 rects the game makes it must be
// exactly the game's own H/1080. The research build's code follows the game's two instructions before the site; the
// 1.005 code carries those two itself.

#include "hud/layout_scale_code.h"

#include "exec_page.h"
#include "expect.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

using namespace lss::hud::layout_scale_code;

/// float harness(int width /*ecx*/, int height /*edx*/): xmm0 is the layout scale.
using Harness = float (*)(int width, int height);

// xmm7 is callee-saved on Win64 (the game's function saves it in its own prologue): kept around the code here.
constexpr std::uint8_t kSaveXmm7[]    = {0x48, 0x83, 0xEC, 0x18,         // sub rsp, 18h
                                         0xC5, 0xF8, 0x11, 0x3C, 0x24};  // vmovups [rsp], xmm7
constexpr std::uint8_t kRestoreXmm7[] = {0xC5, 0xF8, 0x10, 0x3C, 0x24,   // vmovups xmm7, [rsp]
                                         0x48, 0x83, 0xC4, 0x18,         // add rsp, 18h
                                         0xC3};                          // ret
constexpr std::uint8_t kGamesTwo[] = {0xC5, 0xC0, 0x57, 0xFF,   // vxorps xmm7, xmm7, xmm7      (the game's, before the site)
                                      0xC5, 0xC2, 0x2A, 0xFA};  // vcvtsi2ss xmm7, xmm7, edx    H

void FillResearch(lss::test::ExecutablePage& page)
{
    std::size_t at = 0;
    page.Write(at, kSaveXmm7);
    at += sizeof(kSaveXmm7);
    page.Write(at, kGamesTwo);
    at += sizeof(kGamesTwo);
    page.Write(at, kCode);  // ends with its jmp short over the constants, to the byte after them
    at += sizeof(kCode);
    page.Write(at, kRestoreXmm7);
    page.Seal();
}

void Fill1005(lss::test::ExecutablePage& page)
{
    std::size_t at = 0;
    page.Write(at, kSaveXmm7);
    at += sizeof(kSaveXmm7);
    page.Write(at, kCode1005);
    at += sizeof(kCode1005);
    page.Write(at, kRestoreXmm7);
    page.Seal();
}

float Constant(std::size_t at)
{
    float value = 0;
    std::memcpy(&value, kCode + at, sizeof(value));
    return value;
}

struct Rect
{
    int width, height;
};
constexpr Rect kRects[] = {{1920, 1080}, {2560, 1440}, {3840, 2160}, {3440, 1440}, {7680, 1440},  // 16:9 and wider
                           {1280, 1440}, {1280, 720},  {2560, 720},  {100, 100},   {1, 1}};       // narrower, odd

bool Near(float a, float b)
{
    return std::fabs(a - b) <= 1e-6f * std::max(1.0f, std::fabs(b));
}

void TestTheBytes()
{
    std::puts("layout scale code: the constants are the game's 1/1080 and 1/1920; 1.005's code is the two replaced "
              "instructions then the research code; the site offsets agree");
    EXPECT(Near(Constant(kConstant1080) * 1080.0f, 1.0f));
    EXPECT(Near(Constant(kConstant1920) * 1920.0f, 1.0f));
    EXPECT(std::memcmp(kCode1005, kContextBytes, kReplaced1005) == 0);
    EXPECT(std::memcmp(kCode1005 + kReplaced1005, kCode, sizeof(kCode)) == 0);
    EXPECT(kSite - kContext.address == 8);
    EXPECT(kResume - kSite == kReplaced);
    EXPECT(kCode[24] == 0xEB && kCode[25] == sizeof(kCode) - 26);  // the jump lands right after the constants
}

void TestContainRule(const char* build, void (*fill)(lss::test::ExecutablePage&))
{
    std::printf("layout scale code (%s): min(H/1080, W/1920); the game's H/1080 for 16:9 and wider\n", build);
    lss::test::ExecutablePage page;
    fill(page);
    const auto harness = page.Function<Harness>();
    for (const Rect& rect : kRects)
    {
        const float byHeight = static_cast<float>(rect.height) * Constant(kConstant1080);
        const float byWidth  = static_cast<float>(rect.width) * Constant(kConstant1920);
        const float expected = std::min(byHeight, byWidth);
        const float actual   = harness(rect.width, rect.height);
        EXPECT(Near(actual, expected));
        if (rect.width * 1080 >= rect.height * 1920)
        {
            EXPECT(actual == byHeight);  // unchanged for the game's own images
        }
        else
        {
            EXPECT(actual < byHeight);  // a tall band: the width rules
        }
    }
    EXPECT(Near(harness(1920, 1080), 1.0f));
    EXPECT(Near(harness(1280, 1440), 1280.0f / 1920.0f));
}

}  // namespace

int main()
{
    TestTheBytes();
    TestContainRule("research", &FillResearch);
    TestContainRule("1.005", &Fill1005);

    return lss::test::Report();
}
