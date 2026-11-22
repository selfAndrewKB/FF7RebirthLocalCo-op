#pragma once

// The party: its manager, roster and leader, the characters and their AI, the tag registry, and the game globals
// that exist once the game has started (the option holder, the battle state).
//
// Addresses are as IDA shows them (image base 0x140000000). Each function's entry bytes are the first 24 bytes read in
// IDA; call and RIP-relative displacements are relative to the image, so they are identical in the running game. A
// stage is installed only if every fact it uses holds (platform/verified_code.h). Each offset was read at an
// instruction that uses it.
//
// Research: Rebirth/Documentation/research/RE-p2-build-facts-ida.md, RE-p2-deployed-party.md,
// RE-p2-party-switch.md, RE-p2-member-handover.md, RE-p2-sticks-tags-ida.md, RE-p2-classic-autoplay.md.

#include "platform/build_value.h"
#include "platform/verified_code.h"

#include <cstdint>

namespace lss::facts {

using platform::FunctionFact;
using platform::SlotFact;

// The party manager
/// The party manager (a native singleton): +0x140 is the primary player controller, an FWeakObjectPtr {index, serial}
/// read by GetPrimaryEndPlayerController_FromPartyManager (its only read, RE-p2-camera-movement-ida.md); +0x148
/// the leader (an FWeakObjectPtr, read by ResolvePartyLeaderWeakPtr); +0x150 the leader's player type (u8).
inline constexpr std::uintptr_t kPartyManagerGlobal            = 0x1491D8510;
inline constexpr std::uint32_t  kPartyManagerPrimaryController = 0x140;
inline constexpr std::uint32_t  kPartyManagerLeader            = 0x148;
inline constexpr std::uint32_t  kPartyManagerLeaderType        = 0x150;
/// GetPrimaryEndPlayerController_FromPartyManager: a redirect target only; the call sites it is placed at prove it.
inline constexpr std::uintptr_t kGetPrimaryEndPlayerController = 0x140763D40;
/// UEndPartyAPI_GetPartyLeaderType (a leaf): PM ? PM[+0x150] : 16, in al (RE-p2-enemy-target-bias.md).
inline constexpr std::uint8_t kGetPartyLeaderTypeBytes[] = {0x48, 0x8B, 0x05, 0xD5, 0xA3, 0x8A, 0x08, 0x48, 0x85, 0xC0,
                                                            0x74, 0x07, 0x8A, 0x80, 0x50, 0x01, 0x00, 0x00, 0xC3, 0xB0,
                                                            0x10, 0xC3};
inline constexpr FunctionFact kGetPartyLeaderType{L"UEndPartyAPI_GetPartyLeaderType", 0x14092E134,
                                                  kGetPartyLeaderTypeBytes};
/// Party_IsTypeDead_AIHolder6(unused, u8 type): the member of `type`'s control state is Dead (6); the type in dl
/// (RE-downed-ally-commands.md).
inline constexpr std::uint8_t kIsTypeDeadBytes[] = {0x40, 0x53, 0xB8, 0x80, 0x00, 0x00, 0x00};
inline constexpr FunctionFact kIsTypeDead{L"Party_IsTypeDead_AIHolder6", 0x140C24A3C, kIsTypeDeadBytes};
/// PartyType_HasStateId(u8 type, u8 status): the member of `type` has the status; the battle switch's own member test
/// and the command menu's gates ask it for Petrify (RE-downed-ally-commands.md, RE-p2-party-switch.md).
inline constexpr std::uint8_t kTypeHasStateIdBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                                        0x24, 0x10, 0x57, 0xB8, 0x80, 0x00, 0x00, 0x00};
inline constexpr FunctionFact kTypeHasStateId{L"PartyType_HasStateId", 0x1409B047C, kTypeHasStateIdBytes};
inline constexpr std::uint8_t kPetrifyStateId = 22;
/// Party_GetHpByType(u8 type): the member's HP, 0 for a knocked-out member (the command chooser's and the low-health
/// decision's read).
inline constexpr std::uint8_t kPartyHpBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20,
                                                 0x00, 0x00, 0x00, 0xE8, 0x20, 0x61, 0x54, 0x01};
