#pragma once

// The machine code that lets player 2's HUD pass skip the minimap's own map render
// (RE-oom-path.md, "Player 2's minimap paint: hook facts and the skip").
//
// Each minimap widget's OnPaint renders its map into its render target through the menu renderer's
// pool 0 (acquire a slot, paint the map into a layer entry, submit), then adds the element that shows
// that render target in the HUD. One whole instruction before the acquire block is replaced through
// platform/code_redirect.h. While the skip byte is set (player 2's pass), the code jumps over the
// block to the instruction after the submit, doing first what that code needs from the block:
//
// Circle (SEndNaviMapCircle, site 0x140FC6C95 `mov rcx,[r15+1C0h]`, to 0x140FC6F18): xmm13 = 0. rcx is
// loaded by the replaced instruction, so it is free before it; the code after the submit reloads rcx
// and rax first. Flags are not read before the next call on either path.
//
// Octagon (SEndNaviMapOctagon, site 0x143A60235 `vmovss [rsp+78h],xmm15`, to 0x143A604E5): the two
// stores at the site, then xmm13 = 0. rax was last written before the site and is not read before the
// next call (its arguments are rcx and rdx); the code after the submit writes rax first.
//
// Each path counts itself in the fields (lock inc): the skips and the stock paints per widget.
//
// Game version 1.005 (RE-1005-hud-2.md), circle only: the research site (0x14130A4FF there) crosses a 16-byte block, so
// the site is the next instruction, `vmovq rdx,xmm0` (0x14130A506), still before the resize, acquire, paint and submit
// (to 0x14130A77E). rdx is loaded by the replaced instruction, so it is free before it, and rcx (already loaded with
// [r15+1C0h], the resize's argument) is kept for the stock path. The code after the submit now also reads a stack slot
// the block writes: `vmovss xmm7,[rsp+40h]` (0x14130A78E), 1.0f stored at 0x14130A5A2 (1.004 loaded its 1.0f from
// .rdata after the submit), so the skip stores 1.0f there too.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lss::hud {

/// The fields the code reads and counts into, at fixed offsets (the code addresses them from one base).
struct MinimapOnceFields
{
    alignas(8) std::uint64_t skip = 0;  // +0x00: nonzero during player 2's pass with the switch on
    std::uint64_t circleSkipped   = 0;  // +0x08
    std::uint64_t octagonSkipped  = 0;  // +0x10
    std::uint64_t circlePainted   = 0;  // +0x18: the stock path (the map rendered)
    std::uint64_t octagonPainted  = 0;  // +0x20
};
static_assert(offsetof(MinimapOnceFields, circleSkipped) == 0x08 && offsetof(MinimapOnceFields, octagonSkipped) == 0x10 &&
              offsetof(MinimapOnceFields, circlePainted) == 0x18 && offsetof(MinimapOnceFields, octagonPainted) == 0x20);

inline constexpr std::size_t kCircleSkipCodeSize     = 51;
inline constexpr std::size_t kOctagonSkipCodeSize    = 62;
inline constexpr std::size_t kCircleSkipCodeSize1005 = 57;

/// The instruction each site replaces, as the research read it.
inline constexpr std::uint8_t kCircleSiteBytes[]     = {0x49, 0x8B, 0x8F, 0xC0, 0x01, 0x00, 0x00};  // mov rcx,[r15+1C0h]
inline constexpr std::uint8_t kOctagonSiteBytes[]    = {0xC5, 0x7A, 0x11, 0x7C, 0x24, 0x78};        // vmovss [rsp+78h],xmm15
inline constexpr std::uint8_t kCircleSiteBytes1005[] = {0xC4, 0xE1, 0xF9, 0x7E, 0xC2};              // vmovq rdx,xmm0

namespace detail {
inline void PutQword(std::uint8_t* at, std::uint64_t value) noexcept
{
    std::memcpy(at, &value, sizeof(value));
}
}  // namespace detail

