#pragma once

// The world-level managers: the game root (g_EndGameRoot) and the managers hanging off it, the location volume manager,
// the animation world manager. Each pointer global is read live (a null means the world is not up); each offset was
// read in IDA at an instruction that uses it.
//
// Research: Rebirth/Documentation/research/RE-p2-field-interact.md §1.1, §12, RE-p2-battle-start-triggers.md,
// RE-p2-charapop-per-view.md §1.1, RE-p2-framing.md, RE-p2-anim-optimisation-two-views.md, RE-p2-lod-draw-distance.md §1.

#include "platform/verified_code.h"

#include <cstdint>

namespace lss::facts {

using platform::FunctionFact;

/// g_EndGameRoot: a pointer global the game mode fills, the root of the field's managers. Its slots (each a pointer):
inline constexpr std::uintptr_t kEndGameRoot    = 0x1490FEDE8;
inline constexpr std::uint32_t  kRootGameState  = 0x170;  // the game state: the on/off table's owner and its counters
inline constexpr std::uint32_t  kRootCharaPop   = 0x180;  // the CharaPop manager (player_two/world/charapop_facts.h)
inline constexpr std::uint32_t  kRootModeHolder = 0x1B8;  // the mode holder (the encounter manager; +0x3D558 its state)
inline constexpr std::uint32_t  kRootWorldItems = 0x288;  // the WorldItemMgr
/// WorldItemMgr's map of items (RE-p2-field-interact.md §10.5): a stock TMap keyed by the item's FName at element +0
/// (platform/tset_find.h): where the map sits in the manager, its element's size and the chain index in it.
inline constexpr std::uint32_t  kWorldItemMapElements = 0x90;
inline constexpr std::uint32_t  kWorldItemMapStride   = 0xD0;
inline constexpr std::uint32_t  kWorldItemMapHashNext = 0xC0;  // i32
// +0x1A0 is the FieldActionMgr (player_two/field_pickup), +0x270 the chocobo ride manager (facts/vehicles.h).

/// g_LocationVolumeManager (a pointer): the AEndLocationVolumes and the game's own containment test,
/// LocationVolume_ContainsPoint(volume, &position, margin), a pure read (RE-p2-framing.md; walk_limits §4.3.1).
inline constexpr std::uintptr_t kLocationVolumeManager = 0x14939BCA0;
inline constexpr std::uint8_t   kLocationVolumeContainsPointBytes[] = {0x40, 0x53, 0xB8, 0x50, 0x00, 0x00, 0x00, 0xE8, 0xE4,
                                                                       0xF6, 0x60, 0x01, 0x48, 0x2B, 0xE0, 0x33, 0xDB};
inline constexpr FunctionFact   kLocationVolumeContainsPoint{L"LocationVolume_ContainsPoint", 0x140A9F840,
                                                           kLocationVolumeContainsPointBytes};

/// g_AnimWorldManager (a pointer): the SE world manager the animation budget and CharaPop read (0xE00 bytes).
inline constexpr std::uintptr_t kAnimWorldManager = 0x14939BC48;

}  // namespace lss::facts