inline constexpr FunctionFact kPartyHp{L"Party_GetHpByType", 0x140C25A50, kPartyHpBytes};
/// Party roster: lane 0 is the field lane; a type of 0x10 or more is "none".
inline constexpr std::uint8_t kFieldLane       = 0;
inline constexpr std::uint8_t kNoPlayerType    = 0x10;
inline constexpr std::int32_t kMaxRosterEntries = 16;
inline constexpr std::uint8_t kRosterCountBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x4C, 0xC0, 0x7B, 0x01,
    0x48, 0x2B, 0xE0, 0x4C, 0x8B, 0x05, 0x2A, 0xDC, 0x6A, 0x08, 0x45, 0x33};
inline constexpr FunctionFact kRosterCount{L"the party roster count", 0x1409AFB28, kRosterCountBytes};
inline constexpr std::uint8_t kRosterPlayerTypeBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x58, 0xC1, 0x7B, 0x01,
    0x48, 0x2B, 0xE0, 0x4C, 0x8B, 0x05, 0x36, 0xDD, 0x6A, 0x08, 0x44, 0x8B};
inline constexpr FunctionFact kRosterPlayerType{L"the party roster's player type", 0x1409AFA1C, kRosterPlayerTypeBytes};
inline constexpr std::uint8_t kCharacterByPlayerTypeBytes[] = {
    0x4C, 0x8B, 0x05, 0x21, 0xA3, 0x82, 0x08, 0x44, 0x0F, 0xB6, 0xC9, 0x4D,
    0x85, 0xC0, 0x0F, 0x84, 0x87, 0x00, 0x00, 0x00, 0x80, 0xFA, 0x0F, 0x73};
inline constexpr FunctionFact kCharacterByPlayerType{L"PartyManager::GetCharacterByPlayerType", 0x1409AE1E8,
                                                     kCharacterByPlayerTypeBytes};
inline constexpr std::uint8_t kFieldLeaderTypeBytes[] = {
    0x48, 0x8B, 0x05, 0xF9, 0x8B, 0x8F, 0x08, 0x48, 0x85, 0xC0, 0x74, 0x1A,
    0x48, 0x8B, 0x48, 0x48, 0x48, 0x85, 0xC9, 0x75, 0x0D, 0x48, 0x8B, 0x48};
inline constexpr FunctionFact kFieldLeaderType{L"PartyData_GetFieldLeaderType", 0x140764B68, kFieldLeaderTypeBytes};
inline constexpr std::uint8_t kDefaultBattleLeaderTypeBytes[] = {
    0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x92, 0x31, 0x5E, 0x01, 0x48, 0x2B,
    0xE0, 0x48, 0x8B, 0x0D, 0x70, 0x4D, 0x4D, 0x08, 0x48, 0x85, 0xC9, 0x74};
inline constexpr FunctionFact kDefaultBattleLeaderType{L"PartyData_GetDefaultBattleLeaderType", 0x140B889E4,
                                                       kDefaultBattleLeaderTypeBytes};
inline constexpr std::uint8_t kResolvePartyLeaderBytes[] = {
    0x48, 0x8B, 0x05, 0x61, 0x3B, 0xA7, 0x08, 0x45, 0x33, 0xC0, 0x48, 0x85,
    0xC0, 0x74, 0x70, 0x44, 0x8B, 0x88, 0x4C, 0x01, 0x00, 0x00, 0x45, 0x85};
inline constexpr FunctionFact kResolvePartyLeader{L"the party leader resolver", 0x1407649A8, kResolvePartyLeaderBytes};
/// PartyManager_ResolveFieldLeaderCharOrLeader(bool fieldLeader) -> character: the party data's field leader (1) or the
/// party leader (0).
inline constexpr std::uint8_t kResolveFieldLeaderBytes[] = {
    0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x2E, 0x7B, 0xA0, 0x01, 0x48, 0x2B,
    0xE0, 0x48, 0x8B, 0x15, 0xB4, 0x44, 0xA7, 0x08, 0x48, 0x85, 0xD2, 0x74};
inline constexpr FunctionFact kResolveFieldLeader{L"PartyManager_ResolveFieldLeaderCharOrLeader", 0x140764048,
                                                  kResolveFieldLeaderBytes};
inline constexpr std::uint8_t kIsBattleMemberBytes[] = {
    0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xC6, 0xD5, 0xFB, 0x00, 0x48, 0x2B,
    0xE0, 0x44, 0x8A, 0xC9, 0xE8, 0x73, 0x16, 0x80, 0xFF, 0x3C, 0x06, 0x74};
