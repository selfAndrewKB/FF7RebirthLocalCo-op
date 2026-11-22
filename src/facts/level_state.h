#pragma once

// The level's state manager as the lead gate reads it: the state-change and state-trigger rows, the active slots, the
// AI system's setting groups, the field icons the story shows, and the AI states a level's walk or lead enters on a
// member, and the field's named points. 1.004 addresses (1.005 through replaced-walk-gate.json and
// replaced-lead-relay.json); every offset is the same on both builds.
//
// Research: Rebirth/Documentation/research/RE-walk-gate.md, RE-held-member-scripts.md, RE-statechange-row-layout.md,
// RE-walk-keep.md, RE-lead-relay-point.md.

#include "platform/verified_code.h"

#include <cstddef>
#include <cstdint>

namespace lss::facts {

using platform::FunctionFact;

// ---- The row accessors (DataAcc_<Table>_Ctor(acc, FName)): 32 bytes of caller storage, the row at +8 ----
inline constexpr std::uint8_t kRowAccessorBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00};  // push rbx; mov eax, 20h
inline constexpr FunctionFact kStateChangeRow{L"DataAcc_StateChange_Ctor", 0x1409A891C, kRowAccessorBytes};
inline constexpr FunctionFact kStateTriggerRow{L"DataAcc_StateTrigger_Ctor", 0x14196FF7C, kRowAccessorBytes};

// ---- The state manager ([g_EndGameRoot]+0x170, facts/world.h kRootGameState) ----
/// GS_CollectActiveSlotKeys(out): `out` fresh storage of 24 bytes (it never frees what was there); a TArray<int32> of
/// every active slot's key, reserved to 1 MB through the game's allocator: freed with kFreeArrayData every call.
inline constexpr std::uint8_t kCollectActiveSlotKeysBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48,
                                                               0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48};
inline constexpr FunctionFact kCollectActiveSlotKeys{L"GS_CollectActiveSlotKeys", 0x1409AC184, kCollectActiveSlotKeysBytes};
/// GS_GetObjectSlot_ByIndex(SM, key): the slot, or null when the key is not in the map (key 0 answers element 0).
inline constexpr std::uint8_t kObjectSlotByKeyBytes[] = {0x8B, 0xC2, 0x44, 0x8B, 0xC2, 0x25, 0x00, 0x00,
                                                         0x00, 0xC0, 0x4C, 0x8B, 0xD1, 0x3D, 0x00, 0x00};
inline constexpr FunctionFact kObjectSlotByKey{L"GS_GetObjectSlot_ByIndex", 0x1409AC0B4, kObjectSlotByKeyBytes};
/// g_StateTriggerLock: the CRITICAL_SECTION every state-trigger send runs under (recursive).
inline constexpr std::uintptr_t kStateTriggerLock = 0x14864FC80;
inline constexpr std::int32_t   kMaxActiveSlots   = 0x40000;  // the collector's own reserve
// A slot: its owner object's FName and its current state row's FName.
inline constexpr std::uint32_t kSlotOwner = 0x08;
inline constexpr std::uint32_t kSlotState = 0x10;
/// GS_FindObjectByKeyAndAttribute(SM, FName object, attribute): the object's slot for that attribute, or null.
inline constexpr std::uint8_t kFindObjectByKeyAndAttributeBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x54,
                                                                     0x24, 0x10, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
inline constexpr FunctionFact kFindObjectByKeyAndAttribute{L"GS_FindObjectByKeyAndAttribute", 0x140C23D4C,
                                                           kFindObjectByKeyAndAttributeBytes};
/// GSObjectSlot_GetOffCode(slot): the slot's on/off code (its linked base entry's, else its own); 1 = switched off.
inline constexpr std::uint8_t kSlotOffCodeBytes[] = {0x48, 0x8B, 0x05, 0x85, 0xBB, 0x58, 0x08, 0x4C,
                                                     0x8B, 0xD9, 0x44, 0x8B, 0x41, 0x38};