/// `afterSubmit` is the live address of the instruction after the circle's submit call.
inline std::array<std::uint8_t, kCircleSkipCodeSize> CircleSkipCode(const MinimapOnceFields& fields,
                                                                     std::uintptr_t afterSubmit) noexcept
{
    std::array<std::uint8_t, kCircleSkipCodeSize> code = {
        0x48, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0,        //  0: mov  rcx, &fields
        0x80, 0x39, 0x00,                          // 10: cmp  byte [rcx], 0     the skip byte
        0x74, 0x18,                                // 13: je   stock (39)
        0xF0, 0x48, 0xFF, 0x41, 0x08,              // 15: lock inc qword [rcx+8]  circleSkipped
        0xC4, 0x41, 0x10, 0x57, 0xED,              // 20: vxorps xmm13, xmm13, xmm13
        0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,        // 25: jmp  [rip+0]           past the submit
        0, 0, 0, 0, 0, 0, 0, 0,                    // 31: afterSubmit
        0xF0, 0x48, 0xFF, 0x41, 0x18,              // 39: stock: lock inc qword [rcx+18h]  circlePainted
        0x49, 0x8B, 0x8F, 0xC0, 0x01, 0x00, 0x00,  // 44: mov  rcx, [r15+1C0h] (the replaced instruction)
    };                                             // 51: (the jump back is appended here)
    detail::PutQword(&code[2], reinterpret_cast<std::uint64_t>(&fields));
    detail::PutQword(&code[31], afterSubmit);
    return code;
}

/// Game version 1.005's circle (above). `afterSubmit` is the live address of the instruction after its submit call.
inline std::array<std::uint8_t, kCircleSkipCodeSize1005> CircleSkipCode1005(const MinimapOnceFields& fields,
                                                                             std::uintptr_t afterSubmit) noexcept
{
    std::array<std::uint8_t, kCircleSkipCodeSize1005> code = {
        0x48, 0xBA, 0, 0, 0, 0, 0, 0, 0, 0,              //  0: mov  rdx, &fields
        0x80, 0x3A, 0x00,                                // 10: cmp  byte [rdx], 0     the skip byte
        0x74, 0x20,                                      // 13: je   stock (47)
        0xF0, 0x48, 0xFF, 0x42, 0x08,                    // 15: lock inc qword [rdx+8]  circleSkipped
        0xC4, 0x41, 0x10, 0x57, 0xED,                    // 20: vxorps xmm13, xmm13, xmm13
        0xC7, 0x44, 0x24, 0x40, 0x00, 0x00, 0x80, 0x3F,  // 25: mov  dword [rsp+40h], 3F800000h  (1.0f)
        0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,              // 33: jmp  [rip+0]           past the submit
        0, 0, 0, 0, 0, 0, 0, 0,                          // 39: afterSubmit
        0xF0, 0x48, 0xFF, 0x42, 0x18,                    // 47: stock: lock inc qword [rdx+18h]  circlePainted
        0xC4, 0xE1, 0xF9, 0x7E, 0xC2,                    // 52: vmovq rdx, xmm0 (the replaced instruction)
    };                                                   // 57: (the jump back is appended here)
    detail::PutQword(&code[2], reinterpret_cast<std::uint64_t>(&fields));
    detail::PutQword(&code[39], afterSubmit);
    return code;
}

/// `afterSubmit` is the live address of the instruction after the octagon's submit call.
inline std::array<std::uint8_t, kOctagonSkipCodeSize> OctagonSkipCode(const MinimapOnceFields& fields,
                                                                       std::uintptr_t afterSubmit) noexcept
{
    std::array<std::uint8_t, kOctagonSkipCodeSize> code = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,        //  0: mov  rax, &fields
        0x80, 0x38, 0x00,                          // 10: cmp  byte [rax], 0     the skip byte
        0x74, 0x24,                                // 13: je   stock (51)
        0xF0, 0x48, 0xFF, 0x40, 0x10,              // 15: lock inc qword [rax+10h]  octagonSkipped
        0xC5, 0x7A, 0x11, 0x7C, 0x24, 0x78,        // 20: vmovss [rsp+78h], xmm15 (the site's own store)
        0xC5, 0xFA, 0x11, 0x74, 0x24, 0x70,        // 26: vmovss [rsp+70h], xmm6  (the next instruction's)
        0xC4, 0x41, 0x10, 0x57, 0xED,              // 32: vxorps xmm13, xmm13, xmm13
        0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,        // 37: jmp  [rip+0]           past the submit
        0, 0, 0, 0, 0, 0, 0, 0,                    // 43: afterSubmit
        0xF0, 0x48, 0xFF, 0x40, 0x20,              // 51: stock: lock inc qword [rax+20h]  octagonPainted
        0xC5, 0x7A, 0x11, 0x7C, 0x24, 0x78,        // 56: vmovss [rsp+78h], xmm15 (the replaced instruction)
    };                                             // 62: (the jump back is appended here)
    detail::PutQword(&code[2], reinterpret_cast<std::uint64_t>(&fields));
    detail::PutQword(&code[43], afterSubmit);
    return code;
}

}  // namespace lss::hud
