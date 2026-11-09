#pragma once

// The level's state manager as the lead gate reads it: the state-change and state-trigger rows, the active slots, the
// AI system's setting groups, the field icons the story shows, and the AI states a level's walk or lead enters on a
// member. 1.004 addresses (1.005 through replaced-walk-gate.json); every offset is the same on both builds.
//
// Research: Rebirth/Documentation/research/RE-walk-gate.md, RE-held-member-scripts.md, RE-statechange-row-layout.md,
// RE-walk-keep.md.

#include "platform/verified_code.h"

#include <cstddef>
#include <cstdint>

namespace lss::facts {

using platform::FunctionFact;

// ---- The row accessors (DataAcc_<Table>_Ctor(acc, FName)): 32 bytes of caller storage, the row at +8 ----
inline constexpr std::uint8_t kRowAccessorBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00};  // push rbx; mov eax, 20h
inline constexpr FunctionFact kStateChangeRow{L"DataAcc_StateChange_Ctor", 0x14087B56C, kRowAccessorBytes};
inline constexpr FunctionFact kStateTriggerRow{L"DataAcc_StateTrigger_Ctor", 0x140F7A0C8, kRowAccessorBytes};

// ---- The state manager ([g_EndGameRoot]+0x170, facts/world.h kRootGameState) ----
/// GS_CollectActiveSlotKeys(out): `out` fresh storage of 24 bytes (it never frees what was there); a TArray<int32> of
/// every active slot's key, reserved to 1 MB through the game's allocator: freed with kFreeArrayData every call.
inline constexpr std::uint8_t kCollectActiveSlotKeysBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48,
                                                               0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48};
inline constexpr FunctionFact kCollectActiveSlotKeys{L"GS_CollectActiveSlotKeys", 0x140973D74, kCollectActiveSlotKeysBytes};
/// GS_GetObjectSlot_ByIndex(SM, key): the slot, or null when the key is not in the map (key 0 answers element 0).
inline constexpr std::uint8_t kObjectSlotByKeyBytes[] = {0x8B, 0xC2, 0x44, 0x8B, 0xC2, 0x25, 0x00, 0x00,
                                                         0x00, 0xC0, 0x4C, 0x8B, 0xD1, 0x3D, 0x00, 0x00};
inline constexpr FunctionFact kObjectSlotByKey{L"GS_GetObjectSlot_ByIndex", 0x140973AD8, kObjectSlotByKeyBytes};
/// g_StateTriggerLock: the CRITICAL_SECTION every state-trigger send runs under (recursive).
inline constexpr std::uintptr_t kStateTriggerLock = 0x1491B33B0;
inline constexpr std::int32_t   kMaxActiveSlots   = 0x40000;  // the collector's own reserve
// A slot: its owner object's FName and its current state row's FName.
inline constexpr std::uint32_t kSlotOwner = 0x08;
inline constexpr std::uint32_t kSlotState = 0x10;

// ---- A StateChange row (mapped; data_rows) ----
inline constexpr std::uint32_t kStateRowNext       = 0;    // NextStateChange: 16-byte strings, one per trigger
inline constexpr std::uint32_t kStateRowSwaps      = 32;   // SwapStateChange: FNames
inline constexpr std::uint32_t kStateRowStringArgs = 80;   // StringArgumentList: FNames ([0] a group, [1] the point, [2] the trigger)
inline constexpr std::uint32_t kStateRowTriggers   = 96;   // StateTriggerList: FNames, paired with NextStateChange
inline constexpr std::uint32_t kStateRowStatus     = 176;  // Status: a mapped string, compared without case
inline constexpr std::int32_t  kStateRowTriggersRead = 16;  // the game reads at most 16
inline constexpr std::int32_t  kStateRowSwapsRead    = 8;
inline constexpr std::size_t   kStateRowDestinationArg = 1;
inline constexpr std::size_t   kStateRowTriggerArg     = 2;

