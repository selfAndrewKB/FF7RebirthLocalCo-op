#pragma once

// The End player controller (AEndPlayerController): its class, fields, the functions and the slot the mod uses, its
// leader-action component, and Square's input object behind it.
//
// Addresses are as IDA shows them (image base 0x140000000). Each function's entry bytes are the first 24 bytes read in
// IDA; call and RIP-relative displacements are relative to the image, so they are identical in the running game. A
// stage is installed only if every fact it uses holds (platform/verified_code.h). Each offset was read at an
// instruction that uses it.
//
// Research: Rebirth/Documentation/research/RE-p2-own-controller-ida.md, RE-p2-build-facts-ida.md,
// RE-p2-sticks-tags-ida.md, RE-p2-terrain-action-conflict.md.

#include "platform/verified_code.h"

#include <cstddef>
#include <cstdint>

namespace lss::facts {

using platform::FunctionFact;
using platform::SlotFact;

// The class (the vtable stored by the native constructor at offset 0)
inline constexpr std::uintptr_t kEndPlayerControllerVTable = 0x1467C85A0;

// Fields
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
// ActiveVehicleCoordinator (RE-p2-control-mode-object.md): the coordinator of the vehicle its player rides.
inline constexpr std::uint32_t kControllerActiveVehicle  = 0x5B8;

// Functions
/// Party member switch and its helpers. The two predicates return a byte in al.
inline constexpr std::uint8_t kEnsureEndPlayerTriggerBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x50, 0x00, 0x00, 0x00, 0xE8,
    0x54, 0x4A, 0x3A, 0xFE, 0x48, 0x2B, 0xE0, 0x33, 0xC0, 0x48, 0x8B, 0xF9};
inline constexpr FunctionFact kEnsureEndPlayerTrigger{L"AEndPlayerController::EnsureEndPlayerTrigger", 0x143D0A4CC,
                                                      kEnsureEndPlayerTriggerBytes};
inline constexpr std::uint8_t kSwitchLeaderInternalBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57,
    0xB8, 0x40, 0x00, 0x00, 0x00, 0xE8, 0x66, 0x9D, 0xEA, 0x00, 0x48, 0x2B};
inline constexpr FunctionFact kSwitchLeaderInternal{L"AEndPlayerController::SwitchLeaderInternal", 0x1412051B4,
                                                    kSwitchLeaderInternalBytes};

// Replaced slots
inline constexpr std::uint8_t kProcessPlayerInputBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55, 0x57,
    0x41, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0xB9, 0xB8, 0xC0, 0x00, 0x00, 0x00};
inline constexpr SlotFact kProcessPlayerInput{
    L"the End player controller's ProcessPlayerInput slot (400)", 0x1467C9220,
    {L"AEndPlayerController::ProcessPlayerInput", 0x1407BDE58, kProcessPlayerInputBytes}};

// The leader-action component (EndPlayerLeaderActionParamComponent)
/// The leader-action component's owning controller (0x14125C16C reads comp+0xA0 as the PC).
inline constexpr std::uint32_t kLeaderActionOwner = 0xA0;
/// Its ranged selection (RE-p2-ranged-reticle.md, RE-yuffie-box-throw.md; the same in both builds [CONFIRMED]): five
/// key records of 64 bytes (the key's name first; record 0 the throw's key, record 1 the hold's), the published state
/// (bit 1 a box shown, bit 7 can shoot) and the shooting point's key, the key of the point the component adds for
/// its box. The throw fires when that key is the field-action manager's slot-0 target.
inline constexpr std::uint32_t kLeaderActionKeyRecords    = 0x370;
inline constexpr std::uint32_t kLeaderActionKeyRecordSize = 64;
/// Key record 0's press stamp (i64, the component's millisecond clock at the press; -1 while the character may not
/// start a field action: the records are reset every frame then). The throw fires on a stamp of this frame.
inline constexpr std::uint32_t kLeaderActionThrowPressStamp = 0x390;
/// Key record 0's press frame (i64, the game's frame count at the press, beside the stamp).
inline constexpr std::uint32_t kLeaderActionThrowPressFrame = 0x398;
inline constexpr std::uint32_t kLeaderActionStartFlag     = 0x1E8;  // u8: bit 0 the selection starts this frame, bit 1 it consumed the start, bit 2 a target found, bit 6 Yuffie
inline constexpr std::uint32_t kLeaderActionCandidates    = 0x1F8;  // i32: the shooting targets copied for the selection
inline constexpr std::uint32_t kLeaderActionShootingState = 0x4E0;
/// The aim point (FVector): the shown box's icon location the selection wrote, the throw's target.
inline constexpr std::uint32_t kLeaderActionAimPoint      = 0x500;
inline constexpr std::uint32_t kLeaderActionShootingKey   = 0x514;
inline constexpr std::uint32_t kLeaderActionTickLatch     = 0x9A4;  // u8: bit 0 the component ticked this frame (the controller's pre-input clears it)
/// Two fields of the controller the throw's start gate wants at or below zero (a float, an int): their meaning is
/// unknown (the controller's constructor zeroes them); read for the log.
inline constexpr std::uint32_t kControllerActionDelay     = 0x630;
inline constexpr std::uint32_t kControllerActionBlocks    = 0x740;
inline constexpr std::uint8_t  kShootingStateShown        = 0x02;
inline constexpr std::uint8_t  kShootingStateCanShoot     = 0x80;
inline constexpr std::uint8_t kLeaderActionTickBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x7C, 0x24, 0x10, 0x55, 0x48,
    0x8D, 0x6C, 0x24, 0xA9, 0xB8, 0xB0, 0x00, 0x00, 0x00, 0xE8, 0xAA, 0x2D};
