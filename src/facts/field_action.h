#pragma once

// The FieldActionMgr ([g_EndGameRoot]+0x1A0): the point registry's live list and lock, the eight slots' rows, targets
// and latches, the pending flag, and a point's key, preset and flags. Read by world/field_pickup (player 2's own
// pass over the registry) and world/interact_watch (its hooks and log). Each offset was read in IDA at an
// instruction that uses it.
//
// Research: Rebirth/Documentation/research/RE-p2-field-interact.md §1.2, §1.3, §10.B, §10.D, §12.

#include <cstdint>

namespace lss::facts {

// The registry: 1024 pool nodes of 288 bytes; the live list's head node (node+0x08 next, the point at node+0x18) under
// an SRWLOCK the game holds while it walks.
inline constexpr std::uint32_t kFieldActionPointList = 0x48320;
inline constexpr std::uint32_t kFieldActionNodeNext  = 0x08;
inline constexpr std::uint32_t kFieldActionNodePoint = 0x18;
inline constexpr std::uint32_t kFieldActionPointLock = 0x487F0;
inline constexpr int           kFieldActionSlots     = 8;
inline constexpr int           kFieldActionMaxPoints = 1024;

// The eight slots: their HUD rows (FWeakObjectPtr {index, serial}), this frame's target keys (FName), and the held,
// pressed and executed latches (u8 each, three runs of eight); the pending flag.
inline constexpr std::uint32_t kFieldActionSlotRows     = 0x483A8;
inline constexpr std::uint32_t kFieldActionSlotTargets  = 0x48608;
inline constexpr std::uint32_t kFieldActionSlotHeld     = 0x48730;
inline constexpr std::uint32_t kFieldActionSlotPressed  = 0x48738;
inline constexpr std::uint32_t kFieldActionSlotExecuted = 0x48740;
inline constexpr std::uint32_t kFieldActionPending      = 0x487D0;  // u8

// A point: its key (FName), the point-side range preset (FName rpa<Owner>), and its flags (bit 1 slot target, bit 2
// runner-up, bit 4 skipped by the query, bit 0x10 disabled).
inline constexpr std::uint32_t kFieldActionPointKey    = 0x0C;
inline constexpr std::uint32_t kFieldActionPointPreset = 0x44;
inline constexpr std::uint32_t kFieldActionPointFlags  = 0x104;

// A map element's entry, as AcquireEntry hands it out: element + 0x10 (the FName key is 0x10 bytes before the entry).
inline constexpr std::uint32_t kFieldActionEntryKeyBack = 0x10;

}  // namespace lss::facts