// ---- A StateTrigger row (mapped) ----
inline constexpr std::uint32_t kTriggerRowObjects     = 0x00;  // ObjectIDList: the direct sets' objects (FNames)
inline constexpr std::uint32_t kTriggerRowSendObjects = 0x40;  // SendObject: who a SpecificObject trigger reaches
inline constexpr std::uint32_t kTriggerRowScope       = 0x80;  // Target (u8): 0 Self, 1 Location, 2 ResidentAndLocation, 3 SpecificObject
inline constexpr std::int32_t  kTriggerRowDirectSetsMax  = 32;
inline constexpr std::int32_t  kTriggerRowSendObjectsMax = 256;
inline constexpr std::uint8_t  kTriggerScopeSelf     = 0;
inline constexpr std::uint8_t  kTriggerScopeSpecific = 3;

// ---- The AI system's setting groups (EndAISystem_GetFromGameModeWorld()+0x540, a TMap<FName, TArray<entry>>) ----
inline constexpr std::uint8_t kAiSystemBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00};
inline constexpr FunctionFact kAiSystem{L"EndAISystem_GetFromGameModeWorld", 0x140C92B2C, kAiSystemBytes};
inline constexpr std::uint32_t kAiSystemGroups     = 0x540;
inline constexpr std::uint32_t kGroupPairStride    = 32;   // {FName key, entry* data +8, i32 count +16, ...}
inline constexpr std::uint32_t kGroupPairData      = 8;
inline constexpr std::uint32_t kGroupPairCount     = 16;
inline constexpr std::uint32_t kGroupEntryStride   = 160;
inline constexpr std::uint32_t kGroupEntryRow      = 16;   // the AIStateGroup row
inline constexpr std::uint32_t kGroupRowState      = 356;  // u8 EEndAIState
inline constexpr std::uint32_t kGroupRowRepIndex   = 0x100;  // StringArgumentRepIndex: mapped i8 array
inline constexpr std::int32_t  kGroupStatesRead    = 16;

// ---- The AI states a level's walk or lead enters (the brain's state, facts/party.h kBrainState) ----
// The setting's level object, row and arrival trigger (FNames), and its point (world cm), filled at the state's
// creation, before its Enter, held or not.
inline constexpr std::uint32_t kMoveToObject  = 0x20C;
inline constexpr std::uint32_t kMoveToRow     = 0x214;
inline constexpr std::uint32_t kMoveToTrigger = 0x21C;  // 0 for a plain MoveToTarget row: its string argument 2
inline constexpr std::uint32_t kMoveToPoint   = 0x360;
inline constexpr std::uint32_t kLeadObject    = 0x1BC;
inline constexpr std::uint32_t kLeadRow       = 0x1C4;
inline constexpr std::uint32_t kLeadTrigger   = 0x1CC;
inline constexpr std::uint32_t kLeadPoint     = 0x318;

// ---- The field icons (g_FieldIconSystem, a pointer; its +0x500 the icon manager) ----
inline constexpr std::uintptr_t kFieldIconSystem   = 0x14939BC78;
inline constexpr std::uint32_t  kIconSystemManager = 0x500;
inline constexpr std::uint32_t  kIconManagerKeys   = 0x1E0;  // TSparseArray of 20-byte {FName key, i32 index +8}
inline constexpr std::uint32_t  kIconKeyStride     = 20;
inline constexpr std::uint32_t  kIconKeyIndex      = 8;
inline constexpr std::uint32_t  kIconManagerSlots  = 0x280;  // pointer to 64-byte slots, the object at +8
inline constexpr std::uint32_t  kIconSlotStride    = 64;
inline constexpr std::uint32_t  kIconSlotObject    = 8;
inline constexpr std::uint32_t  kIconObjectKind    = 0x40;   // u8 EEndMenuMapIconKind (IconType_Array[0])
inline constexpr std::uint32_t  kIconObjectRow     = 0x20;   // the MapIconInfo row
inline constexpr std::uint32_t  kIconObjectShown   = 0xD0;   // u8: shown this update
inline constexpr std::uint32_t  kIconRowObjectId   = 200;    // FName: what it rides on
inline constexpr std::uint32_t  kIconRowStateType  = 297;    // u8: 3 a story icon
inline constexpr std::uint32_t  kIconRowObjectType = 302;    // u8: 2 a level object
inline constexpr std::uint8_t   kIconKindMainStory             = 2;
inline constexpr std::uint8_t   kIconKindMainStoryIntermediate = 11;
inline constexpr std::uint8_t   kIconStateTypeStory            = 3;
inline constexpr std::uint8_t   kIconObjectTypeLevelObject     = 2;

}  // namespace lss::facts
