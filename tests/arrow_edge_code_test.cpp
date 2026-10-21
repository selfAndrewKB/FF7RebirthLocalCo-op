// Host-side tests for the arrow edge code, run without the game.
//
// The bytes the mod writes near the game (hud/arrow_edge_code.h) are EXECUTED here, installed through
// platform::CodeRedirect over a stand-in: it loads a case into the registers the game has there
// (xmm0 ySide, xmm1 |angle|, xmm6 0, xmm8, xmm10 cy, xmm13, xmm14 cx/|dx|, the float at [rsp+24h]),
// reaches `vfmadd231ss xmm8, xmm13, [rsp+24h]` 8 bytes into a 16-byte block (0x140A59DB8 in the game),
// and returns 1 from its side stub, 2 from its top/bottom stub (3 if the code fell through), with
// xmm8 stored back so the replaced instruction's result is checked too.

#include "hud/arrow_edge_code.h"
#include "platform/code_redirect.h"

#include "exec_page.h"
#include "expect.h"

#include <cstdio>
#include <cstring>

namespace {

struct Case
{
    float ySide, angle, zero, fmaAcc, cy, fmaMul, cxOverDx, fmaStack;
    float fmaOut;
};
static_assert(sizeof(Case) == 36);

class FunctionLike
{
public:
    static constexpr std::size_t kSiteOffset      = 88;
    static constexpr std::size_t kResumeOffset    = 95;
    static constexpr std::size_t kSideOffset      = 102;
    static constexpr std::size_t kTopBottomOffset = 109;

    FunctionLike()
    {
        std::uint8_t code[160];
        std::memset(code, 0x90, sizeof(code));
        const std::uint8_t prologue[] = {
            0x48, 0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00,  //   0: sub rsp, 0A8h
            0xC5, 0xF8, 0x11, 0x74, 0x24, 0x30,        //   7: vmovups [rsp+30h], xmm6
            0xC5, 0x78, 0x11, 0x44, 0x24, 0x40,        //  13: vmovups [rsp+40h], xmm8
            0xC5, 0x78, 0x11, 0x54, 0x24, 0x50,        //  19: vmovups [rsp+50h], xmm10
            0xC5, 0x78, 0x11, 0x6C, 0x24, 0x60,        //  25: vmovups [rsp+60h], xmm13
            0xC5, 0x78, 0x11, 0x74, 0x24, 0x70,        //  31: vmovups [rsp+70h], xmm14
            0xC5, 0xFA, 0x10, 0x01,                    //  37: vmovss xmm0, [rcx]
            0xC5, 0xFA, 0x10, 0x49, 0x04,              //  41: vmovss xmm1, [rcx+4]
            0xC5, 0xFA, 0x10, 0x71, 0x08,              //  46: vmovss xmm6, [rcx+8]
            0xC5, 0x7A, 0x10, 0x41, 0x0C,              //  51: vmovss xmm8, [rcx+0Ch]
            0xC5, 0x7A, 0x10, 0x51, 0x10,              //  56: vmovss xmm10, [rcx+10h]
            0xC5, 0x7A, 0x10, 0x69, 0x14,              //  61: vmovss xmm13, [rcx+14h]
            0xC5, 0x7A, 0x10, 0x71, 0x18,              //  66: vmovss xmm14, [rcx+18h]
            0x8B, 0x41, 0x1C,                          //  71: mov eax, [rcx+1Ch]
            0x89, 0x44, 0x24, 0x24,                    //  74: mov [rsp+24h], eax
        };                                             //  78..87: nop
        const std::uint8_t body[] = {
            0xC4, 0x62, 0x11, 0xB9, 0x44, 0x24, 0x24,  //  88: vfmadd231ss xmm8, xmm13, [rsp+24h] (the site)
            0xB8, 0x03, 0x00, 0x00, 0x00,              //  95: mov eax, 3 (fell through)
            0xEB, 0x0C,                                // 100: jmp epilogue (114)
            0xB8, 0x01, 0x00, 0x00, 0x00,              // 102: side: mov eax, 1
            0xEB, 0x05,                                // 107: jmp epilogue (114)
            0xB8, 0x02, 0x00, 0x00, 0x00,              // 109: topBottom: mov eax, 2
            0xC5, 0x7A, 0x11, 0x41, 0x20,              // 114: epilogue: vmovss [rcx+20h], xmm8
            0xC5, 0xF8, 0x10, 0x74, 0x24, 0x30,        // 119: vmovups xmm6, [rsp+30h]
            0xC5, 0x78, 0x10, 0x44, 0x24, 0x40,        // 125: vmovups xmm8, [rsp+40h]
            0xC5, 0x78, 0x10, 0x54, 0x24, 0x50,        // 131: vmovups xmm10, [rsp+50h]
            0xC5, 0x78, 0x10, 0x6C, 0x24, 0x60,        // 137: vmovups xmm13, [rsp+60h]
            0xC5, 0x78, 0x10, 0x74, 0x24, 0x70,        // 143: vmovups xmm14, [rsp+70h]
            0x48, 0x81, 0xC4, 0xA8, 0x00, 0x00, 0x00,  // 149: add rsp, 0A8h
            0xC3,                                      // 156: ret
        };
        std::memcpy(code, prologue, sizeof(prologue));
        std::memcpy(code + kSiteOffset, body, sizeof(body));
        page_.Write(0, code);
        page_.Seal();
    }

