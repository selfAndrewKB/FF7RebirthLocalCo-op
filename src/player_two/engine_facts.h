#pragma once

// Every engine address, slot, entry-byte string and field offset player 2's stages use.
//
// Addresses are as IDA shows them (image base 0x140000000). Each function's entry bytes are the
// first 24 bytes read in IDA; call and RIP-relative displacements are relative to the image, so
// they are identical in the running game. A stage is installed only if every fact it uses holds
// (platform/verified_code.h). Each offset was read at an instruction that uses it.
//
// Research: Rebirth/Documentation/research/RE-p2-own-controller-ida.md,
// RE-p2-camera-movement-ida.md, RE-p2-build-facts-ida.md, RE-p2-sticks-tags-ida.md,
// RE-p2-viewtarget-movestate-ida.md; the view target and move-state vtable were read live
// (RE-p2-first-run-ce.md).

#include "platform/build_value.h"
#include "platform/verified_code.h"

#include <cstdint>

namespace lss::player_two::facts {

using platform::FunctionFact;
using platform::SlotFact;

// ---------------------------------------------------------------------------------------------
// Classes (vtables stored by the native constructors at offset 0)
// ---------------------------------------------------------------------------------------------

inline constexpr std::uintptr_t kEndPlayerControllerVTable = 0x1467C85A0;
inline constexpr std::uintptr_t kCameraManagerVTable       = 0x145D6FA30;
inline constexpr std::uintptr_t kFieldOperatorVTable       = 0x1466B5CB8;
inline constexpr std::uintptr_t kMoveStateVTable           = 0x145CE7158;  // every party member's move state

// ---------------------------------------------------------------------------------------------
// Field offsets
// ---------------------------------------------------------------------------------------------

// AActor
inline constexpr std::uint32_t kActorOwner     = 0xE8;   // AActor::SetOwner stores it
inline constexpr std::uint32_t kActorTagsData  = 0x178;  // Tags.GetData()
inline constexpr std::uint32_t kActorTagsCount = 0x180;  // Tags.Num()
inline constexpr std::uint32_t kPawnController = 0x260;  // APawn::Controller

// ULocalPlayer
inline constexpr std::uint32_t kLocalPlayerViewportClient = 0x70;
inline constexpr std::uint32_t kLocalPlayerControllerId   = 0xB8;

// UGameViewportClient: the four input-mode bytes (+0x361..+0x364) the controller's BeginPlay sets
// through SetInputMode on the one viewport all local players share.
inline constexpr std::uint32_t kViewportInputModeBytes = 0x361;

// Player controllers (AEndPlayerController)
inline constexpr std::uint32_t kControllerPawn           = 0x258;
inline constexpr std::uint32_t kControllerPlayer         = 0x2A0;  // the ULocalPlayer
inline constexpr std::uint32_t kControllerCameraManager  = 0x2C0;
inline constexpr std::uint32_t kControllerPlayerInput    = 0x350;  // APlayerController::PlayerInput (Destroyed nulls it)
inline constexpr std::uint32_t kControllerBattleTalkActor = 0x5B0;
inline constexpr std::uint32_t kControllerLeader         = 0x5A0;
inline constexpr std::uint32_t kControllerTrigger        = 0x610;
inline constexpr std::uint32_t kControllerAutoManageCamera = 0x2D0;  // u8 bAutoManageActiveCameraTarget
inline constexpr std::uint32_t kControllerLeaderActions  = 0x598;  // the move-state tick calls through it unchecked
inline constexpr std::uint32_t kPrimaryControllerByte897 = 0x897;  // written by the switch through "the primary"
inline constexpr std::uint32_t kPrimaryControllerByte8AB = 0x8AB;

// Camera managers (AEndPlayerCameraManager)
inline constexpr std::uint32_t kCameraManagerPcOwner           = 0x228;
inline constexpr std::uint32_t kCameraManagerViewTarget        = 0x1000;  // ViewTarget.Target
inline constexpr std::uint32_t kCameraManagerOperators         = 0x2C50;  // 13 operator pointers
inline constexpr std::uint32_t kCameraManagerOperatorCount     = 13;
inline constexpr std::uint32_t kCameraManagerFieldOperator     = 0x2CB0;  // operator 12
inline constexpr std::uint32_t kCameraManagerCurrentOperator   = 0x2CD0;  // u8 index into the operators
inline constexpr std::uint32_t kCameraManagerCollisionComponent = 0x2D48;  // outer: the collision actor
inline constexpr std::uint32_t kCameraManagerEndCameraActor    = 0x2DE8;

// Field camera operator
inline constexpr std::uint32_t kFieldOperatorCommand = 0x40;
inline constexpr std::uint32_t kFieldCommandSize     = 0x58;
inline constexpr std::uint32_t kFieldOperatorActive  = 0xD0;

// Characters (AEndCharacter)
inline constexpr std::uint32_t kCharacterRequestedController = 0x578;
inline constexpr std::uint32_t kCharacterEffectiveController = 0x580;
inline constexpr std::uint32_t kCharacterRetainedAi          = 0x588;

// Enemy AI controllers (EndAIControllerEnemyBase, both builds): an FWeakObjectPtr to the enemy character.
inline constexpr std::uint32_t kEnemyControllerCharacter = 0x464;
inline constexpr std::uint32_t kCharacterPlayerControlled    = 0x625;
inline constexpr platform::PerBuild<std::uint32_t> kCharacterMoveState = {0x1750, 0x1770};  // 1.005: EndCharacter tick 0x140E8A44A mov rcx,[rdi+1770h] (1.004 0x14125B20A [rdi+1750h]); GetMoveState 0x141985320

// The AI controller's active brain and its state (RE-p2-member-handover.md §3.3)
inline constexpr std::uint32_t kAiActiveBrain = 0x518;
inline constexpr std::uint32_t kBrainIndex    = 0x19;  // u8; 0 is the base brain
inline constexpr std::uint32_t kBrainState    = 0x20;
inline constexpr std::uint32_t kAiStateType   = 0x18;  // u8, EEndAIState

// Player move state
inline constexpr std::uint32_t kMoveStateCharacter = 0x8;
inline constexpr std::uint32_t kMoveStateBlocked   = 0x91;  // u8; the gate refuses when nonzero
inline constexpr std::uint32_t kMoveStateCooldown  = 0x94;  // float; the gate refuses when > 0
// The state's three field-action row handles (chocobo, buggy, cactus statue), 0x50 bytes each with
// the request byte at +0x48; written by the state's tick, turned into a HUD row by the character's
// own tick (RE-p2-field-action-row.md).
inline constexpr std::uint32_t kMoveStateFieldActionHandles[] = {0xA0, 0xF0, 0x140};
// The chocobo move state's own prompt handles (FieldActionIconHandle, 0x50 bytes: +0 the row's weak widget, +0x48 the
// request byte): Dash, Dismount, Jet, Scent, Scour (RE-p2-chocobo-rows.md §2).
inline constexpr std::uint32_t kChocoboMoveStateRowHandles[] = {0x200, 0x250, 0x2A0, 0x2F0, 0x340};
inline constexpr std::uint8_t  kMoveBlockingTagA   = 17;
inline constexpr std::uint8_t  kMoveBlockingTagB   = 22;

// ---------------------------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------------------------

/// The party manager (a native singleton): +0x140 is the primary player controller, an
/// FWeakObjectPtr {index, serial} read by GetPrimaryEndPlayerController (0x140A61D1C).
inline constexpr std::uintptr_t kPartyManagerGlobal            = 0x14939BCE0;
inline constexpr std::uint32_t  kPartyManagerPrimaryController = 0x140;

/// The camera manager's two spawn names (8-byte FNames; zero is None to UWorld::SpawnActor, which
/// tests the whole qword). The second is set lazily behind a thread-safe-static guard (u32:
/// 0 = never, -1 = in progress, anything else = done).
inline constexpr std::uintptr_t kEndCameraActorNameGlobal     = 0x1491DFB48;
inline constexpr std::uintptr_t kCompositeCollisionNameGlobal = 0x14934D028;
inline constexpr std::uintptr_t kCompositeCollisionNameGuard  = 0x14934D020;
inline constexpr std::uint64_t  kNameNone                     = 0;

inline bool NameGuardInitialised(const void* guard) noexcept
{
    if (guard == nullptr)
    {
        return false;
    }
    const auto value = *static_cast<const std::int32_t*>(guard);
    return value != 0 && value != -1;
}

/// Two game objects that exist once the game has started and are missing at the title and early in
/// a load: the holder of the system option data ([it]+0x100, RE-p2-classic-autoplay.md) and the
/// battle system (IsInBattle reads [it]).
inline constexpr std::uintptr_t kOptionHolderGlobal     = 0x14939BD58;
inline constexpr std::uint32_t  kOptionHolderData       = 0x100;
inline constexpr std::uintptr_t kBattleStateGlobal      = 0x14939BC58;

/// Square's input object (its stick fields and per-pad banks), and the byte that makes the merge
/// read DirectInput alone.
inline constexpr std::uintptr_t kSquareInputGlobal     = 0x1493496F0;
inline constexpr std::uintptr_t kDirectInputOnlyFlag   = 0x1490FF1C0;
inline constexpr std::uint32_t  kInputMergedSticks     = 0x1210;  // float LX, LY, RX, RY
inline constexpr std::uint32_t  kInputDeviceBanks      = 0x11D0;  // {Record* data; int32 num; int32 max}
inline constexpr std::uint32_t  kInputBankHeaderSize   = 16;
inline constexpr std::uint32_t  kXInputBank            = 1;
inline constexpr std::uint32_t  kDirectInputBank       = 2;

/// The tag registry: a weak pointer to the world's EndCharacterMovementActor, whose +0x278 is a
/// TMap<FName, int32> with 20-byte elements {FName, int32 count, ...}; and the trigger actor's tag.
inline constexpr std::uintptr_t kTagRegistryWeak         = 0x1470C94C0;
inline constexpr std::uint32_t  kTagRegistryMap          = 0x278;
inline constexpr std::uint32_t  kTagRegistryElementSize  = 20;
inline constexpr std::uint32_t  kTagRegistryCount        = 8;
inline constexpr std::uintptr_t kTriggerTagName          = 0x1491E3EE0;

/// Party roster: lane 0 is the field lane; a type of 0x10 or more is "none".
inline constexpr std::uint8_t kFieldLane       = 0;
inline constexpr std::uint8_t kNoPlayerType    = 0x10;
inline constexpr std::int32_t kMaxRosterEntries = 16;

// ---------------------------------------------------------------------------------------------
// Functions
// ---------------------------------------------------------------------------------------------

inline constexpr std::uint8_t kCameraManagerBeginPlayBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC, 0xB8, 0x70, 0x00, 0x00, 0x00};
inline constexpr std::uint8_t kCameraUpdateBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41,
    0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0xB8, 0x90, 0x05, 0x00, 0x00};
