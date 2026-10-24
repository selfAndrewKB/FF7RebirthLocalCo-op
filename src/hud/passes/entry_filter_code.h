#pragma once

// The machine code that gives each further seat's HUD entry its own buffers and lets each view family draw only its
// own player's entry (RE-p2-hud-second-image.md §3 and §5; per seat since plan/seats.md step 4).
//
// With the further seats' images, one pool-2 slot holds one layer entry per seat with an image, then player 1's:
// [player 3's, player 2's, player 1's], the further seats highest first (hud/passes/overlay_pass.h). The render
// thread consumes the slot once per view family. On a fresh consume it uploads each entry's vertices and indices into
// the pool's ONE vertex/index buffer set, from offset 0, and draws. On a re-use consume it only draws, from what that
// buffer set last received. So every further entry needs its own buffer set, and a family must skip every other
// player's entry. Two sites in the consume loop do that, each one whole instruction replaced through
// platform/code_redirect.h:
//
// Buffers (0x140FC8501, `mov rax,[rdi+90h]`, loading pool 2's vertex buffer for DrawLayerEntry;
// r9 = the entry, whose FName is its first 8 bytes; the index buffer and the SRM slot are already
// stored at [rsp+28h] and [rsp+30h]): for a further seat's entry, that seat's vertex buffer, index buffer and SRM
// slot are passed instead (the seats' table, walked in order). Only rax, the two argument slots and the flags change;
// nothing after it reads the flags before the call.
//
// Draw gate (0x140FC8E05, `cmp dword [r9+8],0` before `jle` past the draw; rbx = the entry): when a family entry
// name is set and this entry is an overlay entry (player 1's or any seat's) other than the family's own, ZF is forced
// so the stock jle skips the draw. The entry has already uploaded, so a later re-use by another family still finds
// its geometry. rax is dead at this point on both paths.
// The gate is in DrawLayerEntry, which every pool's consume calls (pool 0's at 0x140FC7C9D draws the
// minimap's map into its own target, before the frame's families), so only the overlay entries
// are ever filtered; every other entry is drawn as the game draws it.
//
// The values are read through absolute addresses of 8-byte fields that the mod owns (patched in by
// the builders below), so they can change while the code is installed. Kept apart from the
// installer so the host tests can execute exactly these bytes.

#include "players/seats.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace lss::hud {

/// The further seats with a table entry: seats 1..kMaxPlayers-1, built or not (an unbuilt seat's entry stays unset).
inline constexpr int kFurtherEntries = players::kMaxPlayers - 1;

/// The fields the code reads. Each is an aligned 8-byte value (atomic on x64); a zero name means
/// "not set".
struct EntryFilterFields
{
    struct SeatEntry
    {
        alignas(8) std::uint64_t name = 0;  // the seat's entry FName; set only once its buffers are ready
        alignas(8) std::uint64_t vertexBuffer = 0;  // the seat's buffer set (object pointers)
        alignas(8) std::uint64_t indexBuffer  = 0;
        alignas(8) std::uint64_t srmSlot      = 0;
    };
    SeatEntry seats[kFurtherEntries];          // seat s at [s - 1]
    alignas(8) std::uint64_t familyName = 0;  // the entry this family draws; 0 draws every entry
    alignas(8) std::uint64_t stockName  = 0;  // player 1's (the game's) overlay entry FName

    SeatEntry&       Seat(int seat) noexcept { return seats[seat - 1]; }
    const SeatEntry& Seat(int seat) const noexcept { return seats[seat - 1]; }
};

/// The instruction each site replaces, as the research read it.
inline constexpr std::uint8_t kLoadVertexBufferBytes[] = {0x48, 0x8B, 0x87, 0x90, 0x00, 0x00, 0x00};  // mov rax,[rdi+90h]
// The same load in pool 0's consume (0x140FC7C91, FEndMenuRenderer__OnRenderToTexture_ConsumePool0; r13 = the
// renderer, r9 = the entry, the index buffer and SRM slot already at [rsp+28h]/[rsp+30h]; RE-p2-own-minimap.md §8):
// the minimap's map entries, so each seat's minimap entry gets its own buffers the same way.
inline constexpr std::uint8_t kLoadPoolZeroVertexBufferBytes[] = {0x49, 0x8B, 0x85, 0x80, 0x00, 0x00, 0x00};  // mov rax,[r13+80h]
inline constexpr std::uint8_t kDrawGateBytes[]         = {0x41, 0x83, 0x79, 0x08, 0x00};              // cmp dword [r9+8],0
// Game version 1.005 (RE-1005-hud-2.md): pool 0's load (0x140A460ED) crosses a 16-byte block, so the site is the
// next instruction, the store of the vertex-buffer argument (0x140A460F4, `mov [rsp+20h],rax`, one block; the call of
// DrawLayerEntry follows at 0x140A460F9). rax already holds pool 0's vertex buffer there, r9 the entry.
inline constexpr std::uint8_t kStoreVertexBufferArgBytes1005[] = {0x48, 0x89, 0x44, 0x24, 0x20};  // mov [rsp+20h],rax