inline constexpr SlotFact kLeaderActionTick{
    L"the leader-action component's tick slot (+0x428)", 0x145D6C490,
    {L"EndPlayerLeaderActionParamComponent::Tick_Vt428", 0x14125C16C, kLeaderActionTickBytes}};

// Square's input object and its keys
/// The action keys (FKey24 statics, 24 bytes: the FName first) the routes compare an event's key name with
/// (RE-p2-battle-input.md).
inline constexpr std::size_t    kKeySize          = 24;
inline constexpr std::uintptr_t kKeyAttack        = 0x1491E44D8;
inline constexpr std::uintptr_t kKeyUniqueAbility = 0x1491E4520;
inline constexpr std::uintptr_t kKeyGuard         = 0x1491E4598;
inline constexpr std::uintptr_t kKeyDodge         = 0x1491E45E0;
/// The menu-opening actions the game's one pause poll reads on the merged tables (RE-pause-menu-any-pad.md):
/// PauseMenu (Start; the field-action poll also needs it not held), MapMenu (the touch pad) and EnemyIntel.
inline constexpr std::uintptr_t kPauseMenuKey     = 0x1491E44C0;
inline constexpr std::uintptr_t kMapMenuKey       = 0x1491E45B0;
inline constexpr std::uintptr_t kEnemyIntelKey    = 0x1491E45C8;
/// The engine key L3 sends (EKeys::Gamepad_LeftThumbstick, an FKey: the FName first), keying its own raw-table element
/// with each pad's source bit, whatever action the key assignment binds to it (RE-pad-slots.md).
inline constexpr std::uintptr_t kLeftThumbstickKey = 0x149245C08;
/// InputKey24_CopyAddRef(out, key): copies a key, taking a reference on its name.
inline constexpr std::uint8_t kInputKeyCopyBytes[] = {0x48, 0x8B, 0x02, 0x48, 0x89, 0x01, 0x48, 0x8B,
                                                      0x42, 0x08, 0x48, 0x89, 0x41, 0x08, 0x48, 0x8B};
inline constexpr FunctionFact kInputKeyCopy{L"InputKey24_CopyAddRef", 0x14078C2F4, kInputKeyCopyBytes};
/// Square's input object (its stick fields and per-pad banks), and the byte that makes the merge
/// read DirectInput alone.
inline constexpr std::uintptr_t kSquareInputGlobal     = 0x1493496F0;
inline constexpr std::uintptr_t kDirectInputOnlyFlag   = 0x1490FF1C0;
inline constexpr std::uint32_t  kInputMergedSticks     = 0x1210;  // float LX, LY, RX, RY
inline constexpr std::uint32_t  kInputDeviceBanks      = 0x11D0;  // {Record* data; int32 num; int32 max}
inline constexpr std::uint32_t  kInputBankHeaderSize   = 16;
inline constexpr std::uint32_t  kXInputBank            = 1;
inline constexpr std::uint32_t  kDirectInputBank       = 2;

}  // namespace lss::facts
