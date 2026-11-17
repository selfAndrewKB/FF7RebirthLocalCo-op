// Host-side tests for the target-panel gate (battle/panel_gate_code.h), run without the game.
//
// The bytes the mod writes near the game are EXECUTED here, installed through platform::CodeRedirect
// over a stand-in laid out like the panel request's cold chunk: a frame of 0x118 bytes, the replaced
// `mov [rsp+0x108], rax` at 5 bytes into a 16-byte block (as at the game's site), then the game's
// path (returns what the replaced instruction stored, the request's stand-in) and, apart, the path the
// gate skips to (returns 2).

#include "battle/panel_gate_code.h"
#include "platform/code_redirect.h"

#include "exec_page.h"
#include "expect.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

/// sub rsp, 0x118 ; mov rax, rcx ; nops to offset 0x15 ; mov [rsp+0x108], rax (the site) ;
/// mov rax, [rsp+0x108] ; add rsp, 0x118 ; ret (the game's path: returns the stored argument)
/// and at kSkipOffset: mov eax, 2 ; add rsp, 0x118 ; ret (where the gate goes on).
class ChunkLike
{
public:
    static constexpr std::size_t kSiteOffset   = 0x15;
    static constexpr std::size_t kResumeOffset = 0x1D;
    static constexpr std::size_t kSkipOffset   = 0x40;

    ChunkLike()
    {
        std::uint8_t code[0x60];
        std::memset(code, 0x90, sizeof(code));
        const std::uint8_t entry[] = {0x48, 0x81, 0xEC, 0x18, 0x01, 0x00, 0x00,  // 0x00: sub rsp, 0x118
                                      0x48, 0x89, 0xC8};                         // 0x07: mov rax, rcx
        const std::uint8_t site[]  = {0x48, 0x89, 0x84, 0x24, 0x08, 0x01, 0x00, 0x00,  // 0x15: mov [rsp+0x108], rax
                                      0x48, 0x8B, 0x84, 0x24, 0x08, 0x01, 0x00, 0x00,  // 0x1D: mov rax, [rsp+0x108]
                                      0x48, 0x81, 0xC4, 0x18, 0x01, 0x00, 0x00,        // 0x25: add rsp, 0x118
                                      0xC3};                                           // 0x2C: ret
        const std::uint8_t skip[]  = {0xB8, 0x02, 0x00, 0x00, 0x00,                    // 0x40: mov eax, 2
                                      0x48, 0x81, 0xC4, 0x18, 0x01, 0x00, 0x00,        // 0x45: add rsp, 0x118
                                      0xC3};                                           // 0x4C: ret
        std::memcpy(code, entry, sizeof(entry));
        std::memcpy(code + kSiteOffset, site, sizeof(site));
        std::memcpy(code + kSkipOffset, skip, sizeof(skip));
        page_.Write(0, code);
        page_.Seal();
    }

    std::uintptr_t At(std::size_t offset) const { return page_.At(offset); }
    std::uint64_t  Call(std::uint64_t value) const
    {
        using Fn = std::uint64_t (*)(std::uint64_t value);
        return reinterpret_cast<Fn>(At(0))(value);
    }

private:
    lss::test::ExecutablePage page_;
};

void TestGate()
{
    std::puts("panel gate: open, the chunk runs as the game's (the replaced store included); set, it skips and "
              "counts; restored");
    const ChunkLike chunk;
    static lss::battle::PanelGate gate;
    EXPECT(chunk.Call(0x1234) == 0x1234);

    const auto code = lss::battle::PanelGateCode(gate, chunk.At(ChunkLike::kSkipOffset));
    lss::platform::CodeRedirect redirect;
    EXPECT(redirect.Install(chunk.At(ChunkLike::kSiteOffset), lss::battle::kPanelGateSiteBytes1004, code,
                            chunk.At(ChunkLike::kResumeOffset)));

    EXPECT(chunk.Call(0x5678) == 0x5678);  // open: the replaced store ran, the game's path went on
    EXPECT(gate.skipped == 0);
    gate.skip = 1;
    EXPECT(chunk.Call(0x5678) == 2);  // set: skipped
    EXPECT(chunk.Call(0x9ABC) == 2);
    EXPECT(gate.skipped == 2);
    gate.skip = 0;
    EXPECT(chunk.Call(0x9ABC) == 0x9ABC);
    EXPECT(gate.skipped == 2);

    gate.skip = 1;
    EXPECT(redirect.Restore());
    EXPECT(chunk.Call(0x1111) == 0x1111);  // restored: the gate no longer applies
}

}  // namespace

int main()
{
    TestGate();

    return lss::test::Report();
}