// One seat's test and loads, the same length in both buffer builders (74 bytes), then the replaced instruction.
inline constexpr std::size_t kEntryBuffersCodeSize          = 74 * kFurtherEntries + sizeof(kLoadVertexBufferBytes);
inline constexpr std::size_t kEntryBuffersStoreCodeSize1005 =
    74 * kFurtherEntries + sizeof(kStoreVertexBufferArgBytes1005);
// The family test (23 bytes), one name test per overlay entry (23 bytes each: the seats', then player 1's), the
// skip's tail (6 bytes), then the replaced instruction.
inline constexpr std::size_t kEntryDrawGateCodeSize = 23 + 23 * (kFurtherEntries + 1) + 6 + sizeof(kDrawGateBytes);

// Every jump is a short one (rel8, at most 127 bytes forward): the longest are the first seat's jump past the other
// seats to the end, and the gate's first jump past every name test to the replaced instruction.
static_assert(74 * (kFurtherEntries - 1) + sizeof(kLoadVertexBufferBytes) <= 127, "a buffers jump needs rel32");
static_assert(74 * (kFurtherEntries - 1) <= 127, "a 1.005 store jump needs rel32");
static_assert(5 + 23 * (kFurtherEntries + 1) + 6 <= 127, "a draw gate jump needs rel32");

namespace detail {

/// Writes the bytes in order and resolves forward 8-bit jumps.
template <std::size_t Size>
class Emitter
{
public:
    explicit Emitter(std::array<std::uint8_t, Size>& code) noexcept : code_(code) {}

    // Every write stays inside the array: a size constant short of the code leaves the end unwritten, which the host
    // tests see (each builder ends with the instruction it replaces).
    void Bytes(std::initializer_list<std::uint8_t> bytes) noexcept
    {
        for (const std::uint8_t byte : bytes)
        {
            if (at_ < Size)
            {
                code_[at_] = byte;
            }
            ++at_;
        }
    }

    void Bytes(const std::uint8_t* bytes, std::size_t count) noexcept
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            Bytes({bytes[i]});
        }
    }

    /// `mov rax, imm64` (48 B8) or `mov r10, imm64` (49 BA) with a field's address, then the load through it.
    void LoadField(const void* field, bool r10 = false) noexcept
    {
        Bytes({r10 ? std::uint8_t{0x49} : std::uint8_t{0x48}, r10 ? std::uint8_t{0xBA} : std::uint8_t{0xB8}});
        const auto address = reinterpret_cast<std::uint64_t>(field);
        std::uint8_t bytes[sizeof(address)];
        std::memcpy(bytes, &address, sizeof(address));
        Bytes(bytes, sizeof(bytes));
        if (r10)
        {
            Bytes({0x4D, 0x8B, 0x12});  // mov r10, [r10]
        }
        else
        {
            Bytes({0x48, 0x8B, 0x00});  // mov rax, [rax]
        }
    }

    /// A short jump (`opcode` rel8) to a label resolved later; returns the place to resolve.
    std::size_t Jump(std::uint8_t opcode) noexcept
    {
        Bytes({opcode, 0x00});
        return at_;
    }

    /// The jump that ended at `after` lands here (forward, within the rel8 range the static_asserts above hold).
    void Land(std::size_t after) noexcept { code_[after - 1] = static_cast<std::uint8_t>(at_ - after); }

private:
    std::array<std::uint8_t, Size>& code_;
    std::size_t                     at_ = 0;
};

}  // namespace detail

/// `stockLoad` is the site's own 7-byte vertex-buffer load, run for every other entry.
inline std::array<std::uint8_t, kEntryBuffersCodeSize> EntryBuffersCode(
    const EntryFilterFields& fields, const std::uint8_t (&stockLoad)[7] = kLoadVertexBufferBytes) noexcept
{
    std::array<std::uint8_t, kEntryBuffersCodeSize> code{};
    detail::Emitter emit(code);
    std::size_t     toDone[kFurtherEntries] = {};
    for (int i = 0; i < kFurtherEntries; ++i)
    {
        const auto& entry = fields.seats[i];
        emit.LoadField(&entry.name);              // mov rax, &name ; mov rax, [rax]
        emit.Bytes({0x48, 0x85, 0xC0});           // test rax, rax          the seat has no entry yet
        const auto unset = emit.Jump(0x74);       // jz   next
        emit.Bytes({0x49, 0x3B, 0x01});           // cmp  rax, [r9]         this entry's FName
        const auto other = emit.Jump(0x75);       // jne  next
        emit.LoadField(&entry.indexBuffer);
        emit.Bytes({0x48, 0x89, 0x44, 0x24, 0x28});  // mov [rsp+28h], rax   the index buffer argument
        emit.LoadField(&entry.srmSlot);
        emit.Bytes({0x48, 0x89, 0x44, 0x24, 0x30});  // mov [rsp+30h], rax   the SRM slot argument
        emit.LoadField(&entry.vertexBuffer);      // the vertex buffer, in rax as the stock load leaves it
        toDone[i] = emit.Jump(0xEB);              // jmp  done
        emit.Land(unset);
        emit.Land(other);
    }
    emit.Bytes(stockLoad, sizeof(stockLoad));     // stock: the replaced instruction
    for (const std::size_t jump : toDone)
    {
        emit.Land(jump);                          // done (the jump back is appended here)
    }
    return code;
}

