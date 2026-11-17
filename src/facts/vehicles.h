#pragma once

// The rides: the vehicle coordinators and their kinds, the chocobo ride manager, the chocobo's and the scooter's
// character fields, and their move states (the slots and the chocobo's prompt rows).
//
// Addresses are as IDA shows them (image base 0x140000000). Each function's entry bytes are the first 24 bytes read in
// IDA; call and RIP-relative displacements are relative to the image, so they are identical in the running game. A
// stage is installed only if every fact it uses holds (platform/verified_code.h). Each offset was read at an
// instruction that uses it.
//
// Research: Rebirth/Documentation/research/RE-p2-chocobo-ride.md, RE-p2-chocobo-rows.md,
// RE-p2-chocobo-run-refresh.md, RE-p2-wheelie-ride.md, RE-p2-control-mode-object.md,
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
inline constexpr std::uintptr_t kChocoboCoordinatorVTable = 0x1466766C0;
inline constexpr std::uintptr_t kBuggyCoordinatorVTable   = 0x145BD6F90;
inline constexpr std::uintptr_t kBroncoCoordinatorVTable  = 0x145BD6710;
inline constexpr std::uintptr_t kScooterCoordinatorVTable = 0x145BD6B50;  // UEndPlayerSBScooterCoordinator

/// The scooter coordinator's shared block (a TSharedPtr object at +0x48): its target (the marked scooter, a weak),
/// its scooter list (a TArray of weaks: the data, then the count) and, further on, the bytes the coordinator's
/// tick reads (RE-p2-wheelie-ride.md; the same in both builds, RE-wheelie-polish.md [CONFIRMED]). The list is read
/// up to the cap: a rental area registers a few scooters.
inline constexpr std::uint32_t kScooterCoordShared     = 0x48;
inline constexpr std::uint32_t kScooterSharedTarget    = 0x0;
inline constexpr std::uint32_t kScooterSharedList      = 0x8;
inline constexpr std::uint32_t kScooterSharedListCount = 0x10;
inline constexpr std::int32_t  kScooterListCap         = 32;

/// The scooter coordinator's marker, the auto-ride icon, one per coordinator (RE-wheelie-polish.md; the same in both
/// builds [CONFIRMED]): its state (0 none, 1 out of reach, 2 within reach), the scooter it marks (a weak) and its
/// widget (a weak, in the UI system's field-marker list).
inline constexpr std::uint32_t kScooterCoordMarkerState  = 0x190;
inline constexpr std::uint32_t kScooterCoordMarkerTarget = 0x194;
inline constexpr std::uint32_t kScooterCoordMarkerWidget = 0x1E4;

/// The scooter coordinator's distance counter, the Wheelie_Top widget (RE-wheelie-polish.md, the mileage TopUI; the
/// same in both builds [CONFIRMED]): its weak at +0x40, and the disabled byte the tick's create gate reads with the
/// list count. The game's request (once per coordinator: the asset load's callback makes the widget, hidden, on the
/// root overlay; the riding tick shows it and writes the save's one mileage total) and its close.
inline constexpr std::uint32_t kScooterCoordTopWidget = 0x40;
inline constexpr std::uint32_t kScooterCoordDisabled  = 0x2A;
inline constexpr std::uint32_t kScooterCoordTopLatch  = 0x1FD;  // u8: the request made (cleared by the close)
inline constexpr std::uint8_t  kScooterRequestTopUIBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x20, 0x57, 0xB8, 0x30,
                                                              0x00, 0x00, 0x00, 0xE8, 0x4C, 0x61, 0x83, 0x00};
inline constexpr FunctionFact kScooterRequestTopUI{L"SBScooterCoordinator_RequestTopUIAssets", 0x141935A24,
                                                   kScooterRequestTopUIBytes};
inline constexpr std::uint8_t kScooterCloseTopUIBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x20, 0x57, 0xB8, 0x20,
                                                           0x00, 0x00, 0x00, 0xE8, 0x24, 0xF0, 0x08, 0x01};
inline constexpr FunctionFact kScooterCloseTopUI{L"SBScooterCoordinator_CloseTopUI", 0x1410DCB4C, kScooterCloseTopUIBytes};
/// The close's call in SBScooterCoordinator_Tick_Vt280, under "the list is empty and the counter exists" and nothing
/// else (five bytes inside one block in both builds).
inline constexpr std::uintptr_t kScooterTickCloseTopUICall = 0x140E891D2;

