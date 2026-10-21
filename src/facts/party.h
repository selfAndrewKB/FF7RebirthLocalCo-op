#pragma once

// The party: its manager, roster and leader, the characters and their AI, the tag registry, and the game globals
// that exist once the game has started (the option holder, the battle state).
//
// Addresses are as IDA shows them (image base 0x140000000). Each function's entry bytes are the first 24 bytes read in
// IDA; call and RIP-relative displacements are relative to the image, so they are identical in the running game. A
// stage is installed only if every fact it uses holds (platform/verified_code.h). Each offset was read at an
// instruction that uses it.
//
// Research: Rebirth/Documentation/research/RE-p2-build-facts-ida.md, RE-p2-deployed-party.md §4.1,
// RE-p2-party-switch.md §1.2, RE-p2-member-handover.md §3.3, RE-p2-sticks-tags-ida.md, RE-p2-classic-autoplay.md.

#include "platform/build_value.h"
#include "platform/verified_code.h"

#include <cstdint>

namespace lss::facts {

using platform::FunctionFact;
using platform::SlotFact;

// The party manager
/// The party manager (a native singleton): +0x140 is the primary player controller, an
/// FWeakObjectPtr {index, serial} read by GetPrimaryEndPlayerController (0x140A61D1C).
inline constexpr std::uintptr_t kPartyManagerGlobal            = 0x14939BCE0;
inline constexpr std::uint32_t  kPartyManagerPrimaryController = 0x140;
/// Party roster: lane 0 is the field lane; a type of 0x10 or more is "none".
inline constexpr std::uint8_t kFieldLane       = 0;
inline constexpr std::uint8_t kNoPlayerType    = 0x10;
inline constexpr std::int32_t kMaxRosterEntries = 16;
inline constexpr std::uint8_t kRosterCountBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x10, 0xF9, 0x66, 0x01,
    0x48, 0x2B, 0xE0, 0x4C, 0x8B, 0x05, 0xCE, 0x5F, 0x7E, 0x08, 0x45, 0x33};
inline constexpr FunctionFact kRosterCount{L"the party roster count", 0x140A3F614, kRosterCountBytes};
inline constexpr std::uint8_t kRosterPlayerTypeBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x1C, 0xFA, 0x66, 0x01,
    0x48, 0x2B, 0xE0, 0x4C, 0x8B, 0x05, 0xDA, 0x60, 0x7E, 0x08, 0x44, 0x8B};
inline constexpr FunctionFact kRosterPlayerType{L"the party roster's player type", 0x140A3F508, kRosterPlayerTypeBytes};
inline constexpr std::uint8_t kCharacterByPlayerTypeBytes[] = {
    0x4C, 0x8B, 0x05, 0xA5, 0xA6, 0x96, 0x08, 0x44, 0x0F, 0xB6, 0xC9, 0x4D,
    0x85, 0xC0, 0x0F, 0x84, 0x87, 0x00, 0x00, 0x00, 0x80, 0xFA, 0x0F, 0x73};
inline constexpr FunctionFact kCharacterByPlayerType{L"PartyManager::GetCharacterByPlayerType", 0x140A31634,
                                                     kCharacterByPlayerTypeBytes};
inline constexpr std::uint8_t kFieldLeaderTypeBytes[] = {
    0x48, 0x8B, 0x05, 0x9D, 0x3F, 0x7C, 0x08, 0x48, 0x85, 0xC0, 0x74, 0x1A,
    0x48, 0x8B, 0x48, 0x48, 0x48, 0x85, 0xC9, 0x75, 0x0D, 0x48, 0x8B, 0x48};
inline constexpr FunctionFact kFieldLeaderType{L"PartyData_GetFieldLeaderType", 0x140A61654, kFieldLeaderTypeBytes};
inline constexpr std::uint8_t kDefaultBattleLeaderTypeBytes[] = {
    0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xB6, 0xAC, 0xBA, 0x00, 0x48, 0x2B,
    0xE0, 0x48, 0x8B, 0x0D, 0x74, 0x13, 0xD2, 0x07, 0x48, 0x85, 0xC9, 0x74};
