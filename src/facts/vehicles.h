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

// ---------------------------------------------------------------------------------------------
// Scour and Scent: the chocobo craft (research/RE-chocobo-scour-scent-seats.md)
// ---------------------------------------------------------------------------------------------

/// The chocobo craft manager (the field root's slot, facts/world.h kRootChocoboCraft): the chocobo's input slot writes
/// a request byte, the manager's tick in the world tick consumes it and runs the one session, always for the party
/// leader's chocobo. Its session struct and the session's fields (offsets into the session):
inline constexpr std::uint32_t kCraftScourRequest = 0x8B;  // u8, the manager's: Scour pressed
inline constexpr std::uint32_t kCraftScentRequest = 0x8C;  // u8: Scent pressed
inline constexpr std::uint32_t kCraftSession      = 0x88;
inline constexpr std::uint32_t kCraftFilter       = 0x8;    // weak: the full-screen smell filter
inline constexpr std::uint32_t kCraftItemTimer    = 0x160;  // f32: the dug item's message timer
inline constexpr std::uint32_t kCraftDoneTimer    = 0x164;  // f32: the dig's completion timer
inline constexpr std::uint32_t kCraftStatusIcon   = 0x340;  // weak: the status icon over the chocobo
inline constexpr std::uint32_t kCraftArrowA       = 0x35C;  // weak: the two screen-edge arrows
inline constexpr std::uint32_t kCraftArrowB       = 0x364;
inline constexpr std::uint32_t kCraftSmellMode    = 0x378;  // u8: Scent's smell mode on
/// ChocoCraftMgr_Tick(manager, float dt), at its one call in the world tick's subsystems (the call crosses a block).
inline constexpr std::uint8_t kCraftMgrTickBytes[] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48,
    0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x41, 0x55, 0x41, 0x56, 0x41};
inline constexpr FunctionFact   kCraftMgrTick{L"ChocoCraftMgr_Tick", 0x140D2FC90, kCraftMgrTickBytes};
inline constexpr std::uintptr_t kCraftMgrTickCall = 0x140826A0E;
/// The chocobo move state's input slot's two row gates, bool(): Scent and Scour available for the leader's chocobo.
inline constexpr std::uint8_t kCraftSmellGateBytes[] = {
    0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x8E, 0xA0, 0xEA, 0xFE, 0x48, 0x2B,
    0xE0, 0xE8, 0x2A, 0xE9, 0xA6, 0xFD, 0x48, 0x8D, 0x88, 0x88, 0x00, 0x00};
inline constexpr FunctionFact   kCraftSmellGate{L"ChocoboCraft_SmellAvailable_Wrap", 0x1432C1AE8, kCraftSmellGateBytes};
inline constexpr std::uintptr_t kCraftSmellGateCall = 0x143170827;
inline constexpr std::uint8_t kCraftDigGateBytes[] = {
    0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xB2, 0xA0, 0xEA, 0xFE, 0x48, 0x2B,
    0xE0, 0xE8, 0x4E, 0xE9, 0xA6, 0xFD, 0x48, 0x8D, 0x88, 0x88, 0x00, 0x00};
inline constexpr FunctionFact   kCraftDigGate{L"ChocoboCraft_DigAvailable_Wrap", 0x1432C1AC4, kCraftDigGateBytes};
inline constexpr std::uintptr_t kCraftDigGateCall = 0x143170837;
/// The sniff action's three reads of the party leader (ResolveFieldLeaderCharOrLeader(0); its state 1 enter, state 3
/// enter and finish set and clear move flags on that character), on the animation worker or the game thread.
inline constexpr std::uintptr_t kCraftSniffLeaderCalls[] = {0x142F969B6, 0x142F95C27, 0x142F95498};
/// PrimaryPC_ChocoboCoord_LeaderRideBody(u8 seatedOnly): the party leader's chocobo through the primary controller's
/// chocobo coordinator; its two calls in ChocoCraft_SetCraftLocks (the on branch, the off branch: the forced walk set and
/// released), some of whose callers run outside the manager's tick.
inline constexpr std::uint8_t kLeaderRideBodyBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0xB8,
    0x20, 0x00, 0x00, 0x00, 0xE8, 0xBF, 0x88, 0xA0, 0x01, 0x48, 0x2B, 0xE0};
inline constexpr FunctionFact   kLeaderRideBody{L"PrimaryPC_ChocoboCoord_LeaderRideBody", 0x1407632AC, kLeaderRideBodyBytes};
inline constexpr std::uintptr_t kCraftLocksOnRideCall  = 0x1432C64C4;
inline constexpr std::uintptr_t kCraftLocksOffRideCall = 0x1432C6563;

// ---------------------------------------------------------------------------------------------
// The chocobo call (the whistle; research/RE-chocobo-summon-seats.md)
// ---------------------------------------------------------------------------------------------

/// The ride manager's asset state (u8; 2 loaded): a member entry spawns no chocobo before it.
inline constexpr std::uint32_t kRideManagerAssets = 0xC80;
inline constexpr std::uint8_t  kRideAssetsLoaded  = 2;
/// The summon body's calls the own call answers for a player 2's or 3's member: MoveState_ChocoboWhistleOrDismiss(state,
/// stay) (stay: the "stay" order to the leader's chocobo), PrimaryPCMode_Vt300_ForKey(request) for keys 1 (the walk-in
/// ride), 4 (the scooter) and 2 (the buggy), FieldSummon_CanCallBuggy(character, ...) for the buggy's row and press.
inline constexpr std::uint8_t kWhistleOrDismissBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8,
    0x68, 0x4F, 0x00, 0xFF, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xD9, 0x84, 0xD2};
