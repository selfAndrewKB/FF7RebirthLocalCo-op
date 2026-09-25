#pragma once

// The machine code that records, as the game's enemy-status tick runs, each untargeted bar's
// placement call (RE-p2-enemy-markers.md §7.4 and §7.7 A.1).
//
// EndUI__MF8_ProjectEntryByWidget(mgr, widget, const FVector* loc) (0x14176201C) places one bar from
// the location the tick computed this frame. Its call site crosses a 16-byte block, and so does its
// first instruction, so the second instruction is replaced: `mov [rsp+18h], r8` (5 bytes at
// 0x141762021, inside one block). The code appends {widget, *loc} to a record in this DLL, unless
// the record is full or the mod is replaying the bars itself (its own calls are not the game's),
// then runs the replaced instruction; the function continues as the game's.
//
// Only rax, r10, r11 and the flags change: none carries an argument at a function's entry, and the
// function's own first instruction has already run. No calls, no stack use. Kept apart from the
// installer so the host tests can execute exactly these bytes.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lss::hud {

/// One recorded placement, laid out as the code writes it.
struct BarPlacement
{
    std::uint64_t widget = 0;
    float         location[3]{};
    float         halfSize = 0.0f;  // targeted only
    std::uint8_t  targeted = 0;     // 0 from the code; the targeted call's redirect writes 1
    std::uint8_t  skipLeaderClamp = 0;
    std::uint8_t  pad[6]{};
};
static_assert(sizeof(BarPlacement) == 32 && offsetof(BarPlacement, location) == 8 &&
              offsetof(BarPlacement, targeted) == 24);

inline constexpr std::uint32_t kBarRecordCapacity = 48;  // the pool holds 16 bars; each is placed once per tick

/// The record the code appends to. `replaying` is set while the mod calls the function itself.
struct alignas(16) BarRecord
{
    std::uint32_t count     = 0;
    std::uint8_t  replaying = 0;
    std::uint8_t  pad[11]{};
    BarPlacement  placements[kBarRecordCapacity];
};
static_assert(offsetof(BarRecord, replaying) == 4 && offsetof(BarRecord, placements) == 16);

inline constexpr std::size_t kBarCodeSize = 67;

/// The instruction replaced at 0x141762021, as the research read it.
inline constexpr std::uint8_t kBarEntryBytes[] = {0x4C, 0x89, 0x44, 0x24, 0x18};  // mov [rsp+18h], r8

inline std::array<std::uint8_t, kBarCodeSize> BarCaptureCode(BarRecord& record) noexcept
{
    std::array<std::uint8_t, kBarCodeSize> code = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,   //  0: mov  rax, &record
        0x80, 0x78, 0x04, 0x00,               // 10: cmp  byte [rax+4], 0     replaying: not the game's call
        0x75, 0x2E,                           // 14: jne  done (62)
        0x44, 0x8B, 0x10,                     // 16: mov  r10d, [rax]         count
        0x41, 0x83, 0xFA, kBarRecordCapacity, // 19: cmp  r10d, capacity
        0x73, 0x25,                           // 23: jae  done (62)
        0x45, 0x8D, 0x5A, 0x01,               // 25: lea  r11d, [r10+1]
        0x44, 0x89, 0x18,                     // 29: mov  [rax], r11d         count + 1
        0x49, 0xC1, 0xE2, 0x05,               // 32: shl  r10, 5              * sizeof(BarPlacement)
        0x4C, 0x01, 0xD0,                     // 36: add  rax, r10            (placements start at +10h)
        0x48, 0x89, 0x50, 0x10,               // 39: mov  [rax+10h], rdx      widget
        0x4D, 0x8B, 0x18,                     // 43: mov  r11, [r8]           loc x, y
        0x4C, 0x89, 0x58, 0x18,               // 46: mov  [rax+18h], r11
        0x45, 0x8B, 0x58, 0x08,               // 50: mov  r11d, [r8+8]        loc z
        0x44, 0x89, 0x58, 0x20,               // 54: mov  [rax+20h], r11d
        0xC6, 0x40, 0x28, 0x00,               // 58: mov  byte [rax+28h], 0   untargeted
        0x4C, 0x89, 0x44, 0x24, 0x18,         // 62: done: mov [rsp+18h], r8 (the replaced instruction)
    };                                        // 67: (the jump back to the next instruction is appended here)
    const auto address = reinterpret_cast<std::uint64_t>(&record);
    std::memcpy(&code[2], &address, sizeof(address));
    return code;
}

}  // namespace lss::hud
