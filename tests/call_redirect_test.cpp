// Host-side tests for platform::CallRedirect, run without the game.
//
// A small executable page stands in for the game's code: a function whose `call rel32` sits 7 bytes
// into its 16-byte block, as the overlay's call to EndUI__GetCompositeLayerRect does (0x140C95F57).
// The redirected call is EXECUTED: the replacement must run, see the game's return address, be able
// to call the original, and the original must come back after Restore.

#include "platform/call_redirect.h"
#include "platform/log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <intrin.h>

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

using IntFn = int (*)();

/// page+0x00: sub rsp, 28h ; nop x3 ; call target (at +0x07) ; add rsp, 28h ; ret
/// page+0x40: target: mov eax, 1 ; ret
class GameLikeCode
{
public:
    static constexpr std::size_t kCallOffset   = 0x07;
    static constexpr std::size_t kReturnOffset = 0x0C;
    static constexpr std::size_t kTargetOffset = 0x40;

    GameLikeCode()
    {
        memory_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        const std::uint8_t caller[] = {0x48, 0x83, 0xEC, 0x28,        // sub rsp, 28h
                                       0x90, 0x90, 0x90,              // nop x3
                                       0xE8, 0x00, 0x00, 0x00, 0x00,  // call target (displacement below)
                                       0x48, 0x83, 0xC4, 0x28,        // add rsp, 28h
                                       0xC3};                         // ret
        const std::uint8_t target[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};  // mov eax, 1 ; ret
        std::memcpy(memory_, caller, sizeof(caller));
        const std::int32_t rel = static_cast<std::int32_t>(kTargetOffset - (kCallOffset + 5));
        std::memcpy(memory_ + kCallOffset + 1, &rel, sizeof(rel));
        std::memcpy(memory_ + kTargetOffset, target, sizeof(target));
        DWORD previous = 0;
        VirtualProtect(memory_, 0x1000, PAGE_EXECUTE_READ, &previous);
        FlushInstructionCache(GetCurrentProcess(), memory_, 0x1000);
    }
    ~GameLikeCode() { VirtualFree(memory_, 0, MEM_RELEASE); }

    std::uintptr_t At(std::size_t offset) const { return reinterpret_cast<std::uintptr_t>(memory_) + offset; }
    int            Run() const { return reinterpret_cast<IntFn>(memory_)(); }

private:
    std::uint8_t* memory_ = nullptr;
};

lss::platform::CallRedirect* g_redirect      = nullptr;
std::uintptr_t               g_returnAddress = 0;

/// Records where it was called from, and adds 2 to what the original returns.
__declspec(noinline) int Replacement()
{
    g_returnAddress = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    return 2 + reinterpret_cast<IntFn>(g_redirect->Original())();
}

void TestRedirectRunsAndRestores()
{
    std::puts("call redirect: the replacement runs with the game's return address, calls the original, and is removed");
    const GameLikeCode game;
    EXPECT(game.Run() == 1);

    lss::platform::CallRedirect redirect;
    g_redirect = &redirect;
    EXPECT(redirect.Install(game.At(GameLikeCode::kCallOffset), game.At(GameLikeCode::kTargetOffset),
                            reinterpret_cast<std::uintptr_t>(&Replacement)));
    EXPECT(redirect.IsInstalled());
    EXPECT(redirect.Original() == game.At(GameLikeCode::kTargetOffset));
    EXPECT(game.Run() == 3);
    EXPECT(g_returnAddress == game.At(GameLikeCode::kReturnOffset));

    EXPECT(redirect.Restore());
    EXPECT(!redirect.IsInstalled());
    EXPECT(game.Run() == 1);
}

