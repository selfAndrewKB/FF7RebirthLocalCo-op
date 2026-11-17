#pragma once

// The machine code that picks the screen edge an off-screen enemy arrow pins to by where the ray from
// the centre actually leaves the half, while the HUD is laid out in a split half
// (research/RE-p2-overlay-ui-size.md).
//
// EndUI__PlaceMarkerWidgetAtWorld_Clamped chooses a side edge when the arrow's angle is
// between 60° and 120°, else the top or bottom edge. Those angles are a 16:9 rectangle's corners; in
// a 32:9 half (corner ~74°) arrows between 60° and 74° leave the half, and in an 8:9 half (corner
// ~42°) side arrows bunch in the middle. The ray from the centre meets a side edge first exactly when
// |ySide − cy| ≤ cy, where ySide is the side edge's y the function has already computed.
//
// Replaced: `vfmadd231ss xmm8, xmm13, [rsp+24h]` (7 bytes at kArrowEdgeSite, inside one block), the last
// instruction before the game's split (`jbe` at the site + 7 on the 60° compare, then the 120° compare
// and `jae`). The code runs that instruction, then:
//   - flag set (a split half): dx = 0 (xmm14 = 0) → top/bottom; else |ySide − cy| ≤ cy → side, else
//     top/bottom;
//   - flag clear: the game's own two compares (the 60° flags cannot survive the flag test, so both
//     are redone against the game's constants) → exactly the game's choice;
// and jumps to the side branch (kArrowEdgeSide) or the top/bottom branch (the site + 0x61).
// Registers there (the function is checked byte for byte up to the site + 0x17): xmm0 = ySide,
// xmm10 = cy, xmm14 = cx/|dx|, xmm6 = 0, xmm1 = |angle|; xmm3 and xmm4 are free (clobbered by the
// atan2 call before). Only rax, xmm3, xmm4 and the flags change; no calls, no stack writes.

#include "platform/verified_code.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lss::hud {

inline constexpr std::uintptr_t kArrowEdgeSite       = 0x140E8ED90;
inline constexpr std::uintptr_t kArrowEdgeSide       = 0x140E8EDA3;  // the side branch
inline constexpr std::uintptr_t kArrowEdgeSixty      = 0x145F0A2F0;  // 60.0f (read at the site - 0x17)
inline constexpr std::uintptr_t kArrowEdgeOneTwenty  = 0x145F0A7F4;  // 120.0f (read at the site + 9)

/// The site - 0x74 through the site + 0x16, as the research read it (the split and the side branch's first compare).
inline constexpr std::uint8_t kArrowEdgeBytes[] = {
    0xC5, 0xFA, 0x11, 0x44, 0x24, 0x24, 0xC5, 0xF8, 0x54, 0xC3, 0xC5, 0xF8, 0x2F, 0xC6, 0xC4, 0xC1,
    0x3A, 0x5C, 0xE3, 0xC4, 0x41, 0x10, 0x57, 0xED, 0xC4, 0x41, 0x08, 0x57, 0xF6, 0xC4, 0x41, 0x42,
    0x5C, 0xC2, 0xC4, 0xC1, 0x42, 0x5C, 0xCA, 0x76, 0x04, 0xC5, 0x22, 0x5E, 0xF0, 0xC4, 0xC1, 0x78,
    0x28, 0xD0, 0xC5, 0xE8, 0x54, 0xD3, 0xC5, 0xF8, 0x2F, 0xD6, 0x76, 0x04, 0xC5, 0x2A, 0x5E, 0xEA,
    0xC5, 0xF8, 0x28, 0xC4, 0xE8, 0x4B, 0x97, 0x9C, 0xFF, 0xC5, 0xFA, 0x59, 0x15, 0xD3, 0xAF, 0x07,
    0x05, 0xC5, 0xF8, 0x28, 0xCA, 0xC5, 0xE8, 0x54, 0x0D, 0x97, 0xA9, 0x07, 0x05, 0xC5, 0xF8, 0x2F,
    0x0D, 0x6F, 0xB5, 0x07, 0x05, 0xC4, 0xC1, 0x78, 0x28, 0xC2, 0xC4, 0xC2, 0x09, 0xB9, 0xC0, 0xC4,
    0x41, 0x78, 0x28, 0xC3, 0xC4, 0x62, 0x11, 0xB9, 0x44, 0x24, 0x24, 0x76, 0x58, 0xC5, 0xF8, 0x2F,
    0x0D, 0x53, 0xBA, 0x07, 0x05, 0x73, 0x4E, 0xC5, 0xF8, 0x2F, 0xD6};