inline constexpr std::uint8_t kFieldActivateBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x88, 0x99, 0x80, 0x01,
    0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xD9, 0xE8, 0x75, 0x01, 0x00, 0x00, 0x48};
inline constexpr std::uint8_t kFieldDeactivateBytes[] = {
    0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x56, 0x0F, 0xCB, 0xFE, 0x48, 0x2B,
    0xE0, 0x48, 0x83, 0xC4, 0x28, 0xE9, 0x02, 0xA3, 0x73, 0xFF, 0xCC, 0xCC};
inline constexpr std::uint8_t kFieldCopyCommandBytes[] = {
    0xC5, 0xFC, 0x10, 0x02, 0xC5, 0xFC, 0x11, 0x41, 0x40, 0xC5, 0xFC, 0x10,
    0x4A, 0x20, 0xC5, 0xFC, 0x11, 0x49, 0x60, 0xC5, 0xF8, 0x10, 0x42, 0x40};
inline constexpr std::uint8_t kProcessPlayerInputBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55, 0x57,
    0x41, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0xB9, 0xB8, 0xC0, 0x00, 0x00, 0x00};
inline constexpr std::uint8_t kMoveStateTickBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8,
    0x88, 0xDC, 0xC5, 0xFE, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xF9, 0xE8, 0x3D};