void TestRedirectRefusesWhatItDoesNotRecognise()
{
    std::puts("call redirect: refuses another target, a non-call, a site crossing its block, or a second install");
    const GameLikeCode game;
    const auto         replacement = reinterpret_cast<std::uintptr_t>(&Replacement);
    lss::platform::CallRedirect redirect;
    g_redirect = &redirect;

    EXPECT(!redirect.Install(game.At(GameLikeCode::kCallOffset), game.At(0x50), replacement));  // other target
    EXPECT(!redirect.Install(game.At(0x04), game.At(GameLikeCode::kTargetOffset), replacement));  // a nop, not a call
    EXPECT(!redirect.Install(game.At(0x0D), game.At(GameLikeCode::kTargetOffset), replacement));  // 0x0D..0x11 crosses 0x10
    EXPECT(!redirect.Install(game.At(GameLikeCode::kCallOffset), game.At(GameLikeCode::kTargetOffset), 0));
    EXPECT(game.Run() == 1);  // untouched

    EXPECT(redirect.Install(game.At(GameLikeCode::kCallOffset), game.At(GameLikeCode::kTargetOffset), replacement));
    EXPECT(!redirect.Install(game.At(GameLikeCode::kCallOffset), game.At(GameLikeCode::kTargetOffset), replacement));
    EXPECT(redirect.Restore());
    EXPECT(game.Run() == 1);
}

/// page+0x00: sub rsp, 28h ; nop x9 ; call target (at +0x0D, crossing into the block at +0x10) ;
/// add rsp, 28h ; ret.  page+0x40: target: mov eax, 1 ; ret. As the input dispatcher's extra-key calls
/// sit (0x140E7468D, 0x140E7477E).
class CrossingCode
{
public:
    static constexpr std::size_t kCallOffset   = 0x0D;
    static constexpr std::size_t kReturnOffset = 0x12;
    static constexpr std::size_t kTargetOffset = 0x40;

    CrossingCode()
    {
        memory_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        const std::uint8_t caller[] = {0x48, 0x83, 0xEC, 0x28,                                // sub rsp, 28h
                                       0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,  // nop x9
                                       0xE8, 0x00, 0x00, 0x00, 0x00,                          // call target
                                       0x48, 0x83, 0xC4, 0x28,                                // add rsp, 28h
                                       0xC3};                                                 // ret
        const std::uint8_t target[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};
        std::memcpy(memory_, caller, sizeof(caller));
        const std::int32_t rel = static_cast<std::int32_t>(kTargetOffset - (kCallOffset + 5));
        std::memcpy(memory_ + kCallOffset + 1, &rel, sizeof(rel));
        std::memcpy(memory_ + kTargetOffset, target, sizeof(target));
        DWORD previous = 0;
        VirtualProtect(memory_, 0x1000, PAGE_EXECUTE_READ, &previous);
        FlushInstructionCache(GetCurrentProcess(), memory_, 0x1000);
    }
    ~CrossingCode() { VirtualFree(memory_, 0, MEM_RELEASE); }

    std::uintptr_t At(std::size_t offset) const { return reinterpret_cast<std::uintptr_t>(memory_) + offset; }
    int            Run() const { return reinterpret_cast<IntFn>(memory_)(); }

private:
    std::uint8_t* memory_ = nullptr;
};

void TestCrossingCallFromOwnThread()
{
    std::puts("call redirect: a call crossing two blocks is redirected only from its own thread, runs, and is restored");
    const CrossingCode game;
    EXPECT(game.Run() == 1);
    const auto replacement = reinterpret_cast<std::uintptr_t>(&Replacement);

    lss::platform::CallRedirect refused;
    EXPECT(!refused.Install(game.At(CrossingCode::kCallOffset), game.At(CrossingCode::kTargetOffset), replacement));
    EXPECT(!refused.InstallFromOwnThread(game.At(CrossingCode::kCallOffset), game.At(0x50), replacement));  // other target
    EXPECT(game.Run() == 1);

    lss::platform::CallRedirect redirect;
    g_redirect = &redirect;
    EXPECT(redirect.InstallFromOwnThread(game.At(CrossingCode::kCallOffset), game.At(CrossingCode::kTargetOffset),
                                         replacement));
    EXPECT(redirect.IsInstalled());
    EXPECT(redirect.Original() == game.At(CrossingCode::kTargetOffset));
    EXPECT(game.Run() == 3);
    EXPECT(g_returnAddress == game.At(CrossingCode::kReturnOffset));

    EXPECT(redirect.Restore());
    EXPECT(!redirect.IsInstalled());
    EXPECT(game.Run() == 1);
}

