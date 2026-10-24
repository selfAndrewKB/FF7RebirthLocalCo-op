#pragma once

// The bytes player_two/feedback/rumble_sources_two.cpp places at the 1.004 jump box's inline list load
// (PlaceJumpListLoad): after the game's own load, on the thread the jump route named, the rumble list the route named
// with it (the seat's whose chocobo landed) replaces the leader's. The code reads one thread id and one list pointer,
// so it serves every seat unchanged: the route names one seat's at a time. Pure data, so it is executed on the host
// (tests/rumble_list_code_test.cpp).

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lss::player_two::rumble_list_code {

// 1.004 only: the jump box's add is inlined there; this is its list load, under the game's lock (1.005 calls PlayNamed).
inline constexpr std::uintptr_t kJumpListLoad        = 0x1422AD981;
inline constexpr std::uint8_t   kJumpListLoadBytes[] = {0x48, 0x8B, 0xB6, 0xB0, 0x03, 0x00, 0x00};  // mov rsi,[rsi+3B0h]
inline constexpr std::size_t    kJumpListLoadCodeSize = 49;

/// The game's load, then: if this thread's id (the TEB's, at gs:[48h]) is the 32-bit id at `thread`, and the pointer at
/// `list` is set, rsi becomes that pointer. No call, no stack use; rax, rcx and the flags are dead at the site. The
/// addresses are read as plain memory: the mod's atomics are plain words.
inline std::array<std::uint8_t, kJumpListLoadCodeSize> JumpListLoadCode(const void* thread, const void* list) noexcept
{
    std::array<std::uint8_t, kJumpListLoadCodeSize> code{
        0x48, 0x8B, 0xB6, 0xB0, 0x03, 0x00, 0x00,        // mov rsi,[rsi+3B0h]  (the game's load)
        0x65, 0x8B, 0x04, 0x25, 0x48, 0x00, 0x00, 0x00,  // mov eax,gs:[48h]    (this thread's id)
        0x48, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0,              // mov rcx,&thread
        0x3B, 0x01,                                      // cmp eax,[rcx]
        0x75, 0x14,                                      // jne done
        0x48, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0,              // mov rcx,&list
        0x48, 0x8B, 0x09,                                // mov rcx,[rcx]
        0x48, 0x85, 0xC9,                                // test rcx,rcx
        0x48, 0x0F, 0x45, 0xF1,                          // cmovnz rsi,rcx
    };                                                   // done: (the jump back follows)
    const auto threadAddress = reinterpret_cast<std::uint64_t>(thread);
    const auto listAddress   = reinterpret_cast<std::uint64_t>(list);
    std::memcpy(code.data() + 17, &threadAddress, sizeof(threadAddress));
    std::memcpy(code.data() + 31, &listAddress, sizeof(listAddress));
    return code;
}

}  // namespace lss::player_two::rumble_list_code
