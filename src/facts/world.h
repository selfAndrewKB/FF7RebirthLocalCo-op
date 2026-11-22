#pragma once

// The world-level managers: the game root (g_EndGameRoot) and the managers hanging off it, the location volume manager,
// the animation world manager. Each pointer global is read live (a null means the world is not up); each offset was
// read in IDA at an instruction that uses it.
//
// Research: Rebirth/Documentation/research/RE-p2-field-interact.md, RE-p2-battle-start-triggers.md,
// RE-p2-charapop-per-view.md, RE-p2-framing.md, RE-p2-anim-optimisation-two-views.md, RE-p2-lod-draw-distance.md.

#include "platform/verified_code.h"

#include <cstdint>

namespace lss::facts {

using platform::FunctionFact;

/// g_EndGameRoot: a pointer global the game mode fills, the root of the field's managers. Its slots (each a pointer):
inline constexpr std::uintptr_t kEndGameRoot    = 0x148F325B8;
inline constexpr std::uint32_t  kRootGameState  = 0x170;  // the game state: the on/off table's owner and its counters
inline constexpr std::uint32_t  kRootCharaPop   = 0x180;  // the CharaPop manager (world/charapop_facts.h)
inline constexpr std::uint32_t  kRootModeHolder = 0x1B8;  // the mode holder (the encounter manager; +0x3D558 its state)
inline constexpr std::uint32_t  kRootWorldItems = 0x288;  // the WorldItemMgr
/// WorldItemMgr's map of items (RE-p2-field-interact.md): a stock TMap keyed by the item's FName at element +0
/// (platform/tset_find.h): where the map sits in the manager, its element's size and the chain index in it.
inline constexpr std::uint32_t  kWorldItemMapElements = 0x90;
inline constexpr std::uint32_t  kWorldItemMapStride   = 0xD0;
inline constexpr std::uint32_t  kWorldItemMapHashNext = 0xC0;  // i32
// +0x1A0 is the FieldActionMgr (world/field_pickup), +0x270 the chocobo ride manager (facts/vehicles.h).

/// g_LocationVolumeManager (a pointer): the AEndLocationVolumes and the game's own containment test,
/// LocationVolume_ContainsPoint(volume, &position, margin), a pure read (RE-p2-framing.md).
inline constexpr std::uintptr_t kLocationVolumeManager = 0x1491D84C0;
inline constexpr std::uint8_t   kLocationVolumeContainsPointBytes[] = {0x40, 0x53, 0xB8, 0x50, 0x00, 0x00, 0x00, 0xE8, 0xD4,
                                                                       0xA9, 0x47, 0x01, 0x48, 0x2B, 0xE0, 0x33, 0xDB};
inline constexpr FunctionFact   kLocationVolumeContainsPoint{L"LocationVolume_ContainsPoint", 0x140CF11A0,
                                                           kLocationVolumeContainsPointBytes};

/// g_AnimWorldManager (a pointer): the SE world manager the animation budget and CharaPop read (0xE00 bytes).
inline constexpr std::uintptr_t kAnimWorldManager = 0x1491D8458;

/// The game's catch-up move, as its battle gather calls it (RE-member-story-boxes.md):
/// Nav_FindPointAround_AnglesDistances(unused, querier, const FTransform* origin, const FNavAgentProperties* agent,
/// const TArray<float, TInlineAllocator<4>>* distances, const TArray<float, TInlineAllocator<4>>* anglesRad, int unused,
/// FVector* out) -> bool: for each angle, each distance, a point from the origin projected to the navmesh and reached by
/// a navmesh raycast from it; the first accepted wins.
inline constexpr std::uint8_t kNavPointAroundBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41,
                                                        0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x40, 0xF8};
inline constexpr FunctionFact kNavPointAround{L"Nav_FindPointAround_AnglesDistances", 0x141284B14, kNavPointAroundBytes};
/// The querier's nav agent: its interface sub-object at +0x228, the agent getter at that vtable's +0x10 (argument 0).
inline constexpr std::uint32_t kCharacterNavAgentInterface = 0x228;
inline constexpr std::uint32_t kNavAgentGetterSlot         = 0x10;
/// EndCharacter_BattleTeleport_FadeIn(character, const FVector* look, const FRotator* rotation, const FVector* dest,
/// bool snapToGround, bool resetControlHolder): a queued warp to `dest` and a short fade-in; nothing battle-only.
inline constexpr std::uint8_t kBattleTeleportFadeInBytes[] = {0x48, 0x85, 0xC9, 0x0F, 0x84, 0xF7, 0x01, 0x00, 0x00};
inline constexpr FunctionFact kBattleTeleportFadeIn{L"EndCharacter_BattleTeleport_FadeIn", 0x141328EB4,
                                                    kBattleTeleportFadeInBytes};