inline constexpr FunctionFact kSlotOffCode{L"GSObjectSlot_GetOffCode", 0x1409A6A2C, kSlotOffCodeBytes};
inline constexpr std::uint8_t kSlotOff = 1;

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

// ---- The field's trigger boxes ([g_EndGameRoot]+0x178 the trigger manager; RE-member-story-boxes.md) ----
inline constexpr std::uint32_t kRootTriggerManager = 0x178;
/// FieldStateTrigger_OnColliderBeginOverlap(triggerMgr, FName volumeTag, AActor* collider) and its end twin: a
/// character's own tracker walking into (out of) a box. The enter's first instruction, `mov [rsp+8], rbx`, is
/// position-independent and starts its 16-byte block in neither build at a crossing.
inline constexpr std::uint8_t kBoxBeginFirst[] = {0x48, 0x89, 0x5C, 0x24, 0x08};
inline constexpr std::uint8_t kBoxBeginBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x54, 0x24,
                                                  0x10, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57};
inline constexpr FunctionFact kBoxBegin{L"FieldStateTrigger_OnColliderBeginOverlap", 0x141173484, kBoxBeginBytes};
inline constexpr std::uint8_t kBoxEndBytes[] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x53, 0x57, 0x41,
                                                0x55, 0x41, 0x56, 0x48, 0x8B, 0xEC, 0xB8, 0x30};
inline constexpr FunctionFact kBoxEnd{L"FieldStateTrigger_OnColliderEndOverlap", 0x14117377C, kBoxEndBytes};
/// LayoutVolumeTag_FindObjectName(FName* out, const FName* volumeTag): the box's layout object (0: none).
inline constexpr std::uint8_t kBoxLayoutObjectBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10};
inline constexpr FunctionFact kBoxLayoutObject{L"LayoutVolumeTag_FindObjectName", 0x141174CB4, kBoxLayoutObjectBytes};
/// DataObjRegistry_FindRowByName(registry, FName, 16-byte scratch, u8* flag): a row or null; the ObjectLayout registry.
inline constexpr std::uint8_t kFindRowByNameBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x4C, 0x89, 0x4C, 0x24, 0x20,
                                                       0x4C, 0x89, 0x44, 0x24, 0x18};
inline constexpr FunctionFact   kFindRowByName{L"DataObjRegistry_FindRowByName", 0x14076A330, kFindRowByNameBytes};
inline constexpr std::uintptr_t kObjectLayoutRegistry = 0x148712D00;
inline constexpr std::uint32_t  kLayoutRowActorGroup  = 0x40;  // ObjectLayout: OverlapActorGroup (FName)
/// DataAcc_ActorGroup_Ctor: the ActorGroup row (data_rows accessor); its ActorList (FNames) at +0, the first 32 read.
inline constexpr FunctionFact kActorGroupRow{L"DataAcc_ActorGroup_Ctor", 0x141217A24, kRowAccessorBytes};
inline constexpr std::int32_t kActorGroupActorsRead = 32;
inline constexpr std::int32_t kBoxSlotAttribute     = 1;  // the box object's state slot whose Status is the trigger sent
/// A character's own trigger tracker (both builds): the boxes it stood in at the last pass (FNames, inline 32, sorted),
/// and the box actors this pass overlaps (FWeakObjectPtrs, inline 32).
inline constexpr std::uint32_t kCharacterTracker   = 0x9F0;
inline constexpr std::uint32_t kTrackerKeys        = 0x10;
inline constexpr std::uint32_t kTrackerKeysHeap    = 0x110;
inline constexpr std::uint32_t kTrackerKeysNum     = 0x118;
inline constexpr std::uint32_t kTrackerOverlaps     = 0x230;
inline constexpr std::uint32_t kTrackerOverlapsHeap = 0x330;
inline constexpr std::uint32_t kTrackerOverlapsNum  = 0x338;
inline constexpr std::int32_t  kTrackerInline       = 32;
/// A trigger box actor's collision component (ATriggerBase::CollisionComponent, a UBoxComponent for an AEndTriggerBox)
/// and the box component's unscaled half-size; a scene component's world transform (rotation, location, scale).
inline constexpr std::uint32_t kTriggerCollisionComponent = 0x228;
inline constexpr std::uint32_t kBoxComponentExtent        = 0x4A8;
inline constexpr std::uint32_t kComponentWorldRotation    = 0x1C0;
inline constexpr std::uint32_t kComponentWorldLocation    = 0x1D0;
inline constexpr std::uint32_t kComponentWorldScale       = 0x1E0;