/// SBScooterShared_UpdateNearestTarget(shared): the one writer of a coordinator's target in the coordinator's own
/// tick. It searches that coordinator's own list for the free scooter nearest the party leader and, on the way,
/// warps any free scooter far from the leader and from its home back home (the rental return).
inline constexpr std::uint8_t kScooterUpdateTargetBytes[] = {
    0x40, 0x53, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8, 0x88, 0x63, 0x23, 0x01,
    0x48, 0x2B, 0xE0, 0x48, 0x8D, 0x54, 0x24, 0x20, 0x48, 0xC7, 0x44, 0x24};
inline constexpr FunctionFact kScooterUpdateTarget{L"SBScooterShared_UpdateNearestTarget", 0x140F357EC,
                                                   kScooterUpdateTargetBytes};
/// Its call in SBScooterCoordinator_Tick_Vt280 (five bytes inside one block in both builds).
inline constexpr std::uintptr_t kScooterUpdateTargetCall = 0x140E89147;
/// SBScooterCoordinator_TryBeginRide (the coordinator's idle tick) resolves the party data's field leader to compute
/// its marker against: this call (inside one block in both builds). The rider's resolve later in the same function
/// is another site, left to the game.
inline constexpr std::uintptr_t kScooterMarkerLeaderCall = 0x140E89380;

/// AEndPlayerController::GetOrCreateVehicleCoordinator(controller, type) (RE-p2-chocobo-ride.md).
inline constexpr std::uint8_t kGetOrCreateVehicleCoordinatorBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x56, 0x57,
    0x41, 0x56, 0xB8, 0x50, 0x00, 0x00, 0x00, 0xE8, 0x1C, 0x0D, 0x08, 0x01};
inline constexpr FunctionFact kGetOrCreateVehicleCoordinator{L"EndPC_GetOrCreateVehicleCoordinator", 0x1410EAE4C,
                                                             kGetOrCreateVehicleCoordinatorBytes};

// ---------------------------------------------------------------------------------------------
// The chocobo ride manager, [[game root]+0x270] (RE-p2-chocobo-ride.md)
// ---------------------------------------------------------------------------------------------

/// Its getoff request, and the member entries at +0x40 + 0x2C * player type (state byte +0, weak chocobo +0x20).
inline constexpr std::uint32_t kRootChocoboRideManager = 0x270;
inline constexpr std::uint32_t kRideManagerGetOff      = 0xEB0;  // u8
inline constexpr std::uint32_t kRideManagerEntries     = 0x40;
inline constexpr std::uint32_t kRideManagerEntrySize   = 0x2C;
inline constexpr std::uint8_t  kRideManagerPlayerTypes = 15;

/// ChocoboRideMgr_GetChocoboForCharacter(manager, character): the member's chocobo, a read-only lookup.
inline constexpr std::uint8_t kChocoboForCharacterBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x20, 0xA5, 0x70, 0xFE,
    0x48, 0x2B, 0xE0, 0x45, 0x33, 0xC0, 0x4C, 0x8B, 0xCA, 0x48, 0x8B, 0xD9};
inline constexpr FunctionFact kChocoboForCharacter{L"ChocoboRideMgr_GetChocoboForCharacter", 0x143A61654,
                                                   kChocoboForCharacterBytes};

// ---------------------------------------------------------------------------------------------
// The ridden characters
// ---------------------------------------------------------------------------------------------

/// AEndChocoboCharacter's native tail: its ride object and the ridden byte. 1.005 moved the tail by +0x20
/// (RE-1005-offsets-C.md): CreateRideObject 0x143B05BC3/0x143B05C10 (+0x1938, +0x1930); Chocobo_ReleaseRideObject
/// 0x1411903AC and the chocobo tick 0x140A413DC [rbx+1938h]; ChocoboCoordinator GetRideBody 0x143B13E00 and
/// FinishGetOff 0x143B14F58 [rdi+1944h].
inline constexpr platform::PerBuild<std::uint32_t> kChocoboRideObject = {0x1938, 0x1918};  // pointer
inline constexpr platform::PerBuild<std::uint32_t> kChocoboRidden     = {0x1944, 0x1924};  // u8