// Slot 16 of both move-state vtables (the input update: slot 11, then the terrain detection) and
// the leader-action component's tick (the merged stick's history), RE-p2-terrain-action-conflict.md §7.
inline constexpr std::uint8_t kMoveStateUpdateInputBytes[] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48,
    0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x41, 0x56, 0xB8, 0x30, 0x00};
inline constexpr std::uint8_t kPlayerMoveStateUpdateInputBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89,
    0x74, 0x24, 0x20, 0x57, 0xB8, 0x50, 0x00, 0x00, 0x00, 0xE8, 0x66, 0xEC};
// The chocobo move state's slots 11 and 16 (vtable 0x1466B9E18; RE-p2-chocobo-ride.md §1.1, §1.2): the field
// tick runs the ridden chocobo's slot 16 after player 1's leader-action tick. Entry bytes read from the image.
inline constexpr std::uint8_t kChocoboMoveStateTickBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x70, 0xFF, 0xFF, 0xFF};
inline constexpr std::uint8_t kChocoboMoveStateUpdateInputBytes[] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x48,
    0x89, 0x78, 0x18, 0x4C, 0x89, 0x70, 0x20, 0x55, 0x48, 0x8B, 0xEC, 0xB8};
// Its slot 1 (RE-p2-chocobo-run-refresh.md §2.1, §4.1): the chocobo's own actor tick runs it each frame, game context;
// it holds the anim sync that rewrites the gait, speed and run timer of any chocobo not the primary PC's ridden one.
inline constexpr std::uint8_t kChocoboMoveStateHandlesBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0xB8,
    0x40, 0x00, 0x00, 0x00, 0xE8, 0x03, 0xFD, 0xC5, 0xFE, 0x48, 0x2B, 0xE0};