inline constexpr FunctionFact kDefaultBattleLeaderType{L"PartyData_GetDefaultBattleLeaderType", 0x141504270,
                                                       kDefaultBattleLeaderTypeBytes};
inline constexpr std::uint8_t kResolvePartyLeaderBytes[] = {
    0x48, 0x8B, 0x05, 0xC9, 0xAA, 0x93, 0x08, 0x45, 0x33, 0xC0, 0x48, 0x85,
    0xC0, 0x74, 0x70, 0x44, 0x8B, 0x88, 0x4C, 0x01, 0x00, 0x00, 0x45, 0x85};
inline constexpr FunctionFact kResolvePartyLeader{L"the party leader resolver", 0x140A61210, kResolvePartyLeaderBytes};
inline constexpr std::uint8_t kIsBattleMemberBytes[] = {
    0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xAA, 0xAE, 0xD8, 0x00, 0x48, 0x2B,
    0xE0, 0x44, 0x8A, 0xC9, 0xE8, 0x9B, 0xB6, 0x71, 0xFF, 0x3C, 0x06, 0x74};
inline constexpr FunctionFact kIsBattleMember{L"UEndPartyAPI_IsBattleMember", 0x14132407C, kIsBattleMemberBytes};
/// The battle-player component's "is the party leader" test returns (vt+0x68 player type == the
/// party manager's leader type); the movement gate's first character clause ends in it.
inline constexpr std::uint32_t kPlayerTypeSlotOffset = 0x68;
inline constexpr std::uint8_t kIsPartyLeaderBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x98, 0x7E, 0x8A, 0x01,
    0x48, 0x2B, 0xE0, 0x48, 0x8B, 0x05, 0x3E, 0x4C, 0xB9, 0x08, 0x48, 0x85};
inline constexpr SlotFact kIsPartyLeader{
    L"the battle-player component's leader-test slot (+0x70)", 0x145795F48,
    {L"IsPartyLeader_ForActor", 0x14080708C, kIsPartyLeaderBytes}};

// Characters (AEndCharacter)
inline constexpr std::uint32_t kCharacterRequestedController = 0x578;
inline constexpr std::uint32_t kCharacterEffectiveController = 0x580;
inline constexpr std::uint32_t kCharacterRetainedAi          = 0x588;
inline constexpr std::uint32_t kCharacterPlayerControlled    = 0x625;
// 1.005: EndCharacter tick 0x140E8A44A mov rcx,[rdi+1770h] (1.004 0x14125B20A [rdi+1750h]); GetMoveState
// 0x141985320.
inline constexpr platform::PerBuild<std::uint32_t> kCharacterMoveState = {0x1750, 0x1770};
inline constexpr std::uint8_t kIsEndCharacterBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x80, 0x00, 0x00, 0x00, 0xE8,
    0xD8, 0x58, 0x1B, 0x01, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0x05, 0xE6, 0x01};
inline constexpr FunctionFact kIsEndCharacter{L"IsEndCharacter", 0x140EF9648, kIsEndCharacterBytes};
// Player 1's battle switch's member test T (RE-p2-deployed-party.md §4.1, RE-p2-party-switch.md §1.2)
// as the game's own function (al only; 167 callers, so called, never hooked): the character is live,
// has its battle controller component, is not in the dead state (6) and is not petrified (id 22).
// And the two leader types the game hands back to player 1, and the deployed test itself.
inline constexpr std::uint8_t kIsBattleActiveBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x68, 0xED, 0x66, 0x01,
    0x48, 0x2B, 0xE0, 0x4C, 0x8B, 0xC1, 0x48, 0x85, 0xC9, 0x0F, 0x84, 0xA1};
