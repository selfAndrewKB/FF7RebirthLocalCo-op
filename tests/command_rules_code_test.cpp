// Host-side tests for the command-menu gate (player_two/command_rules_code.h), run without the game.
//
// The bytes the mod writes near the game are EXECUTED here, installed through platform::CodeRedirect
// over a stand-in laid out like FieldCommandCtrl_IsMemberCommandable's entry: `mov [rsp+8], rbx` at
// 4 bytes into a 16-byte block (0x140E77414 in the game), followed by a body that returns ordinal + 1.

#include "platform/code_redirect.h"
#include "player_two/command_rules_code.h"

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

}  // namespace

int main()
{
    TestGate();

    return lss::test::Report();
}
