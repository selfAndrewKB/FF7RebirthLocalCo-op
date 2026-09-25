#include "hud/layout_scale.h"

#include "platform/code_redirect.h"
#include "platform/image.h"
#include "platform/log.h"

#include <cstdint>

namespace lss::hud {

namespace {

// In SEndRootOverlay__RenderToCompositeLayer, after `sub edx, r9d` (edx = rect height) and
// `sub ecx, r8d` (ecx = rect width), read from the running game on 2026-09-25:
//   0x140C95FBE  vxorps    xmm7, xmm7, xmm7
//   0x140C95FC2  vcvtsi2ss xmm7, xmm7, edx               ; H
//   0x140C95FC6  vmulss    xmm0, xmm7, [1/1080]          ; s = H/1080     <- redirected
//   0x140C95FCE  vdivss    xmm1, xmm2, xmm0              ; xmm1 overwritten here, so free before
constexpr std::uintptr_t kContext        = 0x140C95FBE;
constexpr std::uint8_t   kContextBytes[] = {
    0xC5, 0xC0, 0x57, 0xFF,                          // vxorps xmm7, xmm7, xmm7
    0xC5, 0xC2, 0x2A, 0xFA,                          // vcvtsi2ss xmm7, xmm7, edx
    0xC5, 0xC2, 0x59, 0x05, 0x62, 0xDB, 0x40, 0x05,  // vmulss xmm0, xmm7, [rip+0x0540DB62] = [0x1460A3B30]
    0xC5, 0xEA, 0x5E, 0xC8};                         // vdivss xmm1, xmm2, xmm0
constexpr std::uintptr_t kSite     = 0x140C95FC6;
constexpr std::size_t    kReplaced = 8;
constexpr std::uintptr_t kResume   = 0x140C95FCE;

// min(H * (1/1080), W * (1/1920)) with the game's constants (0x1460A3B30, 0x14656D540), placed
// after the instructions and reached rip-relative, so the code is the same wherever it is placed:
//   +0   vmulss    xmm0, xmm7, [rip+18]   ; H/1080          (next +8,  constant at +26)
//   +8   vcvtsi2ss xmm1, xmm1, ecx        ; W
//   +12  vmulss    xmm1, xmm1, [rip+10]   ; W/1920          (next +20, constant at +30)
//   +20  vminss    xmm0, xmm0, xmm1
//   +24  jmp short +8                     ; over the constants, to the redirect's jump back
//   +26  1/1080, +30 1/1920
constexpr std::uint8_t kCode[] = {
    0xC5, 0xC2, 0x59, 0x05, 0x12, 0x00, 0x00, 0x00,
    0xC5, 0xF2, 0x2A, 0xC9,
    0xC5, 0xF2, 0x59, 0x0D, 0x0A, 0x00, 0x00, 0x00,
    0xC5, 0xFA, 0x5D, 0xC1,
    0xEB, 0x08,
    0xD6, 0xB9, 0x72, 0x3A,   // 1/1080 as the game stores it
    0x89, 0x88, 0x08, 0x3A};  // 1/1920 as the game stores it

platform::CodeRedirect g_redirect;

}  // namespace

bool InstallLayoutScale(const platform::GameImage& image) noexcept
{
    const auto site   = image.Rebase(kSite, kReplaced);
    const auto resume = image.Rebase(kResume);
    if (!site || !resume || !image.BytesMatch(kContext, kContextBytes) ||
        !g_redirect.Install(*site, std::span<const std::uint8_t>(kContextBytes + (kSite - kContext), kReplaced), kCode,
                            *resume))
    {
        platform::Log(L"HUD layout scale: the overlay's layout-scale code is not what the research read; left alone "
                      L"(side-by-side halves keep the game's height rule)");
        return false;
    }
    platform::Log(L"HUD layout scale: the overlay lays out by the contain rule (unchanged for 16:9)");
    return true;
}

void RemoveLayoutScale() noexcept
{
    if (g_redirect.IsInstalled() && !g_redirect.Restore())
    {
        platform::Log(L"HUD layout scale: could not restore the game's instruction (changed after us); left in place");
    }
}

}  // namespace lss::hud