// The SB scooter (Wheelie) move state's slot 16 (vtable 0x1466B9D88; RE-p2-wheelie-ride.md §14.5): the field tick runs
// the ridden scooter's slot 16 (drive slot 11 inside it, then the trigger feedback and the getoff latch).
inline constexpr std::uint8_t kScooterMoveStateUpdateInputBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89,
    0x7C, 0x24, 0x18, 0x55, 0x48, 0x8B, 0xEC, 0xB8, 0x50, 0x00, 0x00, 0x00};
inline constexpr std::uint8_t kLeaderActionTickBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x7C, 0x24, 0x10, 0x55, 0x48,
    0x8D, 0x6C, 0x24, 0xA9, 0xB8, 0xB0, 0x00, 0x00, 0x00, 0xE8, 0xAA, 0x2D};
inline constexpr std::uint8_t kMoveStateTickInputBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0xC5, 0xFA, 0x11, 0x4C, 0x24, 0x10, 0x55,
    0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D};

inline constexpr std::uint8_t kWeakObjectGetBytes[] = {
    0x44, 0x8B, 0x49, 0x04, 0x45, 0x33, 0xC0, 0x45, 0x85, 0xC9, 0x74, 0x4F,
    0x8B, 0x01, 0x85, 0xC0, 0x78, 0x49, 0x3B, 0x05, 0x1C, 0x5B, 0xFE, 0x06};
inline constexpr std::uint8_t kWeakObjectAssignBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0xB8,
    0x20, 0x00, 0x00, 0x00, 0xE8, 0x67, 0x96, 0x7A, 0x00, 0x48, 0x2B, 0xE0};
inline constexpr std::uint8_t kEnsureEndPlayerTriggerBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x50, 0x00, 0x00, 0x00, 0xE8,
    0x54, 0x4A, 0x3A, 0xFE, 0x48, 0x2B, 0xE0, 0x33, 0xC0, 0x48, 0x8B, 0xF9};
inline constexpr std::uint8_t kSwitchLeaderInternalBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57,
    0xB8, 0x40, 0x00, 0x00, 0x00, 0xE8, 0x66, 0x9D, 0xEA, 0x00, 0x48, 0x2B};