    std::uintptr_t At(std::size_t offset) const { return page_.At(offset); }
    int            Call(Case& c) const { return page_.Function<int (*)(Case*)>()(&c); }

private:
    lss::test::ExecutablePage page_;
};

constexpr int kSide = 1, kTopBottom = 2;

/// Runs one case; the replaced FMA must give 2 + 3 * 4 = 14 whichever branch is taken.
int Run(const FunctionLike& function, float ySide, float angle, float cxOverDx)
{
    Case c{ySide, angle, 0.0f, 2.0f, 100.0f, 3.0f, cxOverDx, 4.0f, -1.0f};
    const int branch = function.Call(c);
    EXPECT(c.fmaOut == 14.0f);
    return branch;
}

void TestEdges()
{
    std::puts("arrow edges: the game's 60/120 split while the flag is clear, the corner test while set, the replaced "
              "FMA always, and restored");
    const FunctionLike function;
    alignas(8) static std::uint8_t split = 0;
    static const float sixty = 60.0f, oneTwenty = 120.0f;
    const auto code = lss::hud::ArrowEdgeCode(split, function.At(FunctionLike::kSideOffset),
                                              function.At(FunctionLike::kTopBottomOffset),
                                              reinterpret_cast<std::uintptr_t>(&sixty),
                                              reinterpret_cast<std::uintptr_t>(&oneTwenty));
    EXPECT(Run(function, 0, 0, 0) == 3);  // not installed: falls through
    lss::platform::CodeRedirect redirect;
    EXPECT(redirect.Install(function.At(FunctionLike::kSiteOffset), lss::hud::kArrowEdgeFmaBytes, code,
                            function.At(FunctionLike::kResumeOffset)));

    // Flag clear: the game's own choice by angle (cy = 100 throughout).
    EXPECT(Run(function, 150.0f, 50.0f, 1.0f) == kTopBottom);   // |a| <= 60
    EXPECT(Run(function, 150.0f, 60.0f, 1.0f) == kTopBottom);   // exactly 60: jbe
    EXPECT(Run(function, 999.0f, 90.0f, 1.0f) == kSide);        // 60 < |a| < 120, whatever ySide says
    EXPECT(Run(function, 150.0f, 120.0f, 1.0f) == kTopBottom);  // exactly 120: jae
    EXPECT(Run(function, 150.0f, 130.0f, 1.0f) == kTopBottom);

    // Flag set: where the ray leaves, whatever the angle.
    split = 1;
    EXPECT(Run(function, 150.0f, 90.0f, 0.0f) == kTopBottom);   // dx == 0
    EXPECT(Run(function, 150.0f, 90.0f, 1.0f) == kSide);        // |150 - 100| <= 100
    EXPECT(Run(function, 200.0f, 50.0f, 1.0f) == kSide);        // exactly cy away: side (a 16:9 split would say top)
    EXPECT(Run(function, 250.0f, 90.0f, 1.0f) == kTopBottom);   // 150 > 100 (a 16:9 split would say side)
    EXPECT(Run(function, -60.0f, 90.0f, 1.0f) == kTopBottom);   // |-60 - 100| = 160 > 100
    EXPECT(Run(function, 30.0f, 130.0f, 1.0f) == kSide);        // |30 - 100| = 70 <= 100

    split = 0;
    EXPECT(redirect.Restore());
    split = 1;
    EXPECT(Run(function, 150.0f, 90.0f, 1.0f) == 3);  // restored: falls through again
}

}  // namespace

int main()
{
    TestEdges();

    return lss::test::Report();
}