inline constexpr FunctionFact kIsBattleMember{L"UEndPartyAPI_IsBattleMember", 0x1411AE5B0, kIsBattleMemberBytes};
/// The battle-player component's "is the party leader" test returns (vt+0x68 player type == the
/// party manager's leader type); the movement gate's first character clause ends in it.
inline constexpr std::uint32_t kPlayerTypeSlotOffset = 0x68;
inline constexpr std::uint8_t kIsPartyLeaderBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x78, 0xDA, 0x83, 0x01,
    0x48, 0x2B, 0xE0, 0x48, 0x8B, 0x05, 0xFE, 0xA3, 0x8A, 0x08, 0x48, 0x85};
inline constexpr SlotFact kIsPartyLeader{
    L"the battle-player component's leader-test slot (+0x70)", 0x1456117B0,
    {L"IsPartyLeader_ForActor", 0x14092E0FC, kIsPartyLeaderBytes}};

// Characters (AEndCharacter)
inline constexpr std::uint32_t kCharacterRequestedController = 0x578;
inline constexpr std::uint32_t kCharacterEffectiveController = 0x580;
inline constexpr std::uint32_t kCharacterRetainedAi          = 0x588;
inline constexpr std::uint32_t kCharacterPlayerControlled    = 0x625;
// The character's EndActorInterface (IEndActorInterface; the same offset and slots in both builds) and its actor type
// (EActorType: the game's leader PLAYER, every other party member PARTY), read and set through the interface
// (research/RE-actor-type-on-hand.md). The setter re-registers the character's per-type invisible walls, so the type is
// never written as a byte.
inline constexpr std::uint32_t kCharacterActorInterface = 0x4B8;
inline constexpr std::uint32_t kActorTypeGetSlot        = 0x1B0;  // u8 (interface)
inline constexpr std::uint32_t kActorTypeSetSlot        = 0x1B8;  // void (interface, u8 type)
inline constexpr std::uint8_t  kActorTypePlayer         = 0;
inline constexpr std::uint8_t  kActorTypeParty          = 1;
// The character's control holder (IsUCPC_Core: mov rcx,[rcx+638h]): its vt+0x40 gives the input component, its
// vt+0x48 the BattlePlayer component (BattlePlayerControllerComp, whose vtable's +0x70 is the leader test above).
inline constexpr std::uint32_t  kCharacterControlHolder      = 0x638;
inline constexpr std::uintptr_t kBattlePlayerComponentVTable = 0x145611740;
/// The holder's character (a weak) and the action controller the holder makes on demand (its +8 is the holder). The
/// field attack's request on that controller: (ctrl, hold, throw) -> 1 requested, 0 refused while the move owner runs
/// a behaviour action that refuses it; the request switches the character's control state synchronously
/// (RE-yuffie-box-throw.md).
inline constexpr std::uint32_t kCharCtrlHolderCharacter = 0x8;
inline constexpr std::uint32_t kCharActionCtrlHolder    = 0x8;
/// The holder's control state (u8, EEndCharacterControllerStateType, written by its state switch): 1 Idle; an attack,
/// ability, guard, dodge or hit reaction is another (RE-p1-warp-to-fight.md).
inline constexpr std::uint32_t kCharCtrlHolderState = 0x68;
inline constexpr std::uint8_t  kControlStateIdle    = 1;
inline constexpr std::uint8_t  kCharActionCtrlBeginFieldAttackBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                                                         0x24, 0x10, 0x57, 0xB8, 0x70, 0x00, 0x00, 0x00};
inline constexpr FunctionFact kCharActionCtrlBeginFieldAttack{L"CharActionCtrl_Vt70_BeginFieldAttack_SetsThrow21", 0x14150F718,
                                                              kCharActionCtrlBeginFieldAttackBytes};
// 1.005: EndCharacter tick 0x140E8A44A mov rcx,[rdi+1770h] (1.004 0x14125B20A [rdi+1750h]); GetMoveState
// 0x141985320.
inline constexpr platform::PerBuild<std::uint32_t> kCharacterMoveState = {0x1770, 0x1750};
inline constexpr std::uint8_t kIsEndCharacterBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x80, 0x00, 0x00, 0x00, 0xE8,
    0x10, 0x65, 0x80, 0x00, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0x05, 0xCE, 0xD5};
