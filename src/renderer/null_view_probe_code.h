#pragma once

// The machine code of the dead-view probe (renderer/null_view_probe.h; RE-p2-main-menu-crash.md).
//
// ME_BindSRVsAndTransitionResources (0x140816830) copies each shader-resource view of a pixel-shader
// array into a descriptor table for ID3D12Device::CopyDescriptors:
//     0x140816ADE  mov rsi, [rdx+rsi]        ; the view in array element ebp (rdx = ebp*8)
//     0x140816AE2  test rsi, rsi / je 0x1428EED43   an empty element: the null descriptor, see below
//     0x140816AEB  mov rax, [rsi+10h]        ; the view's CPU descriptor handle
//     0x140816AEF  mov [rdx+r15], rax        ; into the table
//     0x140816AF3  cmp byte [rsi+80h], 0     ; the site (one instruction, one aligned block)
// A view whose handle is 0 reaches the driver as a read of address 0: the main-menu crash. The
// game's own empty-element path (0x1428EED43) writes the null descriptor `[[r8+20h]+8]` into the
// table and goes on at 0x140816BA4, the loop's next element; r8 (the context) and r15 (the table)
// hold the same values here as there.
//
// The code runs in place of the site's compare. Handle non-zero: the compare, and the game goes on.
// Handle 0: record the view, its resource (+0x40), its vtable, the stage (the function keeps it at
// [rsp+0C0h] from its entry, 0x1408168C4; rbx holds it only until the loop reuses rbx), the slot
// (r12) and the element (rbp), count the hit, then do exactly what the game does for an empty
// element. Only rax and the table entry change beyond that; rax is dead on both paths (0x140816B1A
// and 0x140816BA4 do not read it). Stores use `mov [imm64], rax`, so no other register and no stack.
//
// Kept apart from the installer so the host tests can execute exactly these bytes.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lss::renderer {

/// What the probe records. Written by the render-side code (any thread that flushes a draw's
/// resources), read by the game thread; each field is an aligned 8-byte value. The fields hold the
/// latest hit; `hits` is counted with a locked increment.
struct NullViewProbe
{
    alignas(8) std::uint64_t hits = 0;
    std::uint64_t view     = 0;  // the view object
    std::uint64_t resource = 0;  // view+0x40
    std::uint64_t vtable   = 0;  // view+0
    std::uint64_t stage    = 0;  // [rsp+0C0h]: the shader stage (5 = pixel)
    std::uint64_t slot     = 0;  // r12: the resource slot
    std::uint64_t element  = 0;  // rbp: the element within the slot's array
};
static_assert(sizeof(NullViewProbe) == 7 * 8);

inline constexpr std::size_t kNullViewProbeCodeSize = 136;

/// The instruction replaced at 0x140816AF3, as the research read it.
inline constexpr std::uint8_t kNullViewProbeSiteBytes[] = {0x80, 0xBE, 0x80, 0x00, 0x00, 0x00, 0x00};  // cmp byte [rsi+80h], 0

namespace detail {
inline void PutQword(std::uint8_t* at, std::uint64_t value) noexcept
{
    std::memcpy(at, &value, sizeof(value));
}
}  // namespace detail

/// `emptyElement` is the live address the game's empty-element path goes on at (0x140816BA4).
inline std::array<std::uint8_t, kNullViewProbeCodeSize> NullViewProbeCode(const NullViewProbe& probe,
                                                                          std::uintptr_t emptyElement) noexcept
{
    std::array<std::uint8_t, kNullViewProbeCodeSize> code = {
        0x48, 0x85, 0xC0,                                //   0: test rax, rax            the handle just stored
        0x75, 0x7C,                                      //   3: jnz  129 (stock)
        0x48, 0x8B, 0xC6,                                //   5: mov  rax, rsi
        0x48, 0xA3, 0, 0, 0, 0, 0, 0, 0, 0,              //   8: mov  [&view], rax
        0x48, 0x8B, 0x46, 0x40,                          //  18: mov  rax, [rsi+40h]
        0x48, 0xA3, 0, 0, 0, 0, 0, 0, 0, 0,              //  22: mov  [&resource], rax
        0x48, 0x8B, 0x06,                                //  32: mov  rax, [rsi]
        0x48, 0xA3, 0, 0, 0, 0, 0, 0, 0, 0,              //  35: mov  [&vtable], rax
        0x48, 0x8B, 0x84, 0x24, 0xC0, 0x00, 0x00, 0x00,  //  45: mov  rax, [rsp+0C0h]    the stage
        0x48, 0xA3, 0, 0, 0, 0, 0, 0, 0, 0,              //  53: mov  [&stage], rax
        0x49, 0x8B, 0xC4,                                //  63: mov  rax, r12
        0x48, 0xA3, 0, 0, 0, 0, 0, 0, 0, 0,              //  66: mov  [&slot], rax
        0x48, 0x8B, 0xC5,                                //  76: mov  rax, rbp
        0x48, 0xA3, 0, 0, 0, 0, 0, 0, 0, 0,              //  79: mov  [&element], rax
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,              //  89: mov  rax, &hits
        0xF0, 0x48, 0xFF, 0x00,                          //  99: lock inc qword [rax]
        0x49, 0x8B, 0x40, 0x20,                          // 103: mov  rax, [r8+20h]      the game's empty-element path:
        0x48, 0x8B, 0x40, 0x08,                          // 107: mov  rax, [rax+8]       its null descriptor
        0x4A, 0x89, 0x04, 0x3A,                          // 111: mov  [rdx+r15], rax     into the table
        0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,              // 115: jmp  [rip+0]            and the loop's next element
        0, 0, 0, 0, 0, 0, 0, 0,                          // 121: emptyElement
        0x80, 0xBE, 0x80, 0x00, 0x00, 0x00, 0x00,        // 129: stock: cmp byte [rsi+80h], 0 (the replaced instruction)
    };                                                   // 136: (the jump back to 0x140816AFA is appended here)
    detail::PutQword(&code[10], reinterpret_cast<std::uint64_t>(&probe.view));
    detail::PutQword(&code[24], reinterpret_cast<std::uint64_t>(&probe.resource));
    detail::PutQword(&code[37], reinterpret_cast<std::uint64_t>(&probe.vtable));
    detail::PutQword(&code[55], reinterpret_cast<std::uint64_t>(&probe.stage));
    detail::PutQword(&code[68], reinterpret_cast<std::uint64_t>(&probe.slot));
    detail::PutQword(&code[81], reinterpret_cast<std::uint64_t>(&probe.element));
    detail::PutQword(&code[91], reinterpret_cast<std::uint64_t>(&probe.hits));
    detail::PutQword(&code[121], static_cast<std::uint64_t>(emptyElement));
    return code;
}

}  // namespace lss::renderer