inline constexpr std::uint8_t kRosterCountBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x10, 0xF9, 0x66, 0x01,
    0x48, 0x2B, 0xE0, 0x4C, 0x8B, 0x05, 0xCE, 0x5F, 0x7E, 0x08, 0x45, 0x33};
inline constexpr std::uint8_t kRosterPlayerTypeBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x1C, 0xFA, 0x66, 0x01,
    0x48, 0x2B, 0xE0, 0x4C, 0x8B, 0x05, 0xDA, 0x60, 0x7E, 0x08, 0x44, 0x8B};
inline constexpr std::uint8_t kCharacterByPlayerTypeBytes[] = {
    0x4C, 0x8B, 0x05, 0xA5, 0xA6, 0x96, 0x08, 0x44, 0x0F, 0xB6, 0xC9, 0x4D,
    0x85, 0xC0, 0x0F, 0x84, 0x87, 0x00, 0x00, 0x00, 0x80, 0xFA, 0x0F, 0x73};
inline constexpr std::uint8_t kIsEndCharacterBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x80, 0x00, 0x00, 0x00, 0xE8,
    0xD8, 0x58, 0x1B, 0x01, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0x05, 0xE6, 0x01};
inline constexpr std::uint8_t kAiControllerComponentBytes[] = {
    0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x02, 0xF0, 0x6B, 0x01, 0x48, 0x2B,
    0xE0, 0x48, 0x8B, 0x89, 0x38, 0x06, 0x00, 0x00, 0x48, 0x85, 0xC9, 0x74};
inline constexpr std::uint8_t kValidateAiControllerBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xA8, 0xEF, 0x6B, 0x01,
    0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9, 0x74, 0x1B, 0x48};
inline constexpr std::uint8_t kIsInBattleBytes[] = {
    0x48, 0x8B, 0x05, 0x25, 0x98, 0xAB, 0x08, 0x33, 0xC9, 0x38, 0x88, 0x00,
    0x03, 0x00, 0x00, 0x75, 0x2A, 0x48, 0x8B, 0x05, 0xA4, 0xC9, 0x81, 0x08};
inline constexpr std::uint8_t kResolvePartyLeaderBytes[] = {
    0x48, 0x8B, 0x05, 0xC9, 0xAA, 0x93, 0x08, 0x45, 0x33, 0xC0, 0x48, 0x85,
    0xC0, 0x74, 0x70, 0x44, 0x8B, 0x88, 0x4C, 0x01, 0x00, 0x00, 0x45, 0x85};

inline constexpr std::uint8_t kIsPlayerCoreBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x08, 0x7F, 0x8A, 0x01,
    0x48, 0x2B, 0xE0, 0x48, 0x8B, 0x89, 0x38, 0x06, 0x00, 0x00, 0x33, 0xDB};
inline constexpr std::uint8_t kIsPrimaryCharacterBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xAC, 0x84, 0x80, 0x01};
inline constexpr std::uint8_t kSpecialMoveCharacterBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x6C, 0x0D, 0xA2, 0x00, 0x48, 0x2B, 0xE0, 0x48};
inline constexpr std::uint8_t kMoveStateControlOwnerBytes[] = {
    0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xC6, 0xFA, 0x82, 0x01, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0x41};
inline constexpr std::uint8_t kHasStateTagBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x30, 0xEC, 0x66, 0x01, 0x48, 0x2B, 0xE0, 0x48};
inline constexpr std::uint8_t kTagSetFindIdBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x7C, 0x24, 0x10, 0x8B, 0x41, 0x08, 0x48, 0x8B, 0xFA};
inline constexpr std::uint8_t kSetViewTargetBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x8D, 0xAC, 0x24, 0xA0, 0xF9, 0xFF, 0xFF, 0xB8, 0x60, 0x07, 0x00};
inline constexpr std::uint8_t kMoveStateTickForCharacterBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8,
    0x54, 0x40, 0xE5, 0x00, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0x41, 0x08, 0x48};
