#include "hud/ui_scale.h"

#include "hud/overlay_band.h"
#include "hud/overlay_layout.h"
#include "platform/code_redirect.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/module_pin.h"

#include <cstdint>
#include <cstring>

namespace lss::hud {

namespace {

// EndUI__ContainScaleFor1920x1080, read from the running game on 2026-09-25. Checked whole: the
// replacement reproduces it, so it must be exactly this code.
constexpr std::uintptr_t kContainScale = 0x1409F52B0;  // float (FVector2D size), size packed in rcx
constexpr std::uint8_t   kContainScaleBytes[] = {
    0xC5, 0xFA, 0x6F, 0x1D, 0x48, 0xD4, 0x6A, 0x05,        // vmovdqu xmm3, [abs mask]   <- replaced
    0xC5, 0xFA, 0x10, 0x25, 0xC4, 0xD3, 0x6A, 0x05,        // vmovss  xmm4, [1e-8]
    0x48, 0x89, 0x4C, 0x24, 0x10,                          // mov [rsp+10h], rcx
    0xC5, 0xFA, 0x10, 0x54, 0x24, 0x10,                    // vmovss xmm2, [rsp+10h]      W
    0xC5, 0xF8, 0x28, 0xC2, 0xC5, 0xE8, 0x54, 0xC3,        // |W|
    0xC5, 0xF8, 0x2F, 0xC4, 0x76, 0x37,                    // |W| <= eps (or NaN) -> 1.0
    0xC5, 0xFA, 0x10, 0x4C, 0x24, 0x14,                    // vmovss xmm1, [rsp+14h]      H
    0xC5, 0xF8, 0x28, 0xC1, 0xC5, 0xF0, 0x54, 0xC3,        // |H|
    0xC5, 0xF8, 0x2F, 0xC4, 0x76, 0x23,                    // |H| <= eps (or NaN) -> 1.0
    0xC5, 0xEA, 0x5E, 0xC1,                                // W / H
    0xC5, 0xF8, 0x2F, 0x05, 0x27, 0xD7, 0x6A, 0x05,        // vs 16/9
    0x0F, 0x82, 0x8D, 0x3A, 0xF2, 0x01,                    // below (or NaN) -> W * (1/1920)
    0xC5, 0xF2, 0x59, 0x05, 0x29, 0xE8, 0x6A, 0x05,        // H * (1/1080)
    0xC5, 0xFA, 0x5F, 0x05, 0x51, 0xD3, 0x6A, 0x05,        // max(.., 0.01)
    0xC3,                                                  // ret
    0xC5, 0xFA, 0x10, 0x05, 0xDC, 0xD3, 0x6A, 0x05, 0xC3}; // 1.0 ; ret
constexpr std::uintptr_t kContainScaleWidthBranch = 0x142918D8C;
constexpr std::uint8_t   kContainScaleWidthBranchBytes[] = {
    0xC5, 0xEA, 0x59, 0x05, 0xAC, 0x47, 0xC5, 0x03,  // vmulss xmm0, xmm2, [1/1920]
    0xE9, 0x6E, 0xC5, 0x0D, 0xFE};                   // jmp to the max
constexpr std::size_t kReplacedLength = 8;           // the first instruction

platform::CodeRedirect g_entry;

/// Same signature as EndUI__ContainScaleFor1920x1080: the FVector2D arrives packed in rcx.
float ContainScale(std::uint64_t packedSize)
{
    float size[2] = {};
    std::memcpy(size, &packedSize, sizeof(size));
    const std::uint64_t band = CurrentBandPacked();
    return OverlayUIScale(size[0], size[1], band != 0 && band == packedSize);
}

}  // namespace

bool InstallOverlayUIScale(const platform::GameImage& image) noexcept
{
    const auto site = image.Rebase(kContainScale, sizeof(kContainScaleBytes));
    if (!site || !image.BytesMatch(kContainScale, kContainScaleBytes) ||
        !image.BytesMatch(kContainScaleWidthBranch, kContainScaleWidthBranchBytes))
    {
        platform::Log(L"HUD UI scale: EndUI__ContainScaleFor1920x1080 is not the code the replacement reproduces; "
                      L"left alone (markers stay off in side-by-side halves)");
        return false;
    }
    // jmp qword ptr [rip+0] ; dq ContainScale. The redirect's own jump back after it is never reached.
    std::uint8_t    jump[14] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    const auto      target   = reinterpret_cast<std::uintptr_t>(&ContainScale);
    std::memcpy(jump + 6, &target, sizeof(target));
    if (!platform::PinThisModule() ||
        !g_entry.Install(*site, std::span<const std::uint8_t>(kContainScaleBytes, kReplacedLength), jump,
                         *site + kReplacedLength))
    {
        platform::Log(L"HUD UI scale: could not replace EndUI__ContainScaleFor1920x1080; left alone");
        return false;
    }
    platform::Log(L"HUD UI scale: installed (bands narrower than 16:9 use the layout scale)");
    return true;
}

void RemoveOverlayUIScale() noexcept
{
    if (g_entry.IsInstalled() && !g_entry.Restore())
    {
        platform::Log(L"HUD UI scale: could not restore EndUI__ContainScaleFor1920x1080 (changed after us); left in "
                      L"place (it reproduces the game's function whenever no band is active)");
    }
}

}  // namespace lss::hud