// ---- The AI system's setting groups (EndAISystem_GetFromGameModeWorld()+0x540, a TMap<FName, TArray<entry>>) ----
inline constexpr std::uint8_t kAiSystemBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00};
inline constexpr FunctionFact kAiSystem{L"EndAISystem_GetFromGameModeWorld", 0x140897BE4, kAiSystemBytes};
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
// The setting's level object, row and arrival trigger (FNames), filled at the state's creation, before its Enter, held
// or not. Its point (world cm) is the walk's end once the move has entered (the entered byte set; its only writer), or
// when the row names a destination; a relay row names none, only a route of points, whose last the walk ends at, and
// the point stays unwritten until the AI ticks (RE-lead-relay-point.md).
inline constexpr std::uint32_t kMoveToObject      = 0x20C;
inline constexpr std::uint32_t kMoveToRow         = 0x214;
inline constexpr std::uint32_t kMoveToTrigger     = 0x21C;  // 0 for a plain MoveToTarget row: its string argument 2
inline constexpr std::uint32_t kMoveToPoint       = 0x360;
inline constexpr std::uint32_t kMoveToDestination = 0x36C;  // FName: the row's destination point (0: none); read on
                                                             // 1.005, 1.004 by the same desc copy
inline constexpr std::uint32_t kMoveToRoute       = 0x390;  // TArray<FName>: the route's point names
inline constexpr std::uint32_t kMoveToMoveEntered = 0x3E1;  // u8
inline constexpr std::uint32_t kLeadObject        = 0x1BC;
inline constexpr std::uint32_t kLeadRow           = 0x1C4;
inline constexpr std::uint32_t kLeadTrigger       = 0x1CC;
inline constexpr std::uint32_t kLeadPoint         = 0x318;
inline constexpr std::uint32_t kLeadDestination   = 0x324;  // FName
inline constexpr std::uint32_t kLeadRoute         = 0x338;  // TArray<FName>
inline constexpr std::uint32_t kLeadMoveEntered   = 0x3C2;  // u8
inline constexpr std::int32_t  kRoutePointsMax    = 256;    // far above any route row's expansion

// ---- The field's named points (FieldPoint_FindByNameGlobal(const FName*): the registry in the manager at
// kRootTriggerManager, read inside without a null check; an FTransform* or null for a point not loaded) ----
inline constexpr std::uint8_t  kFieldPointFindBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x8B, 0x05};
inline constexpr FunctionFact  kFieldPointFind{L"FieldPoint_FindByNameGlobal", 0x140F50EBC, kFieldPointFindBytes};
inline constexpr std::uint32_t kFieldPointLocation = 0x10;  // the transform's translation

// ---- The story flags (StoryFlag_GetByName(const FName*): whether the named flag is set) ----
inline constexpr std::uint8_t kStoryFlagBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10};
inline constexpr FunctionFact kStoryFlag{L"StoryFlag_GetByName", 0x14092B5C8, kStoryFlagBytes};

// ---- The field icons (g_FieldIconSystem, a pointer; its +0x500 the icon manager) ----
inline constexpr std::uintptr_t kFieldIconSystem   = 0x1491D8488;
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
