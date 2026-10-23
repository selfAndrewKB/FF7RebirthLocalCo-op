#pragma once

// The machine code that records, as the game places them, each battle popup's placement call
// (damage numbers, battle texts, UMG popups; RE-p2-battle-popups.md §1.2 and §1.3).
//
// The popup manager (EndUISystem+0xA0) places every popup through one of two wrappers:
//   EndUI__MA0_PlaceSlatePopup_SetVisibility (0x140F5F708) and EndUI__MA0_PlaceUMGPopup_SetVisibility
//   (0x143A41704), both (rcx &out, rdx PC, r8 &canvas, r9 widget key, [rsp+28h] const FVector* loc,
//   [rsp+30h] FVector2D offset, [rsp+38h] u8 clamp).
// The PC is the one stored in the popup at spawn (the world's first player controller), so the
// projection is player 1's in both halves. Each wrapper's first instruction, `mov [rsp+10h], rbx`
// (5 bytes inside one block), is replaced: the code appends {key, PC, *loc, offset, clamp} to a record
// in this DLL (the key: the SWidget* at [r9] for the Slate wrapper, the UUserWidget* in r9 for the UMG
// one), unless the record is full or the mod is replaying, then runs the replaced instruction
// (hud/battle/placement_record.h: the head, the scale and the replaced instruction; the payload here).
//
// Only rax, r10, r11 and the flags change: none carries an argument at a function's entry. No
// calls, no stack writes. Kept apart from the installer so the host tests can execute these bytes.

#include "hud/battle/placement_record.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace lss::hud {

/// One recorded placement, laid out as the code writes it.
struct PopupPlacement
{
    std::uint64_t key = 0;
    std::uint64_t controller = 0;
    float         location[3]{};
    float         offset[2]{};
    std::uint8_t  clamp = 0;
    std::uint8_t  pad[11]{};
};
static_assert(sizeof(PopupPlacement) == 48 && offsetof(PopupPlacement, location) == 0x10 &&
              offsetof(PopupPlacement, offset) == 0x1C && offsetof(PopupPlacement, clamp) == 0x24);

inline constexpr std::uint32_t kPopupRecordCapacity = 96;  // a busy fight shows a few dozen popups

using PopupRecord = PlacementRecord<PopupPlacement, kPopupRecordCapacity>;
static_assert(offsetof(PopupRecord, replaying) == 4 && offsetof(PopupRecord, placements) == 16);

/// The instruction replaced at both wrappers' entries.
inline constexpr std::uint8_t kPopupEntryBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10};  // mov [rsp+10h], rbx

inline constexpr std::size_t kPopupPayloadSize = 49;
inline constexpr std::size_t kPopupCodeSize = kRecordHeadSize + kRecordScaleSize + kRecordAddSize + kPopupPayloadSize +
                                              sizeof(kPopupEntryBytes);  // 93

enum class PopupKey
{
    Slate,  // r9 = &TSharedPtr<SWidget>: the key is [r9]
    Umg,    // r9 = UUserWidget*: the key is r9
};

inline std::array<std::uint8_t, kPopupCodeSize> PopupCaptureCode(PopupRecord& record, PopupKey key) noexcept
{
    // After the head (rax = &placements[count], count + 1 stored): the call's arguments into the placement.
    std::uint8_t payload[kPopupPayloadSize] = {
        0x4D, 0x8B, 0x19,                       // 39: mov  r11, [r9]            the key (Umg: mov r11, r9)
        0x4C, 0x89, 0x58, 0x10,                 // 42: mov  [rax+10h], r11
        0x48, 0x89, 0x50, 0x18,                 // 46: mov  [rax+18h], rdx       PC
        0x4C, 0x8B, 0x5C, 0x24, 0x28,           // 50: mov  r11, [rsp+28h]       &loc
        0x4D, 0x8B, 0x13,                       // 55: mov  r10, [r11]           loc x, y
        0x4C, 0x89, 0x50, 0x20,                 // 58: mov  [rax+20h], r10
        0x45, 0x8B, 0x53, 0x08,                 // 62: mov  r10d, [r11+8]        loc z
        0x44, 0x89, 0x50, 0x28,                 // 66: mov  [rax+28h], r10d
        0x4C, 0x8B, 0x5C, 0x24, 0x30,           // 70: mov  r11, [rsp+30h]       offset
        0x4C, 0x89, 0x58, 0x2C,                 // 75: mov  [rax+2Ch], r11
        0x44, 0x8A, 0x5C, 0x24, 0x38,           // 79: mov  r11b, [rsp+38h]      clamp
        0x44, 0x88, 0x58, 0x34,                 // 84: mov  [rax+34h], r11b
    };                                          // 88: done
    if (key == PopupKey::Umg)
    {
        payload[0] = 0x4D;  // mov r11, r9
        payload[1] = 0x89;
        payload[2] = 0xCB;
    }
    constexpr std::array<std::uint8_t, kRecordScaleSize> kScale = {0x4D, 0x6B, 0xD2, 0x30};  // imul r10, r10, 30h
    return RecordAppendCode(&record, kPopupRecordCapacity, kScale, payload, kPopupEntryBytes);
}

}  // namespace lss::hud
