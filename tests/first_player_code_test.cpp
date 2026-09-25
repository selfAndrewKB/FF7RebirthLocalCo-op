// Host-side tests for the first-player answer (lever L2), run without the game.
//
// The bytes the mod writes near the game (hud/first_player_code.h) are EXECUTED here, installed
// through platform::CodeRedirect over a stand-in for the function's entry: `mov eax, 28h` followed
// by the rest of a function that returns a marker value (as the game's returns its first player).

#include "hud/first_player_code.h"
#include "platform/code_redirect.h"

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

/// A stand-in for the function, with `mov eax, 28h` at the same offset in its 16-byte block as the
/// game's (0x140CCF8E4 is 4 bytes into the block at 0x140CCF8E0):
///     nop x4 ; mov eax, 28h ; mov rax, 5A5Ah (the game's own answer) ; ret
class FunctionLike
{
public:
    static constexpr std::size_t kEntryOffset  = 4;
    static constexpr std::size_t kResumeOffset = kEntryOffset + 5;
    static constexpr std::uint64_t kGameAnswer = 0x5A5A;

    FunctionLike()
    {
        const std::uint8_t code[] = {
            0x90, 0x90, 0x90, 0x90,                                // nop x4
            0xB8, 0x28, 0x00, 0x00, 0x00,                          // mov eax, 28h   (offset 4)
            0x48, 0xB8, 0x5A, 0x5A, 0, 0, 0, 0, 0, 0,              // mov rax, 5A5Ah
            0xC3};                                                 // ret
        memory_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        std::memcpy(memory_, code, sizeof(code));
        DWORD previous = 0;
        VirtualProtect(memory_, 0x1000, PAGE_EXECUTE_READ, &previous);
        FlushInstructionCache(GetCurrentProcess(), memory_, 0x1000);
    }
    ~FunctionLike() { VirtualFree(memory_, 0, MEM_RELEASE); }

    std::uintptr_t At(std::size_t offset) const { return reinterpret_cast<std::uintptr_t>(memory_) + offset; }
    std::uint64_t Call() const { return reinterpret_cast<std::uint64_t (*)()>(At(kEntryOffset))(); }

private:
    std::uint8_t* memory_ = nullptr;
};

void TestAnswer()
{
    std::puts("first player: the field's player while set, the game's own answer otherwise, and restored");
    const FunctionLike function;
    EXPECT(function.Call() == FunctionLike::kGameAnswer);

    alignas(8) static std::uint64_t answer = 0;
    const auto code = lss::hud::FirstPlayerCode(answer);
    lss::platform::CodeRedirect redirect;
    EXPECT(redirect.Install(function.At(FunctionLike::kEntryOffset), lss::hud::kFirstPlayerEntryBytes, code,
                            function.At(FunctionLike::kResumeOffset)));
    EXPECT(function.Call() == FunctionLike::kGameAnswer);  // field 0: the game's answer
    answer = 0x1234567890;
    EXPECT(function.Call() == 0x1234567890);               // player 2's pass
    answer = 0;
    EXPECT(function.Call() == FunctionLike::kGameAnswer);
    EXPECT(redirect.Restore());
    answer = 0x77;
    EXPECT(function.Call() == FunctionLike::kGameAnswer);  // restored: the field no longer matters
}

}  // namespace

int main()
{
    TestAnswer();

    std::printf(g_failures == 0 ? "all tests passed\n" : "%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