/// page+0x00: caller: sub rsp, 28h ; call middle (at +0x04) ; add rsp, 28h ; ret
/// page+0x20: middle: sub rsp, 28h ; add rsp, 28h ; jmp target (at +0x28, a tail call)
/// page+0x40: target: mov eax, 1 ; ret
/// As EndUI__AreaMapIcon_EnableOrHideByCount tail-jumps to its events (0x143544CB0, 0x143544CB9).
class TailJumpCode
{
public:
    static constexpr std::size_t kCallOffset   = 0x04;
    static constexpr std::size_t kReturnOffset = 0x09;  // where the caller's call returns
    static constexpr std::size_t kMiddleOffset = 0x20;
    static constexpr std::size_t kJumpOffset   = 0x28;
    static constexpr std::size_t kTargetOffset = 0x40;

    TailJumpCode()
    {
        memory_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        const std::uint8_t caller[] = {0x48, 0x83, 0xEC, 0x28, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x28, 0xC3};
        const std::uint8_t middle[] = {0x48, 0x83, 0xEC, 0x28, 0x48, 0x83, 0xC4, 0x28, 0xE9, 0x00, 0x00, 0x00, 0x00};
        const std::uint8_t target[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};
        std::memcpy(memory_, caller, sizeof(caller));
        std::memcpy(memory_ + kMiddleOffset, middle, sizeof(middle));
        std::memcpy(memory_ + kTargetOffset, target, sizeof(target));
        const std::int32_t toMiddle = static_cast<std::int32_t>(kMiddleOffset - (kCallOffset + 5));
        const std::int32_t toTarget = static_cast<std::int32_t>(kTargetOffset - (kJumpOffset + 5));
        std::memcpy(memory_ + kCallOffset + 1, &toMiddle, sizeof(toMiddle));
        std::memcpy(memory_ + kJumpOffset + 1, &toTarget, sizeof(toTarget));
        DWORD previous = 0;
        VirtualProtect(memory_, 0x1000, PAGE_EXECUTE_READ, &previous);
        FlushInstructionCache(GetCurrentProcess(), memory_, 0x1000);
    }
    ~TailJumpCode() { VirtualFree(memory_, 0, MEM_RELEASE); }

    std::uintptr_t At(std::size_t offset) const { return reinterpret_cast<std::uintptr_t>(memory_) + offset; }
    int            Run() const { return reinterpret_cast<IntFn>(memory_)(); }

private:
    std::uint8_t* memory_ = nullptr;
};

void TestTailJump()
{
    std::puts("call redirect: a tail jump's replacement returns to the caller's caller, calls the original, and is removed");
    const TailJumpCode game;
    EXPECT(game.Run() == 1);
    const auto replacement = reinterpret_cast<std::uintptr_t>(&Replacement);

    lss::platform::CallRedirect refused;
    EXPECT(!refused.Install(game.At(TailJumpCode::kJumpOffset), game.At(TailJumpCode::kTargetOffset), replacement));
    EXPECT(!refused.InstallTailJump(game.At(TailJumpCode::kCallOffset), game.At(TailJumpCode::kMiddleOffset), replacement));
    EXPECT(game.Run() == 1);

    lss::platform::CallRedirect redirect;
    g_redirect = &redirect;
    EXPECT(redirect.InstallTailJump(game.At(TailJumpCode::kJumpOffset), game.At(TailJumpCode::kTargetOffset), replacement));
    EXPECT(game.Run() == 3);
    EXPECT(g_returnAddress == game.At(TailJumpCode::kReturnOffset));

    EXPECT(redirect.Restore());
    EXPECT(game.Run() == 1);
}
}  // namespace

int main()
{
    TestRedirectRunsAndRestores();
    TestRedirectRefusesWhatItDoesNotRecognise();
    TestCrossingCallFromOwnThread();
    TestTailJump();

    std::printf(g_failures == 0 ? "all tests passed\n" : "%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