inline constexpr platform::FunctionFact kArrowEdgeCheckStart{L"the off-screen arrow's edge split", 0x140E8ED1C,
                                                             kArrowEdgeBytes};  // checked from here...
static_assert(sizeof(kArrowEdgeBytes) == 0x140E8EDA7 - kArrowEdgeCheckStart.address);

/// The top/bottom branch, from its first instruction (vmovss xmm0, [rip+...]).
inline constexpr std::uint8_t kArrowEdgeTopBottomBytes[] = {0xC5, 0xFA, 0x10, 0x05, 0x6B, 0xAC, 0x07, 0x05};
inline constexpr platform::FunctionFact kArrowEdgeTopBottom{L"the arrow edge's top/bottom branch", 0x140E8EDF1,
                                                            kArrowEdgeTopBottomBytes};

/// The instruction replaced: vfmadd231ss xmm8, xmm13, dword ptr [rsp+24h].
inline constexpr std::uint8_t kArrowEdgeFmaBytes[] = {0xC4, 0x62, 0x11, 0xB9, 0x44, 0x24, 0x24};

inline constexpr std::size_t kArrowEdgeCodeSize = 110;

/// Live addresses: the split flag, the two branches, and the game's 60.0f and 120.0f.
inline std::array<std::uint8_t, kArrowEdgeCodeSize> ArrowEdgeCode(const std::uint8_t& split, std::uintptr_t side,
                                                                  std::uintptr_t topBottom, std::uintptr_t sixty,
                                                                  std::uintptr_t oneTwenty) noexcept
{
    std::array<std::uint8_t, kArrowEdgeCodeSize> code = {
        0xC4, 0x62, 0x11, 0xB9, 0x44, 0x24, 0x24,  //   0: vfmadd231ss xmm8, xmm13, [rsp+24h] (the replaced instruction)
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,        //   7: mov      rax, &split
        0x80, 0x38, 0x00,                          //  17: cmp      byte [rax], 0
        0x74, 0x36,                                //  20: je       game (76)
        0xC5, 0x78, 0x2E, 0xF6,                    //  22: vucomiss xmm14, xmm6        dx == 0: straight up or down
        0x74, 0x22,                                //  26: je       topBottom (62)
        0xC4, 0xC1, 0x7A, 0x5C, 0xDA,              //  28: vsubss   xmm3, xmm0, xmm10  ySide - cy
        0xC5, 0xCA, 0x5C, 0xE3,                    //  33: vsubss   xmm4, xmm6, xmm3   -(ySide - cy)
        0xC5, 0xE2, 0x5F, 0xDC,                    //  37: vmaxss   xmm3, xmm3, xmm4   |ySide - cy|
        0xC4, 0xC1, 0x78, 0x2F, 0xDA,              //  41: vcomiss  xmm3, xmm10
        0x77, 0x0E,                                //  46: ja       topBottom (62)     the ray leaves by the top or bottom
        0xFF, 0x25, 0, 0, 0, 0,                    //  48: side: jmp [rip+0]
        0, 0, 0, 0, 0, 0, 0, 0,                    //  54:   the side branch
        0xFF, 0x25, 0, 0, 0, 0,                    //  62: topBottom: jmp [rip+0]
        0, 0, 0, 0, 0, 0, 0, 0,                    //  68:   the top/bottom branch
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,        //  76: game: mov rax, &60.0f
        0xC5, 0xF8, 0x2F, 0x08,                    //  86: vcomiss  xmm1, [rax]
        0x76, 0xE2,                                //  90: jbe      topBottom (62)
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,        //  92: mov      rax, &120.0f
        0xC5, 0xF8, 0x2F, 0x08,                    // 102: vcomiss  xmm1, [rax]
        0x73, 0xD2,                                // 106: jae      topBottom (62)
        0xEB, 0xC2,                                // 108: jmp      side (48)
    };                                             // 110: (the appended jump back is never reached)
    const auto put = [&code](std::size_t at, std::uint64_t value) { std::memcpy(&code[at], &value, sizeof(value)); };
    put(9, reinterpret_cast<std::uint64_t>(&split));
    put(54, side);
    put(68, topBottom);
    put(78, sixty);
    put(94, oneTwenty);
    return code;
}

}  // namespace lss::hud
