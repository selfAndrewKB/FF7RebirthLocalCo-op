#pragma once

// The machine code that answers "no" for one member in FieldCommandCtrl_IsMemberCommandable
// (RE-p2-command-menu.md §4.2, §4.4).
//
// The test's signature is bool(ctrl* rcx, int ordinal edx, u8 flag r8b), and its five callers are all
// in the command menu; one of their call sites straddles a code block, so the test itself is gated,
// at its first instruction: `mov [rsp+8], rbx` (5 bytes at 0x140E77414, inside one block). The code
// tests the ordinal asked about against a set in this DLL (CommandGateMaskCode, installed since plan/seats.md step 6:
// every refused member's bit); on a match it returns 0 (the game's own answer for a member that cannot take a
// command), else it runs the replaced instruction and the test continues as the game's. CommandGateCode, the
// two-player gate that compared one ordinal, is kept for the host tests' side-by-side only.
//
// Only rax and the flags change before the replaced instruction: rax carries no argument at a
// function's entry. No calls, no stack use. Kept apart from the installer so the host tests can
// execute exactly these bytes.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lss::player_two {

inline constexpr std::size_t kCommandGateCodeSize = 22;

/// The instruction replaced at 0x140E77414, as the research read it.
inline constexpr std::uint8_t kCommandGateEntryBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08};  // mov [rsp+8], rbx

inline std::array<std::uint8_t, kCommandGateCodeSize> CommandGateCode(const std::int32_t& gate) noexcept
{
    std::array<std::uint8_t, kCommandGateCodeSize> code = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,  //  0: mov  rax, &gate
        0x3B, 0x10,                          // 10: cmp  edx, [rax]
        0x75, 0x03,                          // 12: jne  17
        0x31, 0xC0,                          // 14: xor  eax, eax     not commandable
        0xC3,                                // 16: ret
        0x48, 0x89, 0x5C, 0x24, 0x08,        // 17: mov  [rsp+8], rbx (the replaced instruction)
    };                                       // 22: (the jump back to the next instruction is appended here)
    const auto address = reinterpret_cast<std::uint64_t>(&gate);
    std::memcpy(&code[2], &address, sizeof(address));
    return code;
}

/// The same gate over a set, in the same 22 bytes (installed since the plan's step 6): `mask` holds one bit per refused
/// ordinal (bit n: ordinal n; 0 refuses none), so player 1 can refuse every seat's member and a seat player 1's leader
/// and the other seats' members. `bt eax, edx` takes the ordinal modulo 32; the five callers pass 0 to the party count
/// less one (at most 16), or -1 for none, which reads bit 31, never set [CONFIRMED, RE-seats-step6-battle.md §2]. Only
/// rax and the flags change before the replaced instruction, as above.
inline std::array<std::uint8_t, kCommandGateCodeSize> CommandGateMaskCode(const std::uint32_t& mask) noexcept
{
    std::array<std::uint8_t, kCommandGateCodeSize> code = {
        0xA1, 0, 0, 0, 0, 0, 0, 0, 0,  //  0: mov  eax, [&mask]  (moffs64)
        0x0F, 0xA3, 0xD0,              //  9: bt   eax, edx       CF = bit (ordinal mod 32)
        0x73, 0x03,                    // 12: jnc  17
        0x31, 0xC0,                    // 14: xor  eax, eax       not commandable
        0xC3,                          // 16: ret
        0x48, 0x89, 0x5C, 0x24, 0x08,  // 17: mov  [rsp+8], rbx (the replaced instruction)
    };                                 // 22: (the jump back to the next instruction is appended here)
    const auto address = reinterpret_cast<std::uint64_t>(&mask);
    std::memcpy(&code[1], &address, sizeof(address));
    return code;
}

}  // namespace lss::player_two
