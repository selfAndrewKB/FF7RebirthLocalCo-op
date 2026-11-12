#pragma once

// The field-action actors (End2FieldActionActorBase and its classes, the traversal: ladders, squeezes, crawls, pole
// slides, lifting ropes, one-shot moves, climbing walls, monkey bars, spider walls, zip lines, beams, wall runs, doors,
// the chocobo climb, launch pad, jump box and slider): their native vtables (data; Blueprint subclasses keep them), the
// trigger enter and leave every one of them shares, and the chocobo spots' unregisters.
//
// Research: Rebirth/Documentation/research/RE-p2-traversal.md, RE-p2-traversal-cameras.md, RE-p2-chocobo-climb.md,
// RE-chocobo-exits-pads.md; 1.005 through replaced-traversal.json, replaced-traversal-classes.json,
// replaced-chocobo-traversal.json, replaced-chocobo-spots.json (every slot and entry read from both exes,
// tools/version_match/_traversal_check.py).

#include "platform/verified_code.h"

#include <cstdint>

namespace lss::facts {

using platform::FunctionFact;

inline constexpr std::uintptr_t kLadderVTable         = 0x145940748;
inline constexpr std::uintptr_t kClimbingWallVTable   = 0x145832BE8;
inline constexpr std::uintptr_t kCrawlVTable          = 0x1457D8960;
inline constexpr std::uintptr_t kSqueezeVTable        = 0x1466E8B68;
inline constexpr std::uintptr_t kBeamVTable           = 0x145872C88;
inline constexpr std::uintptr_t kMonkeyBarsVTable     = 0x1466EF050;
inline constexpr std::uintptr_t kLiftingRopeVTable    = 0x1466FE5D0;
inline constexpr std::uintptr_t kSpiderMoveVTable     = 0x1467095F8;
inline constexpr std::uintptr_t kZipLineVTable        = 0x1458BA188;
inline constexpr std::uintptr_t kPoleSlideVTable      = 0x1458B41F8;
inline constexpr std::uintptr_t kOneShotMoveVTable    = 0x14580EF00;
inline constexpr std::uintptr_t kWallRunVTable        = 0x146707D88;
inline constexpr std::uintptr_t kDoorVTable           = 0x1459434F0;
inline constexpr std::uintptr_t kChocoboClimbVTable   = 0x1466F6F58;
inline constexpr std::uintptr_t kChocoboJumpPadVTable = 0x1466F4DC8;
inline constexpr std::uintptr_t kChocoboJumpBoxVTable = 0x1466F6400;
inline constexpr std::uintptr_t kChocoboSliderVTable  = 0x1466F5950;

/// The chocobo spots' unregister overrides (the slot after the register's; RE-chocobo-exits-pads.md), each clearing
/// the character the register stored: the launch pad's at +0x734, the jump box's at +0x7A0, the slider's at +0x760.
inline constexpr std::uintptr_t kChocoboJumpPadUnregister = 0x143661B94;
inline constexpr std::uintptr_t kChocoboJumpBoxUnregister = 0x143661B18;
inline constexpr std::uintptr_t kChocoboSliderUnregister  = 0x143661C04;

/// The grappling gun's points (AEndFieldActionActorHookShotChild) and the hook ropes (AEndFieldActionHookRopeActor;
/// RE-hooks-for-seats.md, RE-p2-zipline-rumble-hook-who.md): their vtables, the two FName globals of the hookshot
/// point's presets (rpaHookShot, rpaHookShotPlayer: the field-action manager's point for the gun), and the one global
/// camera-module handle every rope's swing installs through (the module, then its weak reference controller).
inline constexpr std::uintptr_t kHookShotVTable             = 0x1466F0F98;
inline constexpr std::uintptr_t kHookRopeVTable             = 0x145884360;
inline constexpr std::uintptr_t kHookShotPresetName         = 0x149452BE8;
inline constexpr std::uintptr_t kHookShotPlayerPresetName   = 0x149452BF8;
inline constexpr std::uintptr_t kHookRopeCameraModuleGlobal = 0x1491B0E00;
/// A gun point actor's own camera-module handle (16 bytes: the module, then its weak reference controller), the
/// per-actor twin of the rope's global.
inline constexpr std::uint32_t kHookShotCameraHandle = 0x9E0;
/// A gun point actor's press flag (u8): set by the point's press listener, read by every holder's notify (each starts
/// the hookshot on its own character while it is set), cleared only when the point goes off (RE-hook-follow.md).
inline constexpr std::uint32_t kHookShotPressFlag = 0x9D8;
/// A gun point actor's key-listener handle (u64): made as the point first goes on, cleared with the press flag by the
/// point's off path; non-zero while the point is on.
inline constexpr std::uint32_t kHookShotListener = 0x9D0;
/// A character's field-action notify pass hands each actor's notify slot a context whose second pointer is the
/// character the pass runs for.
inline constexpr std::uint32_t kNotifyContextCharacter = 0x8;

/// End2FieldActionActor OnTriggerEnter / OnTriggerLeave (actor vt+0x710 / +0x718; 1.005 +0x718 / +0x720), shared by every
/// field-action actor: (actor, const FName* key, AActor* other), `other` being the overlapping actor whose first tag is
/// the key (a controller's trigger actor). Both start `mov [rsp+10h], rbx` (5 bytes, inside one 16-byte block in both
/// builds: 1.004 +0x4 / +0x4, 1.005 +0x0 / +0x8). A fourth register is passed on too.
inline constexpr std::uint8_t kFieldActionTriggerBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C,
                                                            0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57};
inline constexpr FunctionFact kFieldActionTriggerEnter{L"End2FieldActionActor::OnTriggerEnter", 0x1409DC534,
                                                       kFieldActionTriggerBytes};
inline constexpr FunctionFact kFieldActionTriggerLeave{L"End2FieldActionActor::OnTriggerLeave", 0x1409DB844,
                                                       kFieldActionTriggerBytes};

}  // namespace lss::facts
