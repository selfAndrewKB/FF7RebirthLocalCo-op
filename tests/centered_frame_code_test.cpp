// Host-side tests for the centred-frame code, run without the game.
//
// The bytes the mod writes near the game (hud/layout/centered_frame_code.h) are EXECUTED here, installed
// through platform::CodeRedirect over a stand-in that ends like EndUI__GetCentered1920x1080Frame:
// rbx = out, xmm4/xmm5 = the layout size, Min and Max.Y already stored, and the last store
// `vmovss [rbx+8], xmm0` 4 bytes into a 16-byte block (0x140F5FC54 in the game).

#include "hud/layout/centered_frame_code.h"
#include "platform/code_redirect.h"

#include "exec_page.h"
#include "expect.h"

#include <cstdio>
#include <cstring>

namespace {

struct Box
{
    float minX, minY, maxX, maxY;
};

/// push rbx; mov rbx, rcx; xmm0 = 3 (the game's Max.X), xmm4 = 10, xmm5 = 20 (the layout size);
/// Min = (1, 2), Max.Y = 4; nop; vmovss [rbx+8], xmm0 (offset 52); mov rax, rbx; pop rbx; ret.
class FunctionLike
{
public:
    static constexpr std::size_t kSiteOffset   = 52;
    static constexpr std::size_t kResumeOffset = 57;

    FunctionLike()
    {
        const std::uint8_t code[] = {
            0x53,                                      //  0: push rbx
            0x48, 0x89, 0xCB,                          //  1: mov rbx, rcx
            0xB8, 0x00, 0x00, 0x40, 0x40,              //  4: mov eax, 3.0f
            0xC5, 0xF9, 0x6E, 0xC0,                    //  9: vmovd xmm0, eax
            0xB8, 0x00, 0x00, 0x20, 0x41,              // 13: mov eax, 10.0f
            0xC5, 0xF9, 0x6E, 0xE0,                    // 18: vmovd xmm4, eax
            0xB8, 0x00, 0x00, 0xA0, 0x41,              // 22: mov eax, 20.0f
            0xC5, 0xF9, 0x6E, 0xE8,                    // 27: vmovd xmm5, eax
            0xC7, 0x03, 0x00, 0x00, 0x80, 0x3F,        // 31: mov dword [rbx], 1.0f
            0xC7, 0x43, 0x04, 0x00, 0x00, 0x00, 0x40,  // 37: mov dword [rbx+4], 2.0f
            0xC7, 0x43, 0x0C, 0x00, 0x00, 0x80, 0x40,  // 44: mov dword [rbx+0Ch], 4.0f
            0x90,                                      // 51: nop
            0xC5, 0xFA, 0x11, 0x43, 0x08,              // 52: vmovss [rbx+8], xmm0
            0x48, 0x89, 0xD8,                          // 57: mov rax, rbx
            0x5B,                                      // 60: pop rbx
            0xC3};                                     // 61: ret
        page_.Write(0, code);
        page_.Seal();
    }

    std::uintptr_t At(std::size_t offset) const { return page_.At(offset); }
    Box            Call() const
    {
        Box box{-1, -1, -1, -1};
        const auto returned = page_.Function<Box* (*)(Box*)>()(&box);
        EXPECT(returned == &box);
        return box;
    }

private:
    lss::test::ExecutablePage page_;
};

bool Is(const Box& box, float minX, float minY, float maxX, float maxY)
{
    return box.minX == minX && box.minY == minY && box.maxX == maxX && box.maxY == maxY;
}

void TestFrame()
{
    std::puts("centred frame: the game's box while the flag is clear, the whole layout while set, and restored");
    const FunctionLike function;
    EXPECT(Is(function.Call(), 1, 2, 3, 4));

    alignas(8) static std::uint8_t wholeLayout = 0;
    const auto code = lss::hud::CenteredFrameCode(wholeLayout);
    lss::platform::CodeRedirect redirect;
    EXPECT(redirect.Install(function.At(FunctionLike::kSiteOffset), lss::hud::kCenteredFrameStoreBytes, code,
                            function.At(FunctionLike::kResumeOffset)));
    EXPECT(Is(function.Call(), 1, 2, 3, 4));   // flag clear: the game's box
    wholeLayout = 1;
    EXPECT(Is(function.Call(), 0, 0, 10, 20));  // split half: the whole layout
    wholeLayout = 0;
    EXPECT(Is(function.Call(), 1, 2, 3, 4));
    EXPECT(redirect.Restore());
    wholeLayout = 1;
    EXPECT(Is(function.Call(), 1, 2, 3, 4));   // restored: the flag no longer matters
}

}  // namespace

int main()
{
    TestFrame();

    return lss::test::Report();
}