inline constexpr std::uint8_t kFieldActionHandleClearBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x4C, 0x9C, 0xEB, 0x00,
    0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xD9, 0xE8, 0xE5, 0x60, 0x6F, 0x00, 0x84};
inline constexpr std::uint8_t kIsBattleActiveBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x68, 0xED, 0x66, 0x01,
    0x48, 0x2B, 0xE0, 0x4C, 0x8B, 0xC1, 0x48, 0x85, 0xC9, 0x0F, 0x84, 0xA1};
inline constexpr std::uint8_t kFieldLeaderTypeBytes[] = {
    0x48, 0x8B, 0x05, 0x9D, 0x3F, 0x7C, 0x08, 0x48, 0x85, 0xC0, 0x74, 0x1A,
    0x48, 0x8B, 0x48, 0x48, 0x48, 0x85, 0xC9, 0x75, 0x0D, 0x48, 0x8B, 0x48};
inline constexpr std::uint8_t kDefaultBattleLeaderTypeBytes[] = {
    0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xB6, 0xAC, 0xBA, 0x00, 0x48, 0x2B,
    0xE0, 0x48, 0x8B, 0x0D, 0x74, 0x13, 0xD2, 0x07, 0x48, 0x85, 0xC9, 0x74};
inline constexpr std::uint8_t kIsBattleMemberBytes[] = {
    0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xAA, 0xAE, 0xD8, 0x00, 0x48, 0x2B,
    0xE0, 0x44, 0x8A, 0xC9, 0xE8, 0x9B, 0xB6, 0x71, 0xFF, 0x3C, 0x06, 0x74};
inline constexpr std::uint8_t kIsPartyLeaderBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x98, 0x7E, 0x8A, 0x01,
    0x48, 0x2B, 0xE0, 0x48, 0x8B, 0x05, 0x3E, 0x4C, 0xB9, 0x08, 0x48, 0x85};

/// APlayerCameraManager::SetViewTarget(this, NewTarget, const FViewTargetTransitionParams*): the
/// call the End camera manager's BeginPlay makes with its camera actor.
inline constexpr FunctionFact kSetViewTarget{L"APlayerCameraManager::SetViewTarget", 0x1413C2F70, kSetViewTargetBytes};

/// FViewTargetTransitionParams as the End BeginPlay builds it: no blend time, cubic, exponent 2.
struct ViewTargetTransition
{
    float         blendTime     = 0.0f;
    std::uint8_t  blendFunction = 1;  // VTBlend_Cubic
    std::uint8_t  padding[3]    = {};
    float         blendExp      = 2.0f;
    std::uint32_t lockOutgoing  = 0;
};
static_assert(sizeof(ViewTargetTransition) == 16);

/// Ticks one character's move state (rcx = the state, xmm1 = delta time). The game calls it once a
/// frame, for the one character its field tick selects.
inline constexpr FunctionFact kMoveStateTickForCharacter{L"MoveState_TickForCharacter", 0x14125AECC,
                                                         kMoveStateTickForCharacterBytes};

/// Removes a field-action row handle's HUD row, if it has one, and zeroes its request (rcx = the
/// handle). The game calls it itself when the chocobo row's label changes.
inline constexpr FunctionFact kFieldActionHandleClear{L"FieldActionIconHandle::Clear", 0x1411F52D8,
                                                      kFieldActionHandleClearBytes};

/// The battle-player component's "is the party leader" test returns (vt+0x68 player type == the
/// party manager's leader type); the movement gate's first character clause ends in it.
inline constexpr std::uint32_t kPlayerTypeSlotOffset = 0x68;

/// FWeakObjectPtr::Get and assignment from an object.
inline constexpr FunctionFact kWeakObjectGet{L"FWeakObjectPtr::Get", 0x141905998, kWeakObjectGetBytes};
inline constexpr FunctionFact kWeakObjectAssign{L"FWeakObjectPtr assignment", 0x1419058B4, kWeakObjectAssignBytes};

/// Party member switch and its helpers. The two predicates return a byte in al.
inline constexpr FunctionFact kEnsureEndPlayerTrigger{L"AEndPlayerController::EnsureEndPlayerTrigger", 0x143D0A4CC,
                                                      kEnsureEndPlayerTriggerBytes};
