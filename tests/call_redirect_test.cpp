// Host-side tests for platform::CallRedirect, run without the game.
//
// A small executable page stands in for the game's code: a function whose `call rel32` sits 7 bytes
// into its 16-byte block, as the overlay's call to EndUI__GetCompositeLayerRect does.
// The redirected call is EXECUTED: the replacement must run, see the game's return address, be able
// to call the original, and the original must come back after Restore.

#include "platform/call_redirect.h"
#include "platform/image.h"
#include "platform/log.h"

#include "exec_page.h"
#include "expect.h"

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
        const std::uint8_t caller[] = {0x48, 0x83, 0xEC, 0x28,        // sub rsp, 28h
                                       0x90, 0x90, 0x90,              // nop x3
                                       0xE8, 0x00, 0x00, 0x00, 0x00,  // call target (displacement below)
                                       0x48, 0x83, 0xC4, 0x28,        // add rsp, 28h
                                       0xC3};                         // ret
        const std::uint8_t target[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};  // mov eax, 1 ; ret
        page_.Write(0, caller);
        page_.WriteRel32(kCallOffset, kTargetOffset);
        page_.Write(kTargetOffset, target);
        page_.Seal();
    }

    std::uintptr_t At(std::size_t offset) const { return page_.At(offset); }
    int            Run() const { return page_.Function<IntFn>()(); }

private:
    lss::test::ExecutablePage page_;
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
/// sit.
class CrossingCode
{
public:
    static constexpr std::size_t kCallOffset   = 0x0D;
    static constexpr std::size_t kReturnOffset = 0x12;
    static constexpr std::size_t kTargetOffset = 0x40;

    CrossingCode()
    {
        const std::uint8_t caller[] = {0x48, 0x83, 0xEC, 0x28,                                // sub rsp, 28h
                                       0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,  // nop x9
                                       0xE8, 0x00, 0x00, 0x00, 0x00,                          // call target
                                       0x48, 0x83, 0xC4, 0x28,                                // add rsp, 28h
                                       0xC3};                                                 // ret
        const std::uint8_t target[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};
        page_.Write(0, caller);
        page_.WriteRel32(kCallOffset, kTargetOffset);
        page_.Write(kTargetOffset, target);
        page_.Seal();
    }

    std::uintptr_t At(std::size_t offset) const { return page_.At(offset); }
    int            Run() const { return page_.Function<IntFn>()(); }

private:
    lss::test::ExecutablePage page_;
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
/// As EndUI__AreaMapIcon_EnableOrHideByCount tail-jumps to its events.
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
        const std::uint8_t caller[] = {0x48, 0x83, 0xEC, 0x28, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x28, 0xC3};
        const std::uint8_t middle[] = {0x48, 0x83, 0xEC, 0x28, 0x48, 0x83, 0xC4, 0x28, 0xE9, 0x00, 0x00, 0x00, 0x00};
        const std::uint8_t target[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};
        page_.Write(0, caller);
        page_.Write(kMiddleOffset, middle);
        page_.Write(kTargetOffset, target);
        page_.WriteRel32(kCallOffset, kMiddleOffset);
        page_.WriteRel32(kJumpOffset, kTargetOffset);
        page_.Seal();
    }

    std::uintptr_t At(std::size_t offset) const { return page_.At(offset); }
    int            Run() const { return page_.Function<IntFn>()(); }

private:
    lss::test::ExecutablePage page_;
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
void TestInstallThroughTheImage()
{
    std::puts("call redirect: a site given as a source address is rebased through the image, the target live or "
              "a source address");
    const GameLikeCode game;
    constexpr std::uintptr_t kIdaBase = 0x140000000;
    const auto image = lss::platform::GameImage::At(game.At(0), 0x1000);  // the page as the image
    const auto replacement = reinterpret_cast<std::uintptr_t>(&Replacement);

    lss::platform::CallRedirect redirect;
    g_redirect = &redirect;
    EXPECT(!redirect.Install(image, kIdaBase + 0x2000, game.At(GameLikeCode::kTargetOffset), replacement));  // outside
    EXPECT(!redirect.InstallSource(image, kIdaBase + GameLikeCode::kCallOffset, kIdaBase + 0x2000, replacement));
    EXPECT(game.Run() == 1);
    EXPECT(redirect.Install(image, kIdaBase + GameLikeCode::kCallOffset, game.At(GameLikeCode::kTargetOffset),
                            replacement));
    EXPECT(game.Run() == 3);
    EXPECT(redirect.Restore());
    EXPECT(redirect.InstallSource(image, kIdaBase + GameLikeCode::kCallOffset, kIdaBase + GameLikeCode::kTargetOffset,
                                    replacement));
    EXPECT(game.Run() == 3);
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
    TestInstallThroughTheImage();

    return lss::test::Report();
}
