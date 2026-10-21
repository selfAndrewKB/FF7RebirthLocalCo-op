// Host-side tests for the restored load-action rule, run without the game.
//
// The bytes the mod writes near the game (renderer/output_load_action_code.h) are EXECUTED here, in
// a small harness that recreates their surroundings in the game: the game's own `setz al; add al,1`
// on the comparison of Views[0] with the view, rcx pointing at the family, and a frame deep enough
// for the store to [rsp+610h]. Every combination of "first in its family" and "additional family"
// is checked, together with the jump encoding the redirect relies on.

#include "facts/engine.h"
#include "platform/code_redirect.h"
#include "platform/x64_jump.h"
#include "renderer/output_load_action_code.h"

#include "exec_page.h"
#include "expect.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using lss::platform::EncodeJump;
using lss::renderer::kFirstUnlessAdditionalLoadAction;
using lss::renderer::kLoadActionClear;
using lss::renderer::kLoadActionLoad;

/// uint8_t harness(const uint8_t* family, uintptr_t viewsFirst, uintptr_t view):
///     sub rsp, 618h ; cmp rdx, r8 ; setz al ; add al, 1 (the game's computation) ;
///     <the mod's bytes, ending with the store to [rsp+610h]> ; jmp next ;
///     next: movzx eax, byte [rsp+610h] ; add rsp, 618h ; ret
using Harness = std::uint8_t (*)(const std::uint8_t* family, std::uintptr_t viewsFirst, std::uintptr_t view);

class ExecutableHarness
{
public:
    ExecutableHarness()
    {
        const std::uint8_t prologue[] = {0x48, 0x81, 0xEC, 0x18, 0x06, 0x00, 0x00,  // sub rsp, 618h
                                         0x4C, 0x39, 0xC2,                          // cmp rdx, r8
                                         0x0F, 0x94, 0xC0,                          // setz al
                                         0x04, 0x01};                               // add al, 1
        const std::uint8_t epilogue[] = {0x0F, 0xB6, 0x84, 0x24, 0x10, 0x06, 0x00, 0x00,  // movzx eax, [rsp+610h]
                                         0x48, 0x81, 0xC4, 0x18, 0x06, 0x00, 0x00,        // add rsp, 618h
                                         0xC3};                                           // ret
        std::vector<std::uint8_t> code(prologue, prologue + sizeof(prologue));
        code.insert(code.end(), std::begin(kFirstUnlessAdditionalLoadAction), std::end(kFirstUnlessAdditionalLoadAction));

        // The jump back, placed where the redirect appends it, to the instruction after the store.
        const auto from = page_.At() + code.size();
        const auto back = EncodeJump(from, from + lss::platform::kJumpLength);
        code.insert(code.end(), back->begin(), back->end());
        code.insert(code.end(), epilogue, epilogue + sizeof(epilogue));

        page_.Write(0, code);
        page_.Seal();
    }

    std::uint8_t Run(bool firstInFamily, bool additionalFamily) const
    {
        std::uint8_t family[0x80]{};
        family[0x74] = additionalFamily ? 1 : 0;
        const std::uintptr_t view = 0x1000;
        return page_.Function<Harness>()(family, firstInFamily ? view : view + 8, view);
    }

private:
    lss::test::ExecutablePage page_;
};

void TestTheStockRule()
{
    std::puts("load action: clear only for the first view of a main family");
    const ExecutableHarness harness;
    EXPECT(harness.Run(true, false) == kLoadActionClear);   // one player, or player 1's family
    EXPECT(harness.Run(true, true) == kLoadActionLoad);     // player 2's family keeps player 1's half
    EXPECT(harness.Run(false, false) == kLoadActionLoad);   // a later view of a multi-view family
    EXPECT(harness.Run(false, true) == kLoadActionLoad);
}

/// A stand-in for the game's code around the site, with the same instructions and the store at the
/// same offset in its 16-byte block (0x14111A377 is 7 bytes into the block at 0x14111A370):
///     nop x8 ; sub rsp, 618h ; cmp rdx, r8 ; setz al ; add al, 1 ;
///     mov [rsp+610h], al   <- the redirected instruction, at page+0x17
///     movzx eax, byte [rsp+610h] ; add rsp, 618h ; ret
class GameLikeCode
{
public:
    static constexpr std::size_t kStoreOffset  = 0x17;
    static constexpr std::size_t kResumeOffset = kStoreOffset + 7;
    static constexpr std::uint8_t kStore[]     = {0x88, 0x84, 0x24, 0x10, 0x06, 0x00, 0x00};