inline constexpr FunctionFact kIsEndCharacter{L"IsEndCharacter", 0x141965660, kIsEndCharacterBytes};
// Player 1's battle switch's member test T (RE-p2-deployed-party.md, RE-p2-party-switch.md)
// as the game's own function (al only; 167 callers, so called, never hooked): the character is live,
// has its battle controller component, is not in the dead state (6) and is not petrified (id 22).
// And the two leader types the game hands back to player 1, and the deployed test itself.
inline constexpr std::uint8_t kIsBattleActiveBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xB4, 0x62, 0x54, 0x01,
    0x48, 0x2B, 0xE0, 0x4C, 0x8B, 0xC1, 0x48, 0x85, 0xC9, 0x0F, 0x84, 0xA1};
inline constexpr FunctionFact kIsBattleActive{L"Character_IsBattleActive_NotDeadNotPetrified", 0x140C258C0,
                                              kIsBattleActiveBytes};
/// AEndCharacter::SwitchAIControl_Async(character, toPlayer, controller): on the game thread it possesses at once
/// (the rides and the synergy partner call it).
inline constexpr std::uint8_t kSwitchAiControlAsyncBytes[] = {
    0x40, 0x55, 0x48, 0x8D, 0x6C, 0x24, 0xA9, 0xB8, 0x90, 0x00, 0x00, 0x00,
    0xE8, 0x47, 0x28, 0xD5, 0x00, 0x48, 0x2B, 0xE0, 0x80, 0x3D, 0xD6, 0x72};
inline constexpr FunctionFact kSwitchAiControlAsync{L"AEndCharacter::SwitchAIControl_Async", 0x141419328,
                                                    kSwitchAiControlAsyncBytes};
/// EndCharacter_GetPlayerTypeFromComponent: the player type of a character's battle component (al).
inline constexpr std::uint8_t kPlayerTypeFromComponentBytes[] = {
    0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xEE, 0x92, 0x7B, 0x01, 0x48, 0x2B,
    0xE0, 0x48, 0x8B, 0x89, 0x38, 0x06, 0x00, 0x00, 0x48, 0x85, 0xC9, 0x74};
inline constexpr FunctionFact kPlayerTypeFromComponent{L"EndCharacter_GetPlayerTypeFromComponent", 0x1409B2888,
                                                       kPlayerTypeFromComponentBytes};

// The AI: enemy controllers, the active brain and its state, the follow state
/// The AI follow state's player-subject class (vtable, data): FollowState_TickTeleportEQS at its +0x58, ForceTeleport
/// +0x270, FinishForcedTeleportNow +0x278 (world/party_warp).
inline constexpr std::uintptr_t kFollowStatePlayerVTable = 0x145657190;
// Enemy AI controllers (EndAIControllerEnemyBase, both builds): an FWeakObjectPtr to the enemy character.
inline constexpr std::uint32_t kEnemyControllerCharacter = 0x464;
// The AI controller's active brain and its state (RE-p2-member-handover.md)
inline constexpr std::uint32_t kAiActiveBrain = 0x518;
inline constexpr std::uint32_t kBrainIndex    = 0x19;  // u8; 0 is the base brain
inline constexpr std::uint32_t kBrainState    = 0x20;
inline constexpr std::uint32_t kAiStateType   = 0x18;  // u8, EEndAIState
inline constexpr std::uint32_t kAiPauseMask   = 0x460; // u32 (RE-p2-party-warp.md, the sampler table)
/// EEndAIState, the state's type byte (the reflected enum, FF7R2UProj/Source/EndGame/Public/EEndAIState.h).
enum AiState : std::uint8_t
{
    kAiStateEmpty            = 0,
    kAiStateDoNothing        = 1,  // the scene parks' state, among others
    kAiStateConnectEncount   = 2,
    kAiStateBattle           = 3,
    kAiStateStay             = 4,
    kAiStateMoveTo           = 5,  // a run to a mark; fires its arrival trigger
    kAiStateLead             = 6,  // fires its arrival trigger
    kAiStateFollow           = 7,  // the party follow, the base brain's default
    kAiStateWandering        = 8,
    kAiStateAnimation        = 9,
    kAiStateSingleAnimation  = 10,
    kAiStateUnique           = 11,
    kAiStateInterest         = 12,
    kAiStateWildAnimal       = 13,
    kAiStateBattleEvacuation = 14,
    kAiStateHintAnimal       = 15,
    kAiStateFieldAction      = 16,
};
/// Its bit for pause reason 13, the animation budget's: a mesh the budget throttles to every other frame is told
/// "tick / don't tick" on alternate frames and this bit follows it (RE-ai-pause-bit.md); no AI decision reads it on
/// a possessed member. The probes read the mask without it.
inline constexpr std::uint32_t kAiPauseAnimationBudget = 0x2000;
// The follow state's teleport (RE-p2-party-warp.md): the range it allows, and its forced request pending (set by
// ForceTeleport, cleared by the commit).
inline constexpr std::uint32_t kFollowStateTeleportRange = 0x344;  // float
inline constexpr std::uint32_t kFollowStateForced        = 0x39D;  // u8
// Its "must not teleport now" flag (u8; the lifts' hold, read by its CanTeleportNow), written only by the game's setter
// Pawn_SetFollowStateNoTeleport6CC_IfFollow and the state's constructor (RE-chocobo-summon-gather.md).
inline constexpr std::uint32_t kFollowStateNoTeleport    = 0x6CC;
inline constexpr std::uint8_t kAiControllerComponentBytes[] = {
    0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x1A, 0x92, 0x7B, 0x01, 0x48, 0x2B,
    0xE0, 0x48, 0x8B, 0x89, 0x38, 0x06, 0x00, 0x00, 0x48, 0x85, 0xC9, 0x74};
