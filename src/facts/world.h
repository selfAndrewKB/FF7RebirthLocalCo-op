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
inline constexpr std::uint8_t  kEncounterInBattle      = 3;
inline constexpr std::uint8_t  kEncounterEscaping      = 4;
inline constexpr std::uint32_t kGameStateEventCounterA = 0x1A123B8;  // [[g_EndGameRoot]+0x170]+..., i32
inline constexpr std::uint32_t kGameStateEventCounterB = 0x1A12668;
inline constexpr std::uint8_t  kInsideAnyBattleAreaBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xF2, 0x94, 0xE8,
                                                              0x00, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xD1, 0x48, 0x8B};
inline constexpr FunctionFact  kInsideAnyBattleArea{L"EndFieldAPI_IsCharacterInsideAnyBattleArea_Body", 0x1412E2684,
                                                   kInsideAnyBattleAreaBytes};

}  // namespace lss::facts
