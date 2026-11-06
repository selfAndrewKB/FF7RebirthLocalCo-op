#pragma once

// The machine code that lets a seat's overlay pass ask "the first game player" and get the
// seat's player (lever L2 of RE-p2-hud.md P8.6; facts in pass 12).
//
// SEndRootOverlay__RenderToCompositeLayer asks GEngine__GetFirstGamePlayerForGameViewport
// (0x140CCF8E4) once per render for the local player whose view rect it lays the HUD out in and
// caches (+0x318..+0x32C, which the marker math reads). The call site crosses a 16-byte block, so
// the function's first instruction is replaced instead (`mov eax,28h`, the size for its stack
// probe, 5 bytes inside one block): while the field names a local player, the function returns it
// at once, before it has touched the stack or any state; otherwise the replaced instruction runs
// and the function continues as the game's. The field is set only during a seat's pass on the
// game thread; none of the function's other callers runs then (pass 12).
//
// Only rax and the flags change on the early path (rax is the return value; flags are not kept
// across a call). Kept apart from the installer so the host tests can execute exactly these bytes.

#include <array>
#include <cstdint>
#include <cstring>

namespace lss::hud {

inline constexpr std::size_t kFirstPlayerCodeSize = 24;

/// The instruction replaced at 0x140CCF8E4, as the research read it.
inline constexpr std::uint8_t kFirstPlayerEntryBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00};  // mov eax, 28h

/// `answer` is an aligned 8-byte field: a ULocalPlayer* to return, or 0 for the game's own answer.
inline std::array<std::uint8_t, kFirstPlayerCodeSize> FirstPlayerCode(const std::uint64_t& answer) noexcept
{
    std::array<std::uint8_t, kFirstPlayerCodeSize> code = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,  //  0: mov  rax, &answer
        0x48, 0x8B, 0x00,                    // 10: mov  rax, [rax]
        0x48, 0x85, 0xC0,                    // 13: test rax, rax
        0x74, 0x01,                          // 16: jz   stock
        0xC3,                                // 18: ret               the answer, before the function's frame
        0xB8, 0x28, 0x00, 0x00, 0x00,        // 19: stock: mov eax, 28h (the replaced instruction)
    };                                       // 24: (the jump back to the next instruction is appended here)
    const auto address = reinterpret_cast<std::uint64_t>(&answer);
    std::memcpy(&code[2], &address, sizeof(address));
    return code;
}

}  // namespace lss::hud