/// AEndSelfBalancingScooterCharacter (RE-p2-wheelie-ride.md; SDK): its vtable, its ridden and excluded bytes.
/// 1.005 moved the fields by +0x20 (RE-1005-offsets-C.md): SetRider 0x1410E9D1B writes +0x2338 (1.004 0x140DE02C7
/// +0x2318); FindNearestFreeScooter 0x140F3598D reads +0x233A (1.004 0x140AB2F71 +0x231A).
inline constexpr std::uintptr_t                    kScooterCharacterVTable = 0x1456B2078;
inline constexpr platform::PerBuild<std::uint32_t> kScooterRidden          = {0x2338, 0x2318};  // u8
inline constexpr platform::PerBuild<std::uint32_t> kScooterExcluded        = {0x233A, 0x231A};  // u8

// ---------------------------------------------------------------------------------------------
// The ride move states
// ---------------------------------------------------------------------------------------------

// The chocobo move state (its vtable, data; the slots the mod replaces)
inline constexpr std::uintptr_t kChocoboMoveStateVTable = 0x1465478E8;
// The chocobo move state's own prompt handles (FieldActionIconHandle, 0x50 bytes: +0 the row's weak widget, +0x48 the
// request byte): Dash, Dismount, Jet, Scent, Scour (RE-p2-chocobo-rows.md).
inline constexpr std::uint32_t kChocoboMoveStateRowHandles[] = {0x200, 0x250, 0x2A0, 0x2F0, 0x340};
// The chocobo move state's slots 11 and 16 (vtable 0x1465478E8; RE-p2-chocobo-ride.md): the field
// tick runs the ridden chocobo's slot 16 after player 1's leader-action tick. Entry bytes read from the image.
inline constexpr std::uint8_t kChocoboMoveStateTickBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x70, 0xFF, 0xFF, 0xFF};
inline constexpr SlotFact kChocoboMoveStateTick{
    L"the chocobo move state's tick slot (11)", 0x146547940,
    {L"ChocoboMoveState_Slot11", 0x14317159C, kChocoboMoveStateTickBytes}};
inline constexpr std::uint8_t kChocoboMoveStateUpdateInputBytes[] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x48,
    0x89, 0x78, 0x18, 0x4C, 0x89, 0x70, 0x20, 0x55, 0x48, 0x8B, 0xEC, 0xB8};
inline constexpr SlotFact kChocoboMoveStateUpdateInput{
    L"the chocobo move state's input-update slot (16)", 0x146547968,
    {L"ChocoboMoveState_UpdateInput_Slot16", 0x1431706C4, kChocoboMoveStateUpdateInputBytes}};
// Its slot 1 (RE-p2-chocobo-run-refresh.md): the chocobo's own actor tick runs it each frame, game context;
// it holds the anim sync that rewrites the gait, speed and run timer of any chocobo not the primary PC's ridden one.
inline constexpr std::uint8_t kChocoboMoveStateHandlesBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0xB8,
    0x40, 0x00, 0x00, 0x00, 0xE8, 0x7F, 0xC6, 0xFF, 0xFE, 0x48, 0x2B, 0xE0};
inline constexpr SlotFact kChocoboMoveStateHandles{
    L"the chocobo move state's handle tick slot (1)", 0x1465478F0,
    {L"ChocoboMoveState_Slot1_TickHandles", 0x14316F4EC, kChocoboMoveStateHandlesBytes}};

// The SB scooter (Wheelie) move state (vtable 0x146547858)
// Its own prompt handles (FieldActionIconHandle, 0x50 bytes: +0 the row's weak widget, +0x48 the request byte): Get
// off, Accelerate, Brake (RE-p2-wheelie-ride.md).
inline constexpr std::uint32_t kScooterMoveStateRowHandles[] = {0x218, 0x268, 0x2B8};
// The SB scooter (Wheelie) move state's slot 16 (vtable 0x146547858; RE-p2-wheelie-ride.md): the field tick runs
// the ridden scooter's slot 16 (drive slot 11 inside it, then the trigger feedback and the getoff latch).
inline constexpr std::uint8_t kScooterMoveStateUpdateInputBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89,
    0x7C, 0x24, 0x18, 0x55, 0x48, 0x8B, 0xEC, 0xB8, 0x50, 0x00, 0x00, 0x00};
inline constexpr SlotFact kScooterMoveStateUpdateInput{
    L"the scooter move state's input-update slot (16)", 0x1465478D8,
    {L"SBScooterMoveState_Slot16_UpdateInput_GetOff", 0x143170920, kScooterMoveStateUpdateInputBytes}};

}  // namespace lss::facts
