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
// RE-p2-sticks-tags-ida.md, RE-p2-terrain-action-conflict.md §7.

#include "platform/verified_code.h"

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
// ActiveVehicleCoordinator (RE-p2-control-mode-object.md): the coordinator of the vehicle the controller's player rides.
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
inline constexpr std::uint8_t kLeaderActionTickBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x7C, 0x24, 0x10, 0x55, 0x48,
    0x8D, 0x6C, 0x24, 0xA9, 0xB8, 0xB0, 0x00, 0x00, 0x00, 0xE8, 0xAA, 0x2D};
inline constexpr SlotFact kLeaderActionTick{
    L"the leader-action component's tick slot (+0x428)", 0x145D6C490,
    {L"EndPlayerLeaderActionParamComponent::Tick_Vt428", 0x14125C16C, kLeaderActionTickBytes}};

// Square's input object
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
