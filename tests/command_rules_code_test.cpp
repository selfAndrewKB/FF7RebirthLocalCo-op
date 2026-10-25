// Host-side tests for the command-menu gate (player_two/battle/command_rules_code.h), run without the game.
//
// The bytes the mod writes near the game are EXECUTED here, installed through platform::CodeRedirect
// over a stand-in laid out like FieldCommandCtrl_IsMemberCommandable's entry: `mov [rsp+8], rbx` at
// 4 bytes into a 16-byte block (0x140E77414 in the game), followed by a body that returns ordinal + 1.

#include "platform/code_redirect.h"
#include "player_two/battle/command_rules_code.h"

#include "exec_page.h"
#include "expect.h"

#include <cstdio>
#include <cstring>

namespace {

/// nop x4 ; mov [rsp+8], rbx (offset 4) ; lea eax, [rdx+1] ; ret
/// It returns ordinal + 1: the replaced instruction ran and the ordinal register did not change.
class FunctionLike
{
public:
    static constexpr std::size_t kEntryOffset  = 4;
    static constexpr std::size_t kResumeOffset = 9;

    FunctionLike()
    {
        std::uint8_t code[32] = {};
        std::memset(code, 0x90, kEntryOffset);
        const std::uint8_t body[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08,  //  4: mov [rsp+8], rbx
            0x8D, 0x42, 0x01,              //  9: lea eax, [rdx+1]
            0xC3};                         // 12: ret
        std::memcpy(code + kEntryOffset, body, sizeof(body));
        page_.Write(0, code);
        page_.Seal();
    }

    std::uintptr_t At(std::size_t offset) const { return page_.At(offset); }
    std::uint32_t  Call(std::int32_t ordinal) const
    {
        using Fn = std::uint32_t (*)(void* ctrl, std::int32_t ordinal, std::uint8_t flag);
        return reinterpret_cast<Fn>(At(kEntryOffset))(nullptr, ordinal, 1);
    }

private:
    lss::test::ExecutablePage page_;
};

void TestGate()
{
    std::puts("command gate: the gated ordinal reads not commandable, every other passes through, -1 gates none, "
              "and restored");
    const FunctionLike function;
    static std::int32_t gate = -1;
    EXPECT(function.Call(2) == 3);

    const auto code = lss::player_two::CommandGateCode(gate);
    lss::platform::CodeRedirect redirect;
    EXPECT(redirect.Install(function.At(FunctionLike::kEntryOffset), lss::player_two::kCommandGateEntryBytes, code,
                            function.At(FunctionLike::kResumeOffset)));

    EXPECT(function.Call(0) == 1);  // gate -1: none refused
    EXPECT(function.Call(2) == 3);
    gate = 2;
    EXPECT(function.Call(2) == 0);  // the gated member
    EXPECT(function.Call(1) == 2);
    EXPECT(function.Call(0) == 1);
    gate = 0;
    EXPECT(function.Call(0) == 0);
    EXPECT(function.Call(2) == 3);
    gate = -1;
    EXPECT(function.Call(0) == 1);

    gate = 2;
    EXPECT(redirect.Restore());
    EXPECT(function.Call(2) == 3);  // restored: the gate no longer applies
}

void TestMaskGate()
{
    std::puts("command gate over a set (installed since step 6): every masked ordinal reads not commandable, the "
              "others pass through, 0 gates none, the one-ordinal gate's answers for one member, restored");
    const FunctionLike   function;
    static std::uint32_t mask = 0;
    static std::int32_t  gate = -1;  // the one-ordinal gate, for the side-by-side
    const auto code = lss::player_two::CommandGateMaskCode(mask);
    EXPECT(code.size() == lss::player_two::kCommandGateCodeSize);
    lss::platform::CodeRedirect redirect;
    EXPECT(redirect.Install(function.At(FunctionLike::kEntryOffset), lss::player_two::kCommandGateEntryBytes, code,
                            function.At(FunctionLike::kResumeOffset)));

    for (std::int32_t ordinal = 0; ordinal < 8; ++ordinal)
    {
        EXPECT(function.Call(ordinal) == static_cast<std::uint32_t>(ordinal + 1));  // mask 0: none refused
    }
    mask = (1u << 1) | (1u << 4);  // player 1 refusing two seats' members
    for (std::int32_t ordinal = 0; ordinal < 8; ++ordinal)
    {
        const bool refused = ordinal == 1 || ordinal == 4;
        EXPECT(function.Call(ordinal) == (refused ? 0u : static_cast<std::uint32_t>(ordinal + 1)));
    }
    EXPECT(function.Call(-2) == 0xFFFFFFFFu);  // -2 reads bit 30, never set: it passes through (-2 + 1)

    // One refused member: the set gives the one-ordinal gate's answer for every ordinal 0..31 (the bytes'
    // equivalence where both are defined; outside 0..31 the set reads the remainder's bit, see the header).
    const FunctionLike          single;
    lss::platform::CodeRedirect singleRedirect;
    EXPECT(singleRedirect.Install(single.At(FunctionLike::kEntryOffset), lss::player_two::kCommandGateEntryBytes,
                                  lss::player_two::CommandGateCode(gate), single.At(FunctionLike::kResumeOffset)));
    for (std::int32_t member = -1; member < 8; ++member)
    {
        gate = member;
        mask = member >= 0 ? 1u << member : 0u;
        for (std::int32_t ordinal = 0; ordinal < 32; ++ordinal)
        {
            EXPECT(function.Call(ordinal) == single.Call(ordinal));
        }
    }
    gate = -1;
    EXPECT(singleRedirect.Restore());

    mask = 1u << 2;
    EXPECT(redirect.Restore());
    EXPECT(function.Call(2) == 3);  // restored: the gate no longer applies
}

}  // namespace

int main()
{
    TestGate();
    TestMaskGate();

    return lss::test::Report();
}
