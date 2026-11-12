#pragma once

// The machine code that restores stock Unreal's rule for the final output's load action.
//
// Stock (ScreenPass.inl:87): a view's final output CLEARs the screen only when the view is the
// first of its family AND the family is not an additional one; otherwise it LOADs what is there.
// The shipped game dropped the second half of that condition
// (research/RE-additional-view-family-reader.md); the instructions before and at the store site:
//     store - 5  setz al               ; ZF from `cmp [Family->Views], View`
//     store - 2  add  al, 1            ; al = 2 (EClear) if first in the family, 1 (ELoad) otherwise
//     store      mov  [rsp+610h], al   ; store the load action
// With one view per family every view is first, so every family cleared the whole screen,
// erasing the players drawn before it.
//
// This code runs in place of the STORE: one whole instruction, so no thread can be stopped inside
// the bytes the redirect replaces, whenever it is installed or removed (a reload happens while the
// game renders). It receives the game's own result in al, turns a clear into a load when the
// family is additional, and performs the store; the redirect appends the jump back to the next
// instruction (store + 7). At the store, rcx holds the view's family: the scene renderer's copy,
// which includes bAdditionalViewFamily at +0x74 (research/RE-additional-view-family-reader.md). Only al
// and the flags change beyond the store itself; both are dead after it (the next instruction reloads rax,
// and a call follows).
//
// Kept apart from the installer so the host tests can execute exactly these bytes.

#include <cstdint>

namespace lss::renderer {

inline constexpr std::uint8_t kLoadActionClear = 2;  // ERenderTargetLoadAction::EClear
inline constexpr std::uint8_t kLoadActionLoad  = 1;  // ERenderTargetLoadAction::ELoad

inline constexpr std::uint8_t kFirstUnlessAdditionalLoadAction[] = {
    0x3C, kLoadActionClear,                    //        cmp  al, 2              the game chose to clear?
    0x75, 0x08,                                //        jne  store              no: it already keeps the screen
    0x80, 0x79, 0x74, 0x00,                    //        cmp  byte [rcx+74h], 0  bAdditionalViewFamily
    0x74, 0x02,                                //        je   store              a main family: clear, as the game chose
    0xB0, kLoadActionLoad,                     //        mov  al, 1              an additional family keeps the screen
    0x88, 0x84, 0x24, 0x10, 0x06, 0x00, 0x00,  // store: mov  [rsp+610h], al     the game's own store, unchanged
};                                             //        (the jump back to store + 7 is appended here)

// Game version 1.005 (research/RE-1005-core-facts.md): the same decision, compiled differently. AddPostProcessingPasses
// keeps an rbp frame, so the store is `mov [rbp+0E0h], al` (6 bytes; resume at store + 6).
// rcx no longer holds the family there: it is reloaded with [r13+10h] after the comparison and passed to the call
// at store + 0x15, so it must stay untouched. r14 holds the view (set early in the function, `mov r14, rdx`) and [r14]
// is its family, the scene renderer's copy with bAdditionalViewFamily at +0x74. rax and the flags are dead after the
// store (the next instruction reloads rax), so the family is read through rax, and al is set again either way.
inline constexpr std::uint8_t kFirstUnlessAdditionalLoadAction1005[] = {
    0x3C, kLoadActionClear,               //        cmp  al, 2              the game chose to clear?
    0x75, 0x0D,                           //        jne  store              no: it already keeps the screen
    0x49, 0x8B, 0x06,                     //        mov  rax, [r14]         the view's family
    0x80, 0x78, 0x74, 0x00,               //        cmp  byte [rax+74h], 0  bAdditionalViewFamily
    0xB0, kLoadActionClear,               //        mov  al, 2              (rax was just overwritten)
    0x74, 0x02,                           //        je   store              a main family: clear, as the game chose
    0xB0, kLoadActionLoad,                //        mov  al, 1              an additional family keeps the screen
    0x88, 0x85, 0xE0, 0x00, 0x00, 0x00,   // store: mov  [rbp+0E0h], al     the game's own store, unchanged
};                                        //        (the jump back to store + 6 is appended here)

}  // namespace lss::renderer