    GameLikeCode()
    {
        const std::uint8_t code[] = {
            0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,  // nop x8
            0x48, 0x81, 0xEC, 0x18, 0x06, 0x00, 0x00,        // sub rsp, 618h
            0x4C, 0x39, 0xC2,                                // cmp rdx, r8
            0x0F, 0x94, 0xC0,                                // setz al
            0x04, 0x01,                                      // add al, 1
            0x88, 0x84, 0x24, 0x10, 0x06, 0x00, 0x00,        // mov [rsp+610h], al   (offset 0x17)
            0x0F, 0xB6, 0x84, 0x24, 0x10, 0x06, 0x00, 0x00,  // movzx eax, [rsp+610h]
            0x48, 0x81, 0xC4, 0x18, 0x06, 0x00, 0x00,        // add rsp, 618h
            0xC3};                                           // ret
        page_.Write(0, code);
        page_.Seal();
    }

    std::uintptr_t At(std::size_t offset) const { return page_.At(offset); }

    std::uint8_t Run(bool firstInFamily, bool additionalFamily) const
    {
        std::uint8_t family[0x80]{};
        family[0x74] = additionalFamily ? 1 : 0;
        const std::uintptr_t view = 0x1000;
        return page_.Function<Harness>()(family, firstInFamily ? view : view + 8, view);
    }

private:
    lss::test::ExecutablePage page_;
};

void TestRedirectInstallsRunsAndRestores()
{
    std::puts("redirect: replaces the store, applies the rule, and restores the game's behaviour");
    const GameLikeCode game;
    EXPECT(game.Run(true, true) == kLoadActionClear);  // the shipped behaviour: ignores the flag

    {
        lss::platform::CodeRedirect redirect;
        EXPECT(redirect.Install(game.At(GameLikeCode::kStoreOffset), GameLikeCode::kStore,
                                kFirstUnlessAdditionalLoadAction, game.At(GameLikeCode::kResumeOffset)));
        EXPECT(game.Run(true, false) == kLoadActionClear);
        EXPECT(game.Run(true, true) == kLoadActionLoad);
        EXPECT(game.Run(false, false) == kLoadActionLoad);
        EXPECT(game.Run(false, true) == kLoadActionLoad);

        EXPECT(redirect.Restore());
        EXPECT(!redirect.IsInstalled());
    }
    EXPECT(game.Run(true, true) == kLoadActionClear);  // back to the game's own instructions
}

void TestRedirectRefusesWhatItDoesNotRecognise()
{
    std::puts("redirect: refuses different bytes, a site crossing its block, or a second install");
    const GameLikeCode game;
    const std::uint8_t other[] = {0x88, 0x84, 0x24, 0x10, 0x06, 0x00, 0x01};
    lss::platform::CodeRedirect redirect;
    EXPECT(!redirect.Install(game.At(GameLikeCode::kStoreOffset), other, kFirstUnlessAdditionalLoadAction,
                             game.At(GameLikeCode::kResumeOffset)));
    EXPECT(!redirect.Install(game.At(0x0C), GameLikeCode::kStore, kFirstUnlessAdditionalLoadAction,
                             game.At(GameLikeCode::kResumeOffset)));  // 0x0C..0x12 crosses 0x10
    EXPECT(game.Run(true, true) == kLoadActionClear);                 // untouched

    EXPECT(redirect.Install(game.At(GameLikeCode::kStoreOffset), GameLikeCode::kStore,
                            kFirstUnlessAdditionalLoadAction, game.At(GameLikeCode::kResumeOffset)));
    EXPECT(!redirect.Install(game.At(GameLikeCode::kStoreOffset), GameLikeCode::kStore,
                             kFirstUnlessAdditionalLoadAction, game.At(GameLikeCode::kResumeOffset)));
    EXPECT(redirect.Restore());
}

/// Game version 1.005's surroundings (RE-1005-core-facts.md): an rbp frame, r14 = the view whose first field is the
/// family, rcx live across the store (the game passes it to a call right after), the store `mov [rbp+0E0h], al` one
/// byte into its 16-byte block (0x14118C6B1 in the block at 0x14118C6B0):
///     push rbp ; push r14 ; sub rsp, 108h ; mov rbp, rsp ; mov r14, r9 ; cmp rdx, r8 ; setz al ; add al, 1 ; nop x9 ;
///     mov [rbp+0E0h], al   <- the redirected instruction, at page+0x21
///     mov [r14+8], rcx     (hands rcx back, to prove the redirect left it alone)
///     movzx eax, byte [rbp+0E0h] ; add rsp, 108h ; pop r14 ; pop rbp ; ret
class GameLikeCode1005
{
public:
    static constexpr std::size_t kStoreOffset  = 0x21;
    static constexpr std::size_t kResumeOffset = kStoreOffset + 6;

    struct View
    {
        const std::uint8_t* family;
        std::uintptr_t      rcxAfter;
    };
    using Fn = std::uint8_t (*)(std::uintptr_t rcx, std::uintptr_t viewsFirst, std::uintptr_t view, View* r14);