inline constexpr FunctionFact kSwitchLeaderInternal{L"AEndPlayerController::SwitchLeaderInternal", 0x1412051B4,
                                                    kSwitchLeaderInternalBytes};
inline constexpr FunctionFact kRosterCount{L"the party roster count", 0x140A3F614, kRosterCountBytes};

// Player 1's battle switch's member test T (RE-p2-deployed-party.md §4.1, RE-p2-party-switch.md §1.2)
// as the game's own function (al only; 167 callers, so called, never hooked): the character is live,
// has its battle controller component, is not in the dead state (6) and is not petrified (id 22).
// And the two leader types the game hands back to player 1, and the deployed test itself.
inline constexpr FunctionFact kIsBattleActive{L"Character_IsBattleActive_NotDeadNotPetrified", 0x140A401BC,
                                              kIsBattleActiveBytes};
inline constexpr FunctionFact kFieldLeaderType{L"PartyData_GetFieldLeaderType", 0x140A61654, kFieldLeaderTypeBytes};
inline constexpr FunctionFact kDefaultBattleLeaderType{L"PartyData_GetDefaultBattleLeaderType", 0x141504270,
                                                       kDefaultBattleLeaderTypeBytes};
inline constexpr FunctionFact kIsBattleMember{L"UEndPartyAPI_IsBattleMember", 0x14132407C, kIsBattleMemberBytes};
inline constexpr FunctionFact kRosterPlayerType{L"the party roster's player type", 0x140A3F508, kRosterPlayerTypeBytes};
inline constexpr FunctionFact kCharacterByPlayerType{L"PartyManager::GetCharacterByPlayerType", 0x140A31634,
                                                     kCharacterByPlayerTypeBytes};
inline constexpr FunctionFact kIsEndCharacter{L"IsEndCharacter", 0x140EF9648, kIsEndCharacterBytes};
inline constexpr FunctionFact kAiControllerComponent{L"EndCharacter::GetAIControllerComponent", 0x1409EFF24,
                                                     kAiControllerComponentBytes};
inline constexpr FunctionFact kValidateAiController{L"EndCharacter::ValidateAIController", 0x1409EFF7C,
                                                    kValidateAiControllerBytes};
inline constexpr FunctionFact kIsInBattle{L"UEndBattleAPI::IsInBattle", 0x1408E242C, kIsInBattleBytes};
inline constexpr FunctionFact kResolvePartyLeader{L"the party leader resolver", 0x140A61210, kResolvePartyLeaderBytes};

/// The movement gate's per-character tests (read-only).
inline constexpr FunctionFact kIsPlayerCore{L"IsUCPC_Core", 0x14080701C, kIsPlayerCoreBytes};
inline constexpr FunctionFact kIsPrimaryCharacter{L"the primary controller's character test", 0x1408A6A74,
                                                  kIsPrimaryCharacterBytes};
inline constexpr FunctionFact kSpecialMoveCharacter{L"the gate's third character", 0x14168E1B8,
                                                    kSpecialMoveCharacterBytes};
inline constexpr FunctionFact kMoveStateControlOwner{L"the move state's control owner", 0x14087F460,
                                                     kMoveStateControlOwnerBytes};
inline constexpr FunctionFact kHasStateTag{L"the control owner's state-tag test", 0x140A402F4, kHasStateTagBytes};

/// The tag registry's read-only lookup: TSet<TPair<FName, int32>>::FindId.
inline constexpr FunctionFact kTagSetFindId{L"the tag registry lookup", 0x1408428F0, kTagSetFindIdBytes};

// ---------------------------------------------------------------------------------------------
// Replaced vtable slots
// ---------------------------------------------------------------------------------------------

inline constexpr SlotFact kCameraManagerBeginPlay{
    L"the End camera manager's BeginPlay slot (103)", 0x145D6FD68,
    {L"AEndPlayerCameraManager::BeginPlay", 0x143D33994, kCameraManagerBeginPlayBytes}};