inline constexpr FunctionFact kAiControllerComponent{L"EndCharacter::GetAIControllerComponent", 0x1409B295C,
                                                     kAiControllerComponentBytes};
inline constexpr std::uint8_t kValidateAiControllerBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xC0, 0x91, 0x7B, 0x01,
    0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9, 0x74, 0x1B, 0x48};
inline constexpr FunctionFact kValidateAiController{L"EndCharacter::ValidateAIController", 0x1409B29B4,
                                                    kValidateAiControllerBytes};

// The tag registry
/// The tag registry: a weak pointer to the world's EndCharacterMovementActor (the trigger registry, FieldTick's `this`;
/// world/seat_triggers), whose +0x278 is a TMap<FName, int32> with 20-byte elements {FName, int32 count, ...}; and
/// the trigger actor's tag.
inline constexpr std::uintptr_t kTagRegistryWeak         = 0x146EFC6E0;
inline constexpr std::uint32_t  kTagRegistryMap          = 0x278;
inline constexpr std::uint32_t  kTagRegistryElementSize  = 20;
inline constexpr std::uint32_t  kTagRegistryCount        = 8;
inline constexpr std::uintptr_t kTriggerTagName          = 0x14901BFB8;
/// The tag registry's read-only lookup: TSet<TPair<FName, int32>>::FindId.
inline constexpr std::uint8_t kTagSetFindIdBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x7C, 0x24, 0x10, 0x8B, 0x41, 0x08, 0x48, 0x8B, 0xFA};
inline constexpr FunctionFact kTagSetFindId{L"the tag registry lookup", 0x14078CD70, kTagSetFindIdBytes};

// Battle targeting: the Condition target selector's select, bool(selector, u8 reason, UObject** out), in its vtable's
// slot +0x88 (the out is written on every path; 0 keeps the current target). RE-p2-enemy-target-bias.md; the slot
// is replaced by battle/equal_targeting, watched by target_census.
inline constexpr std::uint8_t kCondTargetSelectBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57,
                                                          0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
inline constexpr SlotFact kCondTargetSelect{L"the Condition target selector's select slot (vt+0x88)", 0x14566CB18,
                                            {L"CondTargetSel_Select_Vt88", 0x141113AF8, kCondTargetSelectBytes}};

// Game globals: the option holder and the battle state
/// Two game objects that exist once the game has started and are missing at the title and early in
/// a load: the holder of the system option data ([it]+0x100, RE-p2-classic-autoplay.md) and the
/// battle system (IsInBattle reads [it]).
inline constexpr std::uintptr_t kOptionHolderGlobal     = 0x1491D8580;
inline constexpr std::uint32_t  kOptionHolderData       = 0x100;
inline constexpr std::uintptr_t kBattleStateGlobal      = 0x1491D8468;
inline constexpr std::uint8_t kIsInBattleBytes[] = {
    0x48, 0x8B, 0x05, 0x51, 0x77, 0x8A, 0x08, 0x33, 0xC9, 0x38, 0x88, 0x00,
    0x03, 0x00, 0x00, 0x75, 0x2A, 0x48, 0x8B, 0x05, 0x90, 0x18, 0x60, 0x08};
inline constexpr FunctionFact kIsInBattle{L"UEndBattleAPI::IsInBattle", 0x140930D10, kIsInBattleBytes};

}  // namespace lss::facts