inline constexpr FunctionFact   kWhistleOrDismiss{L"MoveState_ChocoboWhistleOrDismiss", 0x143166C08, kWhistleOrDismissBytes};
inline constexpr std::uintptr_t kWhistleOrDismissCall = 0x1410E1F9B;
inline constexpr std::uint8_t kRideForKeyBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x70, 0x98, 0x08, 0x01,
    0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xD9, 0x8A, 0x09, 0x84, 0xC9, 0x74, 0x2F};
inline constexpr FunctionFact   kRideForKey{L"PrimaryPCMode_Vt300_ForKey", 0x1410E2304, kRideForKeyBytes};
inline constexpr std::uintptr_t kSummonRideForKeyCalls[] = {0x1410E1DB6, 0x1410E1FB2, 0x1410E20D7};
inline constexpr std::uint8_t kCanCallBuggyBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xD4, 0x94, 0x08, 0x01,
    0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xD9, 0xE8, 0x71, 0x00, 0x00, 0x00, 0x84};
inline constexpr FunctionFact   kCanCallBuggy{L"FieldSummon_CanCallBuggy", 0x1410E26A0, kCanCallBuggyBytes};
inline constexpr std::uintptr_t kSummonCanCallBuggyCalls[] = {0x1410E201A, 0x1410E20B6};
/// UEndAnimNotifyCallChocobo's Notify (its vtable slot): the whistle sound, then the party's call
/// ChocoboCall_RequestCall_FromWhistle(bool) at one call.
inline constexpr std::uint8_t kCallNotifyBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x41, 0x56,
    0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xBE, 0xD7, 0x13, 0xFF, 0x48, 0x2B};
inline constexpr SlotFact kCallNotifySlot{
    L"the chocobo call notify's Notify slot", 0x146529A10, {L"UEndAnimNotifyCallChocobo__Notify", 0x14302E3AC, kCallNotifyBytes}};
inline constexpr std::uint8_t kCallFromWhistleBytes[] = {
    0x40, 0x55, 0x48, 0x8D, 0x6C, 0x24, 0xA9, 0xB8, 0xA0, 0x00, 0x00, 0x00,
    0xE8, 0x4B, 0x81, 0x70, 0xFE, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0x05, 0xC1};
inline constexpr FunctionFact   kCallFromWhistle{L"ChocoboCall_RequestCall_FromWhistle", 0x143A63A24, kCallFromWhistleBytes};
inline constexpr std::uintptr_t kCallFromWhistleCall = 0x14302E438;
/// ChocoboRideMgr_IsPartyRideActive(manager) at its four calls in the member entries' tick, where r15 holds the entry's
/// offset (0x2C * player type) from the first entry.
inline constexpr std::uint8_t kPartyRideActiveBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xE8, 0x57, 0xB4, 0x00,
    0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xD9, 0xE8, 0x21, 0x00, 0x00, 0x00, 0x84};
inline constexpr FunctionFact   kPartyRideActive{L"ChocoboRideMgr_IsPartyRideActive", 0x14162638C, kPartyRideActiveBytes};
inline constexpr std::uintptr_t kEntryPartyRideCalls[] = {0x140B36674, 0x1424CC406, 0x1424CC72B, 0x1424CCD8B};
/// The rest of that answer, without the party's ride flag: ChocoboRideMgr_IsSuspended_BattleOrAbort(manager) (a fight, an
/// abort, the getoff in progress) and the manager's ride-off byte.
inline constexpr std::uint8_t kRideSuspendedBytes[] = {
    0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xB2, 0x57, 0xB4, 0x00, 0x48, 0x2B,
    0xE0, 0x80, 0xB9, 0xB6, 0x0E, 0x00, 0x00, 0x00, 0x75, 0x18, 0x80, 0xB9};
inline constexpr FunctionFact  kRideSuspended{L"ChocoboRideMgr_IsSuspended_BattleOrAbort", 0x1416263C4, kRideSuspendedBytes};
inline constexpr std::uint32_t kRideManagerRideOff = 0xC90;  // u8: set, no member rides
/// ChocoboCall_LeaderChocoboFollowing_StayLabel(state): the summon row reads "stay" while the leader's chocobo follows,
/// at its call in the summon body (the call crosses a block).
inline constexpr std::uint8_t kStayLabelBytes[] = {
    0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x72, 0x1E, 0x3B, 0x01, 0x48, 0x2B,
    0xE0, 0xE8, 0x56, 0x11, 0x2F, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x1A, 0x48};
inline constexpr FunctionFact   kStayLabel{L"ChocoboCall_LeaderChocoboFollowing_StayLabel", 0x140DB9D04, kStayLabelBytes};
inline constexpr std::uintptr_t kStayLabelCall = 0x1410E182C;
/// ChocoboRideMgr_RequestPartyGetOff(request*): the getoff of every rider, at its call in the chocobo move state's input
/// slot (the rider's Vehicle press; the call crosses a block).
inline constexpr std::uint8_t kPartyGetOffBytes[] = {
    0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x16, 0x82, 0x70, 0xFE, 0x48, 0x2B,
    0xE0, 0x48, 0x8B, 0x05, 0x44, 0xEC, 0x4C, 0x05, 0x48, 0x85, 0xC0, 0x74};
inline constexpr FunctionFact   kPartyGetOff{L"ChocoboRideMgr_RequestPartyGetOff", 0x143A63960, kPartyGetOffBytes};
inline constexpr std::uintptr_t kPartyGetOffCall = 0x14317088E;
}  // namespace lss::facts
