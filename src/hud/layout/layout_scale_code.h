#pragma once

// The bytes hud/layout/layout_scale.cpp places over the overlay's layout-scale instruction: the contain rule,
// min(height/1080, width/1920), with the game's own constants (research/RE-p2-hud.md; RE-1005-hud-2.md for 1.005).
// Pure data, so it is executed on the host (tests/layout_scale_code_test.cpp).

#include "platform/verified_code.h"

#include <cstddef>
#include <cstdint>

namespace lss::hud::layout_scale_code {

// In SEndRootOverlay__RenderToCompositeLayer, after `sub edx, r9d` (edx = rect height) and
// `sub ecx, r8d` (ecx = rect width), read from the running game:
//   context +0   vxorps    xmm7, xmm7, xmm7
//   context +4   vcvtsi2ss xmm7, xmm7, edx               ; H
//   context +8   vmulss    xmm0, xmm7, [1/1080]          ; s = H/1080     <- redirected (kSite)
//   context +16  vdivss    xmm1, xmm2, xmm0              ; xmm1 overwritten here, so free before
inline constexpr std::uint8_t   kContextBytes[] = {
    0xC5, 0xC0, 0x57, 0xFF,                          // vxorps xmm7, xmm7, xmm7
    0xC5, 0xC2, 0x2A, 0xFA,                          // vcvtsi2ss xmm7, xmm7, edx
    0xC5, 0xC2, 0x59, 0x05, 0x9C, 0xAF, 0x3F, 0x05,  // vmulss xmm0, xmm7, [rip+0x0540DB62] = the game's 1/1080
    0xC5, 0xEA, 0x5E, 0xC8};                         // vdivss xmm1, xmm2, xmm0
inline constexpr platform::FunctionFact kContext{L"the overlay's layout-scale instruction and its context", 0x140B0F014,
                                                 kContextBytes};
inline constexpr std::uintptr_t kSite     = 0x140B0F01C;
inline constexpr std::size_t    kReplaced = 8;
inline constexpr std::uintptr_t kResume   = 0x140B0F024;
/// Game version 1.004's vmulss at the site: its RIP-relative distance to the constant differs, so its own bytes are
/// compared where the redirect replaces it (kSiteBytes1004 is kContextBytes' third instruction there).
inline constexpr std::uint8_t kSiteBytes1004[] = {0xC5, 0xC2, 0x59, 0x05, 0x62, 0xDB, 0x40, 0x05};

// min(H * (1/1080), W * (1/1920)) with the game's constants (its 1/1080 and 1/1920 floats), placed
// after the instructions and reached rip-relative, so the code is the same wherever it is placed:
//   +0   vmulss    xmm0, xmm7, [rip+18]   ; H/1080          (next +8,  constant at +26)
//   +8   vcvtsi2ss xmm1, xmm1, ecx        ; W
//   +12  vmulss    xmm1, xmm1, [rip+10]   ; W/1920          (next +20, constant at +30)
//   +20  vminss    xmm0, xmm0, xmm1
//   +24  jmp short +8                     ; over the constants, to the redirect's jump back
//   +26  1/1080, +30 1/1920
inline constexpr std::uint8_t kCode[] = {
    0xC5, 0xC2, 0x59, 0x05, 0x12, 0x00, 0x00, 0x00,
    0xC5, 0xF2, 0x2A, 0xC9,
    0xC5, 0xF2, 0x59, 0x0D, 0x0A, 0x00, 0x00, 0x00,
    0xC5, 0xFA, 0x5D, 0xC1,
    0xEB, 0x08,
    0xD6, 0xB9, 0x72, 0x3A,   // 1/1080 as the game stores it
    0x89, 0x88, 0x08, 0x3A};  // 1/1920 as the game stores it
inline constexpr std::size_t kConstant1080 = 26;  // where the two constants sit in kCode
inline constexpr std::size_t kConstant1920 = 30;

// The vmulss crosses a 16-byte block (research/RE-1005-hud-2.md), and so does every single instruction around it
// that a jump fits in. The two instructions before it (vxorps, vcvtsi2ss: 8 bytes, one block, neither RIP-relative)
// are replaced instead by themselves followed by the code above, which resumes after the vmulss, so the game's vmulss
// is left in place unrun. A thread stopped between the two would resume inside the jump, so this is installed from
// the game thread, the only one rendering the overlay, while it is outside the render (platform/deferred_sites.h:
// the session's tick). In 1.004 the vmulss fits one block and kCode replaces it directly, at install.
inline constexpr std::size_t  kContextReplaced = 8;
inline constexpr std::uint8_t kContextCode[]   = {
    0xC5, 0xC0, 0x57, 0xFF,  // vxorps    xmm7, xmm7, xmm7   (the replaced instructions)
    0xC5, 0xC2, 0x2A, 0xFA,  // vcvtsi2ss xmm7, xmm7, edx
    0xC5, 0xC2, 0x59, 0x05, 0x12, 0x00, 0x00, 0x00,
    0xC5, 0xF2, 0x2A, 0xC9,
    0xC5, 0xF2, 0x59, 0x0D, 0x0A, 0x00, 0x00, 0x00,
    0xC5, 0xFA, 0x5D, 0xC1,
    0xEB, 0x08,
    0xD6, 0xB9, 0x72, 0x3A,
    0x89, 0x88, 0x08, 0x3A};  // kCode, unchanged: its rip-relative reads are to its own constants
static_assert(sizeof(kContextCode) == kContextReplaced + sizeof(kCode));

}  // namespace lss::hud::layout_scale_code
