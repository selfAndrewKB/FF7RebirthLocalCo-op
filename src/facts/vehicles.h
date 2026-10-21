#pragma once

// The rides: the chocobo's and the SB scooter's move states (their slots and the chocobo's prompt rows).
//
// Addresses are as IDA shows them (image base 0x140000000). Each function's entry bytes are the first 24 bytes read in
// IDA; call and RIP-relative displacements are relative to the image, so they are identical in the running game. A
// stage is installed only if every fact it uses holds (platform/verified_code.h). Each offset was read at an
// instruction that uses it.
//
// Research: Rebirth/Documentation/research/RE-p2-chocobo-ride.md §1.1, §1.2, RE-p2-chocobo-rows.md §2,
// RE-p2-chocobo-run-refresh.md §2.1, §4.1, RE-p2-wheelie-ride.md §14.5.

#include "platform/verified_code.h"

#include <cstdint>

namespace lss::facts {

using platform::FunctionFact;
using platform::SlotFact;

// The chocobo move state (vtable 0x1466B9E18)
// The chocobo move state's own prompt handles (FieldActionIconHandle, 0x50 bytes: +0 the row's weak widget, +0x48 the
// request byte): Dash, Dismount, Jet, Scent, Scour (RE-p2-chocobo-rows.md §2).
inline constexpr std::uint32_t kChocoboMoveStateRowHandles[] = {0x200, 0x250, 0x2A0, 0x2F0, 0x340};
// The chocobo move state's slots 11 and 16 (vtable 0x1466B9E18; RE-p2-chocobo-ride.md §1.1, §1.2): the field
// tick runs the ridden chocobo's slot 16 after player 1's leader-action tick. Entry bytes read from the image.
inline constexpr std::uint8_t kChocoboMoveStateTickBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x70, 0xFF, 0xFF, 0xFF};
inline constexpr SlotFact kChocoboMoveStateTick{
    L"the chocobo move state's tick slot (11)", 0x1466B9E70,
    {L"ChocoboMoveState_Slot11", 0x143451320, kChocoboMoveStateTickBytes}};
inline constexpr std::uint8_t kChocoboMoveStateUpdateInputBytes[] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x48,
    0x89, 0x78, 0x18, 0x4C, 0x89, 0x70, 0x20, 0x55, 0x48, 0x8B, 0xEC, 0xB8};
inline constexpr SlotFact kChocoboMoveStateUpdateInput{
    L"the chocobo move state's input-update slot (16)", 0x1466B9E98,
    {L"ChocoboMoveState_UpdateInput_Slot16", 0x143450444, kChocoboMoveStateUpdateInputBytes}};
// Its slot 1 (RE-p2-chocobo-run-refresh.md §2.1, §4.1): the chocobo's own actor tick runs it each frame, game context;
// it holds the anim sync that rewrites the gait, speed and run timer of any chocobo not the primary PC's ridden one.
inline constexpr std::uint8_t kChocoboMoveStateHandlesBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0xB8,
    0x40, 0x00, 0x00, 0x00, 0xE8, 0x03, 0xFD, 0xC5, 0xFE, 0x48, 0x2B, 0xE0};
inline constexpr SlotFact kChocoboMoveStateHandles{
    L"the chocobo move state's handle tick slot (1)", 0x1466B9E20,
    {L"ChocoboMoveState_Slot1_TickHandles", 0x14344F218, kChocoboMoveStateHandlesBytes}};

// The SB scooter (Wheelie) move state (vtable 0x1466B9D88)
// The SB scooter (Wheelie) move state's slot 16 (vtable 0x1466B9D88; RE-p2-wheelie-ride.md §14.5): the field tick runs
// the ridden scooter's slot 16 (drive slot 11 inside it, then the trigger feedback and the getoff latch).
inline constexpr std::uint8_t kScooterMoveStateUpdateInputBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89,
    0x7C, 0x24, 0x18, 0x55, 0x48, 0x8B, 0xEC, 0xB8, 0x50, 0x00, 0x00, 0x00};
inline constexpr SlotFact kScooterMoveStateUpdateInput{
    L"the scooter move state's input-update slot (16)", 0x1466B9E08,
    {L"SBScooterMoveState_Slot16_UpdateInput_GetOff", 0x1434506A0, kScooterMoveStateUpdateInputBytes}};

}  // namespace lss::facts