/// The fight's own state and area (RE-arena-warp.md): the encounter's state in the mode holder (3 in battle, 4
/// while player 1 escapes); the game's "inside any battle area" (spheres around the engaged enemies) for a character;
/// and the game state's two event counters the leader switch refuses on (an event holding the party).
inline constexpr std::uint32_t kEncounterState         = 0x3D558;  // [[g_EndGameRoot]+0x1B8]+0x3D558, u8
inline constexpr std::uint8_t  kEncounterField         = 1;
inline constexpr std::uint8_t  kEncounterInBattle      = 3;
inline constexpr std::uint8_t  kEncounterEscaping      = 4;
inline constexpr std::uint32_t kGameStateEventCounterA = 0x1A123B8;  // [[g_EndGameRoot]+0x170]+..., i32
inline constexpr std::uint32_t kGameStateEventCounterB = 0x1A12668;
inline constexpr std::uint8_t  kInsideAnyBattleAreaBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xF2, 0x94, 0xE8,
                                                              0x00, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xD1, 0x48, 0x8B};
inline constexpr FunctionFact  kInsideAnyBattleArea{L"EndFieldAPI_IsCharacterInsideAnyBattleArea_Body", 0x1412E2684,
                                                   kInsideAnyBattleAreaBytes};

/// The field encounter (RE-field-battle-start-seats.md): its tick reads one watched character (the party leader) for
/// every field enemy, through one call; the fight starts in one call. The manager's fields beside its state:
inline constexpr std::uint32_t kEncounterEntriesInline    = 0xE0;   // the field enemies' records, inline (64 pointers)
inline constexpr std::uint32_t kEncounterEntriesHeap      = 0x2E0;  // or on the heap once allocated (a pointer)
inline constexpr std::uint32_t kEncounterEntryCount       = 0x2E8;  // i32
inline constexpr std::int32_t  kEncounterEntriesInlineMax = 64;
inline constexpr std::uint32_t kEncounterVigilanceGauge   = 0x3D55C;  // f32: the highest gauge ratio, the HUD's danger
inline constexpr std::uint32_t kEncounterVigilanceReach   = 0x3D560;  // f32: the nearest distance ratio
inline constexpr std::uint32_t kEncounterEscapeTimer      = 0x3D564;  // f32: time outside every fight area while escaping
inline constexpr std::uint32_t kEncounterPromptShown      = 0x3D5D0;  // u8: the preemptive-attack prompt is shown
inline constexpr std::uint32_t kEncounterPreemptiveNotice = 0x3D5E2;  // u8: the battle-in notice will say "preemptive"
/// A field enemy's record: its key (an FName), its territory and enemy (weaks), its two gauges, three force bytes (the
/// vehicle bump and the scripted starts set them), two perception states (3 once in a fight), its engage reach and
/// whether it fights in a pre-placed named area.
inline constexpr std::uint32_t kEncounterEntryName      = 0x28;
inline constexpr std::uint32_t kEncounterEntryTerritory = 0x48;
inline constexpr std::uint32_t kEncounterEntryEnemy     = 0x90;
inline constexpr std::uint32_t kEncounterEntryGaugeA    = 0xE8;  // f32
inline constexpr std::uint32_t kEncounterEntryGaugeB    = 0xEC;  // f32
inline constexpr std::uint32_t kEncounterEntryForce     = 0xF0;  // u8[3]
inline constexpr std::uint32_t kEncounterEntryState     = 0xF9;  // u8[2]
inline constexpr std::uint32_t kEncounterEntryReach     = 0xFC;  // f32, squared centimetres
inline constexpr std::uint32_t kEncounterEntryNamedArea = 0x11D;  // u8
/// The tick's one read of the watched character (`call` the party leader resolver), the fight's start
/// EncountMgr_EnterBattle(manager, const TArray<entry*>* engaged, character) at its one call, and the battle-in gather's
/// anchor (`mov cl, 1; call` the field leader resolver) in PartyBattleAIMgr_QueueBattleInTeleports.
inline constexpr std::uintptr_t kEncounterSubjectCall = 0x1410B30E2;
inline constexpr std::uint8_t   kEncounterEnterBattleBytes[] = {0x48, 0x8B, 0xC4, 0x4C, 0x89, 0x40, 0x18, 0x48,
                                                                0x89, 0x50, 0x10, 0x48, 0x89, 0x48, 0x08, 0x55,
                                                                0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41};
inline constexpr FunctionFact   kEncounterEnterBattle{L"EncountMgr_EnterBattle", 0x141155228, kEncounterEnterBattleBytes};
inline constexpr std::uintptr_t kEncounterEnterBattleCall = 0x1410B52F3;
inline constexpr std::uintptr_t kBattleInAnchorCall       = 0x141157EE3;
/// EnemyTerritory_ContainsPoint(territory, const FVector* point, float flatDistanceSquared) -> bool: the tick's test of
/// the watched character against a field enemy's territory (the distance from the territory's root); a pure read.
inline constexpr std::uint8_t kTerritoryContainsPointBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x7C,
                                                                0x24, 0x10, 0x55, 0x48, 0x8B, 0xEC, 0xB8, 0x60,
                                                                0x00, 0x00, 0x00, 0xE8, 0x50, 0x59, 0x57, 0x01};
inline constexpr FunctionFact kTerritoryContainsPoint{L"EnemyTerritory_ContainsPoint", 0x140BF6218,
                                                      kTerritoryContainsPointBytes};

}  // namespace lss::facts
