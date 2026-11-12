#pragma once

// The bytes feedback/traversal_rumble_sources.cpp places at the 1.004 jump box's inline list load
// (PlaceJumpListLoad): after the game's own load, on a thread a jump route named, the rumble list the route named with
// it (the seat's whose chocobo landed) replaces the leader's. Two routes, one per seat: two chocobos' landings
// run on different animation workers at once (research/RE-seats-step6-camera-rumble.md), so each landing names its own slot.
// Pure data, so it is executed on the host (tests/traversal_rumble_code_test.cpp).

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lss::feedback::traversal_rumble_code {

// 1.004 only: the jump box's add is inlined there; this is its list load, under the game's lock (1.005 calls PlayNamed).
inline constexpr std::uintptr_t kJumpListLoad        = 0x1422AD981;
inline constexpr std::uint8_t   kJumpListLoadBytes[] = {0x48, 0x8B, 0xB6, 0xB0, 0x03, 0x00, 0x00};  // mov rsi,[rsi+3B0h]
inline constexpr std::size_t    kJumpListLoadCodeSize = 51;

/// One landing's route: the thread whose inline add takes `list` (0: the slot is free; no thread's id is 0), and the
/// list (null: none). The code reads both as plain words at these offsets.
struct JumpSlot
{
    std::atomic<std::uint32_t> thread{0};
    std::atomic<void*>         list{nullptr};
};
static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t) &&
                  sizeof(std::atomic<void*>) == sizeof(void*) && std::atomic<void*>::is_always_lock_free,
              "the code reads the slots' atomics as plain words");
static_assert(sizeof(JumpSlot) == 16 && offsetof(JumpSlot, list) == 8, "the code's slot layout");

/// The routes: one per seat (players::kMaxPlayers - 1; the code below walks exactly this many).
inline constexpr std::size_t kJumpSlots = 2;
using JumpSlots                         = std::array<JumpSlot, kJumpSlots>;

/// The game's load, then: if this thread's id (the TEB's, at gs:[48h]) is a slot's 32-bit id, and that slot's list is
/// set, rsi becomes that list. No call, no stack use; rax, rcx and the flags are dead at the site.
inline std::array<std::uint8_t, kJumpListLoadCodeSize> JumpListLoadCode(const JumpSlots* slots) noexcept
{
    std::array<std::uint8_t, kJumpListLoadCodeSize> code{
        0x48, 0x8B, 0xB6, 0xB0, 0x03, 0x00, 0x00,        //  0 mov rsi,[rsi+3B0h]  (the game's load)
        0x65, 0x8B, 0x04, 0x25, 0x48, 0x00, 0x00, 0x00,  //  7 mov eax,gs:[48h]    (this thread's id)
        0x48, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0,              // 15 mov rcx,&slots
        0x3B, 0x01,                                      // 25 cmp eax,[rcx]       (slot 0's thread)
        0x75, 0x06,                                      // 27 jne second
        0x48, 0x8B, 0x49, 0x08,                          // 29 mov rcx,[rcx+8]     (slot 0's list)
        0xEB, 0x09,                                      // 33 jmp take
        0x3B, 0x41, 0x10,                                // 35 second: cmp eax,[rcx+10h]  (slot 1's thread)
        0x75, 0x0B,                                      // 38 jne done
        0x48, 0x8B, 0x49, 0x18,                          // 40 mov rcx,[rcx+18h]   (slot 1's list)
        0x48, 0x85, 0xC9,                                // 44 take: test rcx,rcx
        0x48, 0x0F, 0x45, 0xF1,                          // 47 cmovnz rsi,rcx
    };                                                   // 51 done: (the jump back follows)
    static_assert(kJumpSlots == 2, "the code walks two slots");
    const auto address = reinterpret_cast<std::uint64_t>(slots);
    std::memcpy(code.data() + 17, &address, sizeof(address));
    return code;
}

}  // namespace lss::feedback::traversal_rumble_code