/// Game version 1.005, pool 0: the same choice made at the store of the vertex-buffer argument. rax holds the stock
/// vertex buffer and must survive for every other entry, so the code reads its fields through r10, which is volatile
/// and carries no argument of the call that follows (DrawLayerEntry: rcx, rdx, r8, r9 and the stack); the flags are
/// not read before that call either.
inline std::array<std::uint8_t, kEntryBuffersStoreCodeSize1005> EntryBuffersStoreCode1005(
    const EntryFilterFields& fields) noexcept
{
    std::array<std::uint8_t, kEntryBuffersStoreCodeSize1005> code{};
    detail::Emitter emit(code);
    std::size_t     toStore[kFurtherEntries] = {};
    for (int i = 0; i < kFurtherEntries; ++i)
    {
        const auto& entry = fields.seats[i];
        emit.LoadField(&entry.name, true);        // mov r10, &name ; mov r10, [r10]
        emit.Bytes({0x4D, 0x85, 0xD2});           // test r10, r10          the seat has no entry yet
        const auto unset = emit.Jump(0x74);       // jz   next
        emit.Bytes({0x4D, 0x3B, 0x11});           // cmp  r10, [r9]         this entry's FName
        const auto other = emit.Jump(0x75);       // jne  next
        emit.LoadField(&entry.indexBuffer, true);
        emit.Bytes({0x4C, 0x89, 0x54, 0x24, 0x28});  // mov [rsp+28h], r10   the index buffer argument
        emit.LoadField(&entry.srmSlot, true);
        emit.Bytes({0x4C, 0x89, 0x54, 0x24, 0x30});  // mov [rsp+30h], r10   the SRM slot argument
        emit.LoadField(&entry.vertexBuffer);      // mov rax, &vertexBuffer ; mov rax, [rax]   the seat's vertex buffer
        toStore[i] = emit.Jump(0xEB);             // jmp  store
        emit.Land(unset);
        emit.Land(other);
    }
    for (const std::size_t jump : toStore)
    {
        emit.Land(jump);
    }
    // store: the replaced instruction (the jump back is appended)
    emit.Bytes(kStoreVertexBufferArgBytes1005, sizeof(kStoreVertexBufferArgBytes1005));
    return code;
}

inline std::array<std::uint8_t, kEntryDrawGateCodeSize> EntryDrawGateCode(const EntryFilterFields& fields) noexcept
{
    std::array<std::uint8_t, kEntryDrawGateCodeSize> code{};
    detail::Emitter emit(code);
    emit.LoadField(&fields.familyName);           // mov rax, &familyName ; mov rax, [rax]
    emit.Bytes({0x48, 0x85, 0xC0});               // test rax, rax
    const auto noFilter = emit.Jump(0x74);        // jz   stock             no filter: draw as the game does
    emit.Bytes({0x48, 0x3B, 0x03});               // cmp  rax, [rbx]        this entry's FName
    const auto own = emit.Jump(0x74);             // je   stock             this family's entry
    std::size_t toSkip[kFurtherEntries + 1] = {};
    for (int i = 0; i <= kFurtherEntries; ++i)    // every seat's entry name, then player 1's
    {
        emit.LoadField(i < kFurtherEntries ? &fields.seats[i].name : &fields.stockName);
        emit.Bytes({0x48, 0x85, 0xC0});           // test rax, rax          no such entry
        const auto unset = emit.Jump(0x74);       // jz   next
        emit.Bytes({0x48, 0x3B, 0x03});           // cmp  rax, [rbx]
        toSkip[i] = emit.Jump(0x74);              // je   skip              another player's overlay entry
        emit.Land(unset);
    }
    const auto notOverlay = emit.Jump(0xEB);      // jmp  stock             not an overlay entry (the minimap's map)
    for (const std::size_t jump : toSkip)
    {
        emit.Land(jump);
    }
    emit.Bytes({0x31, 0xC0});                     // skip: xor eax, eax     ZF=1, SF=OF=0: the stock jle skips the draw
    const auto skipped = emit.Jump(0xEB);         // jmp  done
    emit.Land(noFilter);
    emit.Land(own);
    emit.Land(notOverlay);
    emit.Bytes(kDrawGateBytes, sizeof(kDrawGateBytes));  // stock: the replaced instruction
    emit.Land(skipped);                                  // done (the jump back is appended here)
    return code;
}

}  // namespace lss::hud