inline constexpr FunctionFact kIsBattleActive{L"Character_IsBattleActive_NotDeadNotPetrified", 0x140A401BC,
                                              kIsBattleActiveBytes};

// The AI: enemy controllers, the active brain and its state
// Enemy AI controllers (EndAIControllerEnemyBase, both builds): an FWeakObjectPtr to the enemy character.
inline constexpr std::uint32_t kEnemyControllerCharacter = 0x464;
// The AI controller's active brain and its state (RE-p2-member-handover.md §3.3)
inline constexpr std::uint32_t kAiActiveBrain = 0x518;
inline constexpr std::uint32_t kBrainIndex    = 0x19;  // u8; 0 is the base brain
inline constexpr std::uint32_t kBrainState    = 0x20;
inline constexpr std::uint32_t kAiStateType   = 0x18;  // u8, EEndAIState
inline constexpr std::uint8_t kAiControllerComponentBytes[] = {
    0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x02, 0xF0, 0x6B, 0x01, 0x48, 0x2B,
    0xE0, 0x48, 0x8B, 0x89, 0x38, 0x06, 0x00, 0x00, 0x48, 0x85, 0xC9, 0x74};
inline constexpr FunctionFact kAiControllerComponent{L"EndCharacter::GetAIControllerComponent", 0x1409EFF24,
                                                     kAiControllerComponentBytes};
inline constexpr std::uint8_t kValidateAiControllerBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xA8, 0xEF, 0x6B, 0x01,
    0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9, 0x74, 0x1B, 0x48};
inline constexpr FunctionFact kValidateAiController{L"EndCharacter::ValidateAIController", 0x1409EFF7C,
                                                    kValidateAiControllerBytes};

// The tag registry
/// The tag registry: a weak pointer to the world's EndCharacterMovementActor, whose +0x278 is a
/// TMap<FName, int32> with 20-byte elements {FName, int32 count, ...}; and the trigger actor's tag.
inline constexpr std::uintptr_t kTagRegistryWeak         = 0x1470C94C0;
inline constexpr std::uint32_t  kTagRegistryMap          = 0x278;
inline constexpr std::uint32_t  kTagRegistryElementSize  = 20;
inline constexpr std::uint32_t  kTagRegistryCount        = 8;
inline constexpr std::uintptr_t kTriggerTagName          = 0x1491E3EE0;
/// The tag registry's read-only lookup: TSet<TPair<FName, int32>>::FindId.
inline constexpr std::uint8_t kTagSetFindIdBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x7C, 0x24, 0x10, 0x8B, 0x41, 0x08, 0x48, 0x8B, 0xFA};
inline constexpr FunctionFact kTagSetFindId{L"the tag registry lookup", 0x1408428F0, kTagSetFindIdBytes};

// Game globals: the option holder and the battle state
/// Two game objects that exist once the game has started and are missing at the title and early in
/// a load: the holder of the system option data ([it]+0x100, RE-p2-classic-autoplay.md) and the
/// battle system (IsInBattle reads [it]).
inline constexpr std::uintptr_t kOptionHolderGlobal     = 0x14939BD58;
inline constexpr std::uint32_t  kOptionHolderData       = 0x100;
inline constexpr std::uintptr_t kBattleStateGlobal      = 0x14939BC58;
inline constexpr std::uint8_t kIsInBattleBytes[] = {
    0x48, 0x8B, 0x05, 0x25, 0x98, 0xAB, 0x08, 0x33, 0xC9, 0x38, 0x88, 0x00,
    0x03, 0x00, 0x00, 0x75, 0x2A, 0x48, 0x8B, 0x05, 0xA4, 0xC9, 0x81, 0x08};
inline constexpr FunctionFact kIsInBattle{L"UEndBattleAPI::IsInBattle", 0x1408E242C, kIsInBattleBytes};

}  // namespace lss::facts
