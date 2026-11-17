#pragma once

// The move states (a party member's movement machine): the base and player states, their slots, the field-action
// rows they request, and the movement gate's per-character tests.
//
// Addresses are as IDA shows them (image base 0x140000000). Each function's entry bytes are the first 24 bytes read in
// IDA; call and RIP-relative displacements are relative to the image, so they are identical in the running game. A
// stage is installed only if every fact it uses holds (platform/verified_code.h). Each offset was read at an
// instruction that uses it.
//
// Research: Rebirth/Documentation/research/RE-p2-viewtarget-movestate-ida.md, RE-p2-camera-movement-ida.md,
// RE-p2-field-action-row.md, RE-p2-terrain-action-conflict.md; the move-state vtable was read live
// (RE-p2-first-run-ce.md).

#include "platform/verified_code.h"

#include <cstdint>

namespace lss::facts {

using platform::FunctionFact;
using platform::SlotFact;

// The class
inline constexpr std::uintptr_t kMoveStateVTable           = 0x145B51B60;  // every party member's move state

// Fields
inline constexpr std::uint32_t kMoveStateCharacter = 0x8;
inline constexpr std::uint32_t kMoveStateBlocked   = 0x91;  // u8; the gate refuses when nonzero
inline constexpr std::uint32_t kMoveStateCooldown  = 0x94;  // float; the gate refuses when > 0
// The state's three field-action row handles (chocobo, buggy, cactus statue), 0x50 bytes each with
// the request byte at +0x48; written by the state's tick, turned into a HUD row by the character's
// own tick (RE-p2-field-action-row.md).
inline constexpr std::uint32_t kMoveStateFieldActionHandles[] = {0xA0, 0xF0, 0x140};
inline constexpr std::uint8_t  kMoveBlockingTagA   = 17;
inline constexpr std::uint8_t  kMoveBlockingTagB   = 22;

// Functions
/// Ticks one character's move state (rcx = the state, xmm1 = delta time). The game calls it once a
/// frame, for the one character its field tick selects.
inline constexpr std::uint8_t kMoveStateTickForCharacterBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8,
    0xC0, 0x1C, 0x2E, 0x01, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0x41, 0x08, 0x48};
inline constexpr FunctionFact kMoveStateTickForCharacter{L"MoveState_TickForCharacter", 0x140E89EB0,
                                                         kMoveStateTickForCharacterBytes};
/// Removes a field-action row handle's HUD row, if it has one, and zeroes its request (rcx = the
/// handle). The game calls it itself when the chocobo row's label changes.
inline constexpr std::uint8_t kFieldActionHandleClearBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x84, 0xED, 0xDE, 0x00,
    0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xD9, 0xE8, 0x59, 0x7C, 0x4B, 0xFF, 0x84};
inline constexpr FunctionFact kFieldActionHandleClear{L"FieldActionIconHandle::Clear", 0x14137CDF0,
                                                      kFieldActionHandleClearBytes};
inline constexpr std::uint8_t kMoveStateControlOwnerBytes[] = {
    0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xCE, 0x4C, 0x8D, 0x01, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0x41};
inline constexpr FunctionFact kMoveStateControlOwner{L"the move state's control owner", 0x140896EA8,
                                                     kMoveStateControlOwnerBytes};

// The movement gate's per-character tests (read-only)
inline constexpr std::uint8_t kIsPlayerCoreBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x8C, 0x90, 0x7B, 0x01,
    0x48, 0x2B, 0xE0, 0x48, 0x8B, 0x89, 0x38, 0x06, 0x00, 0x00, 0x33, 0xDB};
inline constexpr FunctionFact kIsPlayerCore{L"IsUCPC_Core", 0x1409B2AE8, kIsPlayerCoreBytes};
inline constexpr std::uint8_t kIsActiveRiddenVehicleBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x54, 0x60, 0x82, 0x01};
/// IsActiveRiddenVehicle(actor): the primary controller rides it (its +0x5B8 coordinator's body is the actor;
/// RE-p2-chocobo-ride.md, RE-p2-chocobo-climb.md); the movement gate's second character clause.
inline constexpr FunctionFact kIsActiveRiddenVehicle{L"IsActiveRiddenVehicle", 0x140945B1C,
                                                     kIsActiveRiddenVehicleBytes};
inline constexpr std::uint8_t kSpecialMoveCharacterBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xF4, 0x78, 0x63, 0x01, 0x48, 0x2B, 0xE0, 0x48};
inline constexpr FunctionFact kSpecialMoveCharacter{L"the gate's third character", 0x140B34280,
                                                    kSpecialMoveCharacterBytes};
inline constexpr std::uint8_t kHasStateTagBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xFC, 0xB4, 0x7B, 0x01, 0x48, 0x2B, 0xE0, 0x48};
inline constexpr FunctionFact kHasStateTag{L"the control owner's state-tag test", 0x1409B0678, kHasStateTagBytes};

// Replaced slots
inline constexpr std::uint8_t kMoveStateTickBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8,
    0x5C, 0xA6, 0xFF, 0xFE, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xF9, 0xE8, 0x95};
inline constexpr SlotFact kMoveStateTick{
    L"the player move state's Tick slot (11)", 0x145674518,
    {L"PlayerMoveState::Tick", 0x143171514, kMoveStateTickBytes}};
inline constexpr std::uint8_t kMoveStateTickInputBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0xC5, 0xFA, 0x11, 0x4C, 0x24, 0x10, 0x55,
    0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D};
inline constexpr SlotFact kMoveStateTickInput{
    L"the base move state's Tick slot (11)", 0x145B51BB8,
    {L"PlayerMoveState::TickInput", 0x1410E33C4, kMoveStateTickInputBytes}};
// Slot 16 of both move-state vtables (the input update: slot 11, then the terrain detection) and
// the leader-action component's tick (the merged stick's history), RE-p2-terrain-action-conflict.md.
inline constexpr std::uint8_t kMoveStateUpdateInputBytes[] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48,
    0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x41, 0x56, 0xB8, 0x30, 0x00};
inline constexpr SlotFact kMoveStateUpdateInput{
    L"the base move state's input-update slot (16)", 0x145B51BE0,
    {L"MoveStateBase_UpdateInput_Slot16", 0x1410E41AC, kMoveStateUpdateInputBytes}};
inline constexpr std::uint8_t kPlayerMoveStateUpdateInputBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89,
    0x74, 0x24, 0x20, 0x57, 0xB8, 0x50, 0x00, 0x00, 0x00, 0xE8, 0x36, 0xB6};
inline constexpr SlotFact kPlayerMoveStateUpdateInput{
    L"the player move state's input-update slot (16)", 0x145674540,
    {L"PlayerMoveState_Slot16_UpdateInput", 0x143170530, kPlayerMoveStateUpdateInputBytes}};

}  // namespace lss::facts