inline constexpr SlotFact kCameraUpdate{
    L"the End camera manager's UpdateCamera slot (211)", 0x145D700C8,
    {L"AEndPlayerCameraManager::UpdateCamera", 0x140E55E40, kCameraUpdateBytes}};
inline constexpr SlotFact kFieldActivate{
    L"the Field operator's Activate slot (+0x10)", 0x1466B5CC8,
    {L"FEndFieldCameraOperator::Activate", 0x1408A559C, kFieldActivateBytes}};
inline constexpr SlotFact kFieldDeactivate{
    L"the Field operator's Deactivate slot (+0x20)", 0x1466B5CD8,
    {L"FEndFieldCameraOperator::Deactivate", 0x1433FDFD0, kFieldDeactivateBytes}};
inline constexpr SlotFact kFieldCopyCommand{
    L"the Field operator's CopyCommand slot (+0x58)", 0x1466B5D10,
    {L"FEndCameraOperator::CopyCommandSnapshot", 0x140FA0E80, kFieldCopyCommandBytes}};
inline constexpr SlotFact kProcessPlayerInput{
    L"the End player controller's ProcessPlayerInput slot (400)", 0x1467C9220,
    {L"AEndPlayerController::ProcessPlayerInput", 0x1407BDE58, kProcessPlayerInputBytes}};
inline constexpr SlotFact kMoveStateTick{
    L"the player move state's Tick slot (11)", 0x1457C5908,
    {L"PlayerMoveState::Tick", 0x143451298, kMoveStateTickBytes}};
inline constexpr SlotFact kMoveStateTickInput{
    L"the base move state's Tick slot (11)", 0x145CE71B0,
    {L"PlayerMoveState::TickInput", 0x1407BCAF0, kMoveStateTickInputBytes}};
/// The leader-action component's owning controller (0x14125C16C reads comp+0xA0 as the PC).
inline constexpr std::uint32_t kLeaderActionOwner = 0xA0;

inline constexpr SlotFact kMoveStateUpdateInput{
    L"the base move state's input-update slot (16)", 0x145CE71D8,
    {L"MoveStateBase_UpdateInput_Slot16", 0x1407BC858, kMoveStateUpdateInputBytes}};
inline constexpr SlotFact kPlayerMoveStateUpdateInput{
    L"the player move state's input-update slot (16)", 0x1457C5930,
    {L"PlayerMoveState_Slot16_UpdateInput", 0x1434502B0, kPlayerMoveStateUpdateInputBytes}};
inline constexpr SlotFact kChocoboMoveStateTick{
    L"the chocobo move state's tick slot (11)", 0x1466B9E70,
    {L"ChocoboMoveState_Slot11", 0x143451320, kChocoboMoveStateTickBytes}};
inline constexpr SlotFact kChocoboMoveStateUpdateInput{
    L"the chocobo move state's input-update slot (16)", 0x1466B9E98,
    {L"ChocoboMoveState_UpdateInput_Slot16", 0x143450444, kChocoboMoveStateUpdateInputBytes}};
inline constexpr SlotFact kChocoboMoveStateHandles{
    L"the chocobo move state's handle tick slot (1)", 0x1466B9E20,
    {L"ChocoboMoveState_Slot1_TickHandles", 0x14344F218, kChocoboMoveStateHandlesBytes}};
inline constexpr SlotFact kScooterMoveStateUpdateInput{
    L"the scooter move state's input-update slot (16)", 0x1466B9E08,
    {L"SBScooterMoveState_Slot16_UpdateInput_GetOff", 0x1434506A0, kScooterMoveStateUpdateInputBytes}};
inline constexpr SlotFact kLeaderActionTick{
    L"the leader-action component's tick slot (+0x428)", 0x145D6C490,
    {L"EndPlayerLeaderActionParamComponent::Tick_Vt428", 0x14125C16C, kLeaderActionTickBytes}};
inline constexpr SlotFact kIsPartyLeader{
    L"the battle-player component's leader-test slot (+0x70)", 0x145795F48,
    {L"IsPartyLeader_ForActor", 0x14080708C, kIsPartyLeaderBytes}};

}  // namespace lss::player_two::facts
