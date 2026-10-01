#pragma once

// The machine code that gives player 2's HUD entry its own buffers and lets each view family draw
// only its own player's entry (RE-p2-hud-second-image.md §3 and §5).
//
// With player 2's image, one pool-2 slot holds two layer entries, [player 2's, player 1's]. The
// render thread consumes the slot once per view family. On a fresh consume it uploads each entry's
// vertices and indices into the pool's ONE vertex/index buffer set, from offset 0, and draws. On a
// re-use consume it only draws, from what that buffer set last received. So a second entry needs
// its own buffer set, and a family must skip the other player's entry. Two sites in the consume
// loop do that, each one whole instruction replaced through platform/code_redirect.h:
//
// Buffers (0x140FC8501, `mov rax,[rdi+90h]`, loading pool 2's vertex buffer for DrawLayerEntry;
// r9 = the entry, whose FName is its first 8 bytes; the index buffer and the SRM slot are already
// stored at [rsp+28h] and [rsp+30h]): for player 2's entry, player 2's vertex buffer, index buffer
// and SRM slot are passed instead. Only rax, the two argument slots and the flags change; nothing
// after it reads the flags before the call.
//
// Draw gate (0x140FC8E05, `cmp dword [r9+8],0` before `jle` past the draw; rbx = the entry): when a
// family entry name is set and this entry is the OTHER player's overlay entry, ZF is forced so the
// stock jle skips the draw. The entry has already uploaded, so a later re-use by the other family
// still finds its geometry. rax is dead at this point on both paths.
// The gate is in DrawLayerEntry, which every pool's consume calls (pool 0's at 0x140FC7C9D draws the
// minimap's map into its own target, before the frame's families), so only the two overlay entries
// are ever filtered; every other entry is drawn as the game draws it.
//
// The values are read through absolute addresses of 8-byte fields that the mod owns (patched in by
// the builders below), so they can change while the code is installed. Kept apart from the
// installer so the host tests can execute exactly these bytes.

#include <array>
#include <cstdint>
#include <cstring>

namespace lss::hud {

/// The fields the code reads. Each is an aligned 8-byte value (atomic on x64); a zero name means
/// "not set".
struct EntryFilterFields
{
    alignas(8) std::uint64_t playerTwoName = 0;  // player 2's entry FName; set only once its buffers are ready
    alignas(8) std::uint64_t vertexBuffer  = 0;  // player 2's buffer set (object pointers)
    alignas(8) std::uint64_t indexBuffer   = 0;
    alignas(8) std::uint64_t srmSlot       = 0;
    alignas(8) std::uint64_t familyName    = 0;  // the entry this family draws; 0 draws every entry
    alignas(8) std::uint64_t stockName     = 0;  // player 1's (the game's) overlay entry FName
};

inline constexpr std::size_t kEntryBuffersCodeSize = 81;
inline constexpr std::size_t kEntryDrawGateCodeSize = 68;

/// The instruction each site replaces, as the research read it.
inline constexpr std::uint8_t kLoadVertexBufferBytes[] = {0x48, 0x8B, 0x87, 0x90, 0x00, 0x00, 0x00};  // mov rax,[rdi+90h]
inline constexpr std::uint8_t kDrawGateBytes[]         = {0x41, 0x83, 0x79, 0x08, 0x00};              // cmp dword [r9+8],0

namespace detail {
inline void PutAddress(std::uint8_t* at, const void* field) noexcept
{
    const auto address = reinterpret_cast<std::uint64_t>(field);
    std::memcpy(at, &address, sizeof(address));
}
}  // namespace detail

inline std::array<std::uint8_t, kEntryBuffersCodeSize> EntryBuffersCode(const EntryFilterFields& fields) noexcept
{
    std::array<std::uint8_t, kEntryBuffersCodeSize> code = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,        //  0: mov  rax, &playerTwoName
        0x48, 0x8B, 0x00,                          // 10: mov  rax, [rax]
        0x48, 0x85, 0xC0,                          // 13: test rax, rax
        0x74, 0x38,                                // 16: jz   stock               no player-2 entry yet
        0x49, 0x3B, 0x01,                          // 18: cmp  rax, [r9]           this entry's FName
        0x75, 0x33,                                // 21: jne  stock
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,        // 23: mov  rax, &indexBuffer
        0x48, 0x8B, 0x00,                          // 33: mov  rax, [rax]
        0x48, 0x89, 0x44, 0x24, 0x28,              // 36: mov  [rsp+28h], rax      the index buffer argument
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,        // 41: mov  rax, &srmSlot
        0x48, 0x8B, 0x00,                          // 51: mov  rax, [rax]
        0x48, 0x89, 0x44, 0x24, 0x30,              // 54: mov  [rsp+30h], rax      the SRM slot argument
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,        // 59: mov  rax, &vertexBuffer
        0x48, 0x8B, 0x00,                          // 69: mov  rax, [rax]          the vertex buffer, as the stock load
        0xEB, 0x07,                                // 72: jmp  done
        0x48, 0x8B, 0x87, 0x90, 0x00, 0x00, 0x00,  // 74: stock: mov rax, [rdi+90h] (the replaced instruction)
    };                                             // 81: done (the jump back is appended here)
    detail::PutAddress(&code[2], &fields.playerTwoName);
    detail::PutAddress(&code[25], &fields.indexBuffer);
    detail::PutAddress(&code[43], &fields.srmSlot);
    detail::PutAddress(&code[61], &fields.vertexBuffer);
    return code;
}

inline std::array<std::uint8_t, kEntryDrawGateCodeSize> EntryDrawGateCode(const EntryFilterFields& fields) noexcept
{
    std::array<std::uint8_t, kEntryDrawGateCodeSize> code = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,  //  0: mov  rax, &familyName
        0x48, 0x8B, 0x00,                    // 10: mov  rax, [rax]
        0x48, 0x85, 0xC0,                    // 13: test rax, rax
        0x74, 0x2D,                          // 16: jz   stock          no filter: draw as the game does
        0x48, 0x3B, 0x03,                    // 18: cmp  rax, [rbx]     this entry's FName
        0x74, 0x28,                          // 21: je   stock          this family's entry
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,  // 23: mov  rax, &playerTwoName
        0x48, 0x8B, 0x00,                    // 33: mov  rax, [rax]
        0x48, 0x3B, 0x03,                    // 36: cmp  rax, [rbx]
        0x74, 0x12,                          // 39: je   skip           player 2's entry, player 1's family
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,  // 41: mov  rax, &stockName
        0x48, 0x8B, 0x00,                    // 51: mov  rax, [rax]
        0x48, 0x3B, 0x03,                    // 54: cmp  rax, [rbx]
        0x75, 0x04,                          // 57: jne  stock          not an overlay entry (the minimap's map, ...)
        0x31, 0xC0,                          // 59: skip: xor eax, eax  ZF=1, SF=OF=0: the stock jle skips the draw
        0xEB, 0x05,                          // 61: jmp  done
        0x41, 0x83, 0x79, 0x08, 0x00,        // 63: stock: cmp dword [r9+8], 0 (the replaced instruction)
    };                                       // 68: done (the jump back is appended here)
    detail::PutAddress(&code[2], &fields.familyName);
    detail::PutAddress(&code[25], &fields.playerTwoName);
    detail::PutAddress(&code[43], &fields.stockName);
    return code;
}

}  // namespace lss::hud
