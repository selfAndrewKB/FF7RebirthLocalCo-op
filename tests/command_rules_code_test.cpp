// Host-side tests for the command-menu gate (player_two/command_rules_code.h), run without the game.
//
// The bytes the mod writes near the game are EXECUTED here, installed through platform::CodeRedirect
// over a stand-in laid out like FieldCommandCtrl_IsMemberCommandable's entry: `mov [rsp+8], rbx` at
// 4 bytes into a 16-byte block (0x140E77414 in the game), followed by a body that returns ordinal + 1.

#include "platform/code_redirect.h"
#include "player_two/command_rules_code.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;

#define EXPECT(condition)                                                                \
    do                                                                                   \
    {                                                                                    \
        if (!(condition))                                                                \
        {                                                                                \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);                 \
            ++g_failures;                                                                \
        }                                                                                \
    } while (false)

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
        memory_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        std::memcpy(memory_, code, sizeof(code));
        DWORD previous = 0;
        VirtualProtect(memory_, 0x1000, PAGE_EXECUTE_READ, &previous);
        FlushInstructionCache(GetCurrentProcess(), memory_, 0x1000);
    }
    ~FunctionLike() { VirtualFree(memory_, 0, MEM_RELEASE); }

    std::uintptr_t At(std::size_t offset) const { return reinterpret_cast<std::uintptr_t>(memory_) + offset; }
    std::uint32_t  Call(std::int32_t ordinal) const
    {
        using Fn = std::uint32_t (*)(void* ctrl, std::int32_t ordinal, std::uint8_t flag);
        return reinterpret_cast<Fn>(At(kEntryOffset))(nullptr, ordinal, 1);
    }

private:
    std::uint8_t* memory_ = nullptr;
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

    std::printf(g_failures == 0 ? "all tests passed\n" : "%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