    GameLikeCode1005()
    {
        const std::uint8_t code[] = {
            0x55,                                      // push rbp
            0x41, 0x56,                                // push r14
            0x48, 0x81, 0xEC, 0x08, 0x01, 0x00, 0x00,  // sub rsp, 108h
            0x48, 0x89, 0xE5,                          // mov rbp, rsp
            0x4D, 0x89, 0xCE,                          // mov r14, r9
            0x4C, 0x39, 0xC2,                          // cmp rdx, r8
            0x0F, 0x94, 0xC0,                          // setz al
            0x04, 0x01,                                // add al, 1
            0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,  // nop x9
            0x88, 0x85, 0xE0, 0x00, 0x00, 0x00,        // mov [rbp+0E0h], al   (offset 0x21)
            0x49, 0x89, 0x4E, 0x08,                    // mov [r14+8], rcx
            0x0F, 0xB6, 0x85, 0xE0, 0x00, 0x00, 0x00,  // movzx eax, byte [rbp+0E0h]
            0x48, 0x81, 0xC4, 0x08, 0x01, 0x00, 0x00,  // add rsp, 108h
            0x41, 0x5E,                                // pop r14
            0x5D,                                      // pop rbp
            0xC3};                                     // ret
        page_.Write(0, code);
        page_.Seal();
    }

    std::uintptr_t At(std::size_t offset) const { return page_.At(offset); }

    /// The load action, and whether rcx came through unchanged.
    std::uint8_t Run(bool firstInFamily, bool additionalFamily, bool& rcxKept) const
    {
        std::uint8_t family[0x80]{};
        family[0x74]                  = additionalFamily ? 1 : 0;
        View                 view{family, 0};
        const std::uintptr_t viewId = 0x1000;
        const std::uintptr_t rcx    = 0xC0FFEE;
        const std::uint8_t   result =
            page_.Function<Fn>()(rcx, firstInFamily ? viewId : viewId + 8, viewId, &view);
        rcxKept = view.rcxAfter == rcx;
        return result;
    }

private:
    lss::test::ExecutablePage page_;
};

void TestRedirect1005()
{
    std::puts("1.005: the redirect applies the rule through [r14], stores to [rbp+0E0h], leaves rcx, and restores");
    const GameLikeCode1005 game;
    bool                   kept = false;
    EXPECT(game.Run(true, true, kept) == kLoadActionClear && kept);  // the shipped behaviour: ignores the flag

    lss::platform::CodeRedirect redirect;
    EXPECT(redirect.Install(game.At(GameLikeCode1005::kStoreOffset), lss::facts::kOutputLoadActionStoreBytes1005,
                            lss::renderer::kFirstUnlessAdditionalLoadAction1005, game.At(GameLikeCode1005::kResumeOffset)));
    EXPECT(game.Run(true, false, kept) == kLoadActionClear && kept);
    EXPECT(game.Run(true, true, kept) == kLoadActionLoad && kept);
    EXPECT(game.Run(false, false, kept) == kLoadActionLoad && kept);
    EXPECT(game.Run(false, true, kept) == kLoadActionLoad && kept);
    // The research build's store bytes are not 1.005's: the redirect refuses them.
    lss::platform::CodeRedirect wrong;
    EXPECT(!wrong.Install(game.At(GameLikeCode1005::kStoreOffset), GameLikeCode::kStore, kFirstUnlessAdditionalLoadAction,
                          game.At(GameLikeCode1005::kStoreOffset + 7)));
    EXPECT(redirect.Restore());
    EXPECT(game.Run(true, true, kept) == kLoadActionClear && kept);
}

void TestJumpEncoding()
{
    std::puts("jump: rel32 measured from the end of the instruction, refused out of reach");
    const auto forward = EncodeJump(0x1000, 0x1010);
    EXPECT(forward && (*forward)[0] == 0xE9 && (*forward)[1] == 0x0B && (*forward)[4] == 0x00);
    const auto backward = EncodeJump(0x1000, 0x1000);  // to itself: -5
    EXPECT(backward && (*backward)[1] == 0xFB && (*backward)[4] == 0xFF);
    // The displacement is counted from the end of the 5-byte jump: +0x7FFFFFFF is the farthest reach.
    EXPECT(EncodeJump(0x140000000ull, 0x140000000ull + 5 + 0x7FFFFFFFull));
    EXPECT(!EncodeJump(0x140000000ull, 0x140000000ull + 5 + 0x80000000ull));
    EXPECT(EncodeJump(0x140000000ull, 0x140000000ull + 5 - 0x80000000ull));
    EXPECT(!EncodeJump(0x140000000ull, 0x140000000ull + 5 - 0x80000001ull));
}

}  // namespace

int main()
{
    TestTheStockRule();
    TestRedirectInstallsRunsAndRestores();
    TestRedirectRefusesWhatItDoesNotRecognise();
    TestRedirect1005();
    TestJumpEncoding();

    return lss::test::Report();
}
