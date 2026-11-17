#pragma once

// The machine code that makes the HUD's "centred 1920x1080 frame" the whole layout while the HUD is
// laid out in a split half (research/RE-p2-overlay-ui-size.md).
//
// EndUI__GetCentered1920x1080Frame(FBox2D* out) returns a 1920x1080 box centred in the
// overlay's layout (L = UI size / UI scale). The lock-on target's name bar, the popups and ~30 other
// placements clamp into it. For a 16:9 image (every single-player case) the box is the whole layout;
// in a 32:9 or 8:9 split half it is a 16:9 strip in the middle, so an off-screen target's name pins
// partway across the half. While the mod's band lays the HUD out in the half, the box is made the
// whole layout, as a 16:9 image gives it.
//
// The function's last store, `vmovss [rbx+8], xmm0` (Max.X, 5 bytes at kCenteredFrameSite, inside one
// block), is replaced. There rbx = out, xmm4 = L.X and xmm5 = L.Y (the whole function up to it is
// checked byte for byte before install). While the flag is set the code writes Min = (0,0) and
// Max = L; otherwise it runs the replaced store. rax (set again by the next instruction) and the
// flags change; nothing else. No calls, no stack use.

#include "platform/verified_code.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lss::hud {

inline constexpr std::uintptr_t kCenteredFrameSite   = 0x14130F468;
inline constexpr std::uintptr_t kCenteredFrameResume = 0x14130F46D;

/// The function from its entry through the replaced store, as the research read it.
inline constexpr std::uint8_t kCenteredFrameBytes[] = {
    0x40, 0x53, 0xB8, 0x40, 0x00, 0x00, 0x00, 0xE8, 0xA4, 0xC7, 0xE5, 0x00,
    0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xD9, 0x48, 0x8D, 0x4C, 0x24, 0x50, 0xC5,
    0xF8, 0x29, 0x74, 0x24, 0x30, 0xC5, 0xF8, 0x29, 0x7C, 0x24, 0x20, 0xE8,
    0x10, 0x8A, 0x6C, 0xFF, 0xC5, 0xFA, 0x10, 0x7C, 0x24, 0x50, 0xC5, 0xFA,
    0x10, 0x74, 0x24, 0x54, 0xC5, 0xC0, 0x14, 0xC6, 0xC4, 0xE1, 0xF9, 0x7E,
    0xC1, 0xE8, 0x86, 0x85, 0x8B, 0xFF, 0xC5, 0xFA, 0x10, 0x0D, 0xEE, 0xA2,
    0xBF, 0x04, 0xC5, 0xF2, 0x5E, 0xC0, 0xC5, 0xFA, 0x59, 0xE7, 0xC5, 0xDA,
    0x5C, 0x0D, 0x76, 0xB4, 0xBF, 0x04, 0xC5, 0xF2, 0x59, 0x1D, 0xDE, 0xA3,
    0xBF, 0x04, 0xC5, 0xF8, 0x28, 0x7C, 0x24, 0x20, 0xC5, 0xFA, 0x59, 0xEE,
    0xC5, 0xD2, 0x5C, 0x05, 0x60, 0xB4, 0xBF, 0x04, 0xC5, 0xFA, 0x59, 0x0D,
    0xC4, 0xA3, 0xBF, 0x04, 0xC5, 0xF8, 0x28, 0x74, 0x24, 0x30, 0xC5, 0xFA,
    0x11, 0x4B, 0x04, 0xC5, 0xD2, 0x5C, 0xC9, 0xC5, 0xDA, 0x5C, 0xC3, 0xC5,
    0xFA, 0x11, 0x4B, 0x0C, 0xC5, 0xFA, 0x11, 0x1B, 0xC5, 0xFA, 0x11, 0x43,
    0x08};
inline constexpr platform::FunctionFact kCenteredFrame{L"EndUI__GetCentered1920x1080Frame", 0x14130F3D0,
                                                       kCenteredFrameBytes};
static_assert(sizeof(kCenteredFrameBytes) == kCenteredFrameResume - kCenteredFrame.address);

/// The instruction replaced: vmovss [rbx+8], xmm0.
inline constexpr std::uint8_t kCenteredFrameStoreBytes[] = {0xC5, 0xFA, 0x11, 0x43, 0x08};

inline constexpr std::size_t kCenteredFrameCodeSize = 37;

/// `wholeLayout` is a byte: nonzero while the band lays the HUD out in a split half.
inline std::array<std::uint8_t, kCenteredFrameCodeSize> CenteredFrameCode(const std::uint8_t& wholeLayout) noexcept
{
    std::array<std::uint8_t, kCenteredFrameCodeSize> code = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,  //  0: mov    rax, &wholeLayout
        0x80, 0x38, 0x00,                    // 10: cmp    byte [rax], 0
        0x74, 0x11,                          // 13: je     stock (32)
        0x31, 0xC0,                          // 15: xor    eax, eax
        0x48, 0x89, 0x03,                    // 17: mov    [rbx], rax          Min = (0, 0)
        0xC5, 0xFA, 0x11, 0x63, 0x08,        // 20: vmovss [rbx+8], xmm4       Max.X = L.X
        0xC5, 0xFA, 0x11, 0x6B, 0x0C,        // 25: vmovss [rbx+0Ch], xmm5     Max.Y = L.Y
        0xEB, 0x05,                          // 30: jmp    done (37)
        0xC5, 0xFA, 0x11, 0x43, 0x08,        // 32: stock: vmovss [rbx+8], xmm0 (the replaced instruction)
    };                                       // 37: done (the jump back to the next instruction is appended here)
    const auto address = reinterpret_cast<std::uint64_t>(&wholeLayout);
    std::memcpy(&code[2], &address, sizeof(address));
    return code;
}

}  // namespace lss::hud
