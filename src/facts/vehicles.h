#pragma once

// The rides: the vehicle coordinators and their kinds, the chocobo ride manager, the chocobo's and the scooter's
// character fields, and their move states (the slots and the chocobo's prompt rows).
//
// Addresses are as IDA shows them (image base 0x140000000). Each function's entry bytes are the first 24 bytes read in
// IDA; call and RIP-relative displacements are relative to the image, so they are identical in the running game. A
// stage is installed only if every fact it uses holds (platform/verified_code.h). Each offset was read at an
// instruction that uses it.
//
// Research: Rebirth/Documentation/research/RE-p2-chocobo-ride.md §1.1, §1.2, §4, §7.1, §9.4, RE-p2-chocobo-rows.md
// §2, RE-p2-chocobo-run-refresh.md §2.1, §4.1, RE-p2-wheelie-ride.md §1, §14.5, RE-p2-control-mode-object.md,
// RE-p2-tiny-bronco.md, RE-1005-offsets-C.md (the fields 1.005 moved).

#include "platform/build_value.h"
#include "platform/verified_code.h"

#include <cstdint>

namespace lss::facts {

using platform::FunctionFact;
using platform::SlotFact;

// ---------------------------------------------------------------------------------------------
// The vehicle coordinators (one per kind, owned by the player controller)
// ---------------------------------------------------------------------------------------------

/// A coordinator's body (the VehicleChara: the chocobo, the buggy, the boat, the scooter) and its rider.
inline constexpr std::uint32_t kCoordinatorBody  = 0x30;
inline constexpr std::uint32_t kCoordinatorRider = 0x38;

/// Each kind's coordinator vtable (data).
inline constexpr std::uintptr_t kChocoboCoordinatorVTable = 0x1467EB420;
inline constexpr std::uintptr_t kBuggyCoordinatorVTable   = 0x145D77220;
inline constexpr std::uintptr_t kBroncoCoordinatorVTable  = 0x145D769B8;
inline constexpr std::uintptr_t kScooterCoordinatorVTable = 0x145D76DF0;  // UEndPlayerSBScooterCoordinator

/// AEndPlayerController::GetOrCreateVehicleCoordinator(controller, type) (RE-p2-chocobo-ride.md §9.4).
inline constexpr std::uint8_t kGetOrCreateVehicleCoordinatorBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x56, 0x57,
    0x41, 0x56, 0xB8, 0x50, 0x00, 0x00, 0x00, 0xE8, 0x4C, 0x7F, 0xEA, 0x00};
inline constexpr FunctionFact kGetOrCreateVehicleCoordinator{L"EndPC_GetOrCreateVehicleCoordinator", 0x141206FCC,
                                                             kGetOrCreateVehicleCoordinatorBytes};

// ---------------------------------------------------------------------------------------------
// The chocobo ride manager, [[game root]+0x270] (RE-p2-chocobo-ride.md §1.1)
// ---------------------------------------------------------------------------------------------

/// Its getoff request, and the member entries at +0x40 + 0x2C * player type (state byte +0, weak chocobo +0x20).
inline constexpr std::uint32_t kRootChocoboRideManager = 0x270;
inline constexpr std::uint32_t kRideManagerGetOff      = 0xEB0;  // u8
inline constexpr std::uint32_t kRideManagerEntries     = 0x40;
inline constexpr std::uint32_t kRideManagerEntrySize   = 0x2C;
inline constexpr std::uint8_t  kRideManagerPlayerTypes = 15;

/// ChocoboRideMgr_GetChocoboForCharacter(manager, character): the member's chocobo, a read-only lookup (§7.1).
inline constexpr std::uint8_t kChocoboForCharacterBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xB8, 0xED, 0x3A, 0xFE,
    0x48, 0x2B, 0xE0, 0x45, 0x33, 0xC0, 0x4C, 0x8B, 0xCA, 0x48, 0x8B, 0xD9};
inline constexpr FunctionFact kChocoboForCharacter{L"ChocoboRideMgr_GetChocoboForCharacter", 0x143D0016C,
                                                   kChocoboForCharacterBytes};

// ---------------------------------------------------------------------------------------------
// The ridden characters
// ---------------------------------------------------------------------------------------------

/// AEndChocoboCharacter's native tail (§1.1): its ride object and the ridden byte. 1.005 moved the tail by +0x20
/// (RE-1005-offsets-C.md): CreateRideObject 0x143B05BC3/0x143B05C10 (+0x1938, +0x1930); Chocobo_ReleaseRideObject
/// 0x1411903AC and the chocobo tick 0x140A413DC [rbx+1938h]; ChocoboCoordinator GetRideBody 0x143B13E00 and
/// FinishGetOff 0x143B14F58 [rdi+1944h].
inline constexpr platform::PerBuild<std::uint32_t> kChocoboRideObject = {0x1918, 0x1938};  // pointer
inline constexpr platform::PerBuild<std::uint32_t> kChocoboRidden     = {0x1924, 0x1944};  // u8

/// AEndSelfBalancingScooterCharacter (RE-p2-wheelie-ride.md §1.1; SDK): its vtable, its ridden and excluded bytes.
/// 1.005 moved the fields by +0x20 (RE-1005-offsets-C.md): SetRider 0x1410E9D1B writes +0x2338 (1.004 0x140DE02C7
/// +0x2318); FindNearestFreeScooter 0x140F3598D reads +0x233A (1.004 0x140AB2F71 +0x231A).
inline constexpr std::uintptr_t                    kScooterCharacterVTable = 0x14583F980;
inline constexpr platform::PerBuild<std::uint32_t> kScooterRidden          = {0x2318, 0x2338};  // u8
inline constexpr platform::PerBuild<std::uint32_t> kScooterExcluded        = {0x231A, 0x233A};  // u8

// ---------------------------------------------------------------------------------------------
// The ride move states
// ---------------------------------------------------------------------------------------------

// The chocobo move state (its vtable, data; the slots the mod replaces)
inline constexpr std::uintptr_t kChocoboMoveStateVTable = 0x1466B9E18;
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
