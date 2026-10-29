#pragma once

// The machine code that keeps player 2's targeting run out of player 1's target panel, at the one
// panel request whose call site straddles a code block (RE-p2-targeting-scoped-tick.md §2, §4.3).
//
// The displayed-target update posts "old target cleared" to the target panel from a cold chunk entered
// only for that request (0x14295F095): it copies the old target (a TWeakPtr) into a local, adds a weak
// reference, and calls EndUI4_QueueTargetCleared, which releases that reference at its end; then it
// goes on at 0x14295F0C3. The call (0x14295F0BE) straddles a 16-byte block, so the chunk is gated at
// `mov [rsp+0x108], rax` (8 bytes at 0x14295F0A5, one instruction inside one block): the code runs the
// replaced instruction, and while the gate is set (player 2's run) it counts the skip and jumps to
// 0x14295F0C3, past the added reference and the call together, so the net effect on the reference
// count is the call's own (none) and the panel gets no request; otherwise the chunk goes on as the
// game's.
//
// rcx is written at 0x14295F0B6 before anything reads it, so the code may use it; after the skipped
// call it would hold nothing either. No calls, no stack use. Kept apart from the installer so the host
// tests can execute exactly these bytes.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lss::battle {

/// The gate the code reads: skip set during a further player's targeting run (any seat's: the gate is seat-neutral);
/// skipped counts the requests kept from player 1's panel.
struct PanelGate
{
    std::uint8_t  skip = 0;
    std::uint8_t  padding[3]{};
    std::uint32_t skipped = 0;
};
static_assert(offsetof(PanelGate, skipped) == 4);

inline constexpr std::size_t kPanelGateCodeSize = 40;

/// The instruction replaced at 0x14295F0A5, as the research read it.
inline constexpr std::uint8_t kPanelGateSiteBytes[] = {0x48, 0x89, 0x84, 0x24, 0x08, 0x01, 0x00, 0x00};  // mov [rsp+0x108], rax

/// `skipTo` is the live address the gated path goes on at (0x14295F0C3 in this build).
inline std::array<std::uint8_t, kPanelGateCodeSize> PanelGateCode(const PanelGate& gate, std::uintptr_t skipTo) noexcept
{
    std::array<std::uint8_t, kPanelGateCodeSize> code = {
        0x48, 0x89, 0x84, 0x24, 0x08, 0x01, 0x00, 0x00,  //  0: mov  [rsp+0x108], rax (the replaced instruction)
        0x48, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0,              //  8: mov  rcx, &gate
        0x80, 0x39, 0x00,                                // 18: cmp  byte [rcx], 0
        0x74, 0x11,                                      // 21: je   40 (the game's path)
        0xFF, 0x41, 0x04,                                // 23: inc  dword [rcx+4]
        0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,              // 26: jmp  [rip+0]
        0, 0, 0, 0, 0, 0, 0, 0,                          // 32: skipTo
    };                                                   // 40: (the jump back to the next instruction is appended here)
    const auto address = reinterpret_cast<std::uint64_t>(&gate);
    const auto target  = static_cast<std::uint64_t>(skipTo);
    std::memcpy(&code[10], &address, sizeof(address));
    std::memcpy(&code[32], &target, sizeof(target));
    return code;
}

}  // namespace lss::battle
