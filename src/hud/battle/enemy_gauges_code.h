#pragma once

// The machine code that records, as the game's enemy-status tick runs, each untargeted gauge's
// placement call (research/RE-p2-enemy-markers.md).
//
// EndUI__MF8_ProjectEntryByWidget(mgr, widget, const FVector* loc) places one gauge from
// the location the tick computed this frame. Its call site crosses a 16-byte block, and so does its
// first instruction, so the second instruction is replaced: `mov [rsp+18h], r8` (5 bytes, inside
// one block). The code appends {widget, *loc} to a record in this DLL, unless
// the record is full or the mod is replaying the gauges itself (its own calls are not the game's),
// then runs the replaced instruction; the function continues as the game's (hud/battle/placement_record.h:
// the head, the scale and the replaced instruction; the payload here).
//
// Only rax, r10, r11 and the flags change: none carries an argument at a function's entry, and the
// function's own first instruction has already run. No calls, no stack use. Kept apart from the
// installer so the host tests can execute exactly these bytes.

#include "hud/battle/placement_record.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace lss::hud {

/// One recorded placement, laid out as the code writes it.
struct GaugePlacement
{
    std::uint64_t widget = 0;
    float         location[3]{};
    float         halfSize = 0.0f;  // targeted only
    std::uint8_t  targeted = 0;     // 0 from the code; the targeted call's redirect writes 1
    std::uint8_t  skipLeaderClamp = 0;
    std::uint8_t  pad[6]{};
};
static_assert(sizeof(GaugePlacement) == 32 && offsetof(GaugePlacement, location) == 8 &&
              offsetof(GaugePlacement, targeted) == 24);

inline constexpr std::uint32_t kGaugeRecordCapacity = 48;  // the pool holds 16 gauges; each is placed once per tick

/// The record the code appends to. `replaying` is set while the mod calls the function itself.
using GaugeRecord = PlacementRecord<GaugePlacement, kGaugeRecordCapacity>;
static_assert(offsetof(GaugeRecord, replaying) == 4 && offsetof(GaugeRecord, placements) == 16);

/// The instruction replaced at the site, as the research read it.
inline constexpr std::uint8_t kGaugeEntryBytes[] = {0x4C, 0x89, 0x44, 0x24, 0x18};  // mov [rsp+18h], r8

inline constexpr std::size_t kGaugePayloadSize = 23;
inline constexpr std::size_t kGaugeCodeSize = kRecordHeadSize + kRecordScaleSize + kRecordAddSize + kGaugePayloadSize +
                                              sizeof(kGaugeEntryBytes);  // 67

inline std::array<std::uint8_t, kGaugeCodeSize> GaugeCaptureCode(GaugeRecord& record) noexcept
{
    // After the head (rax = &placements[count], count + 1 stored): the call's arguments into the placement.
    constexpr std::uint8_t kPayload[kGaugePayloadSize] = {
        0x48, 0x89, 0x50, 0x10,               // 39: mov  [rax+10h], rdx      widget
        0x4D, 0x8B, 0x18,                     // 43: mov  r11, [r8]           loc x, y
        0x4C, 0x89, 0x58, 0x18,               // 46: mov  [rax+18h], r11
        0x45, 0x8B, 0x58, 0x08,               // 50: mov  r11d, [r8+8]        loc z
        0x44, 0x89, 0x58, 0x20,               // 54: mov  [rax+20h], r11d
        0xC6, 0x40, 0x28, 0x00,               // 58: mov  byte [rax+28h], 0   untargeted
    };                                        // 62: done
    constexpr std::array<std::uint8_t, kRecordScaleSize> kScale = {0x49, 0xC1, 0xE2, 0x05};  // shl r10, 5
    return RecordAppendCode(&record, kGaugeRecordCapacity, kScale, kPayload, kGaugeEntryBytes);
}

}  // namespace lss::hud
