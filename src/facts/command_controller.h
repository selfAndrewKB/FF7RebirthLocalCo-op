#pragma once

// The field command controller (FieldCommandCtrl, EndUISystem[23]): the game's one, and each seat's own, which the mod
// builds the same way (battle/command_menu). The fields two or more modules read; each was read in IDA at an
// instruction that uses it.
//
// Research: Rebirth/Documentation/research/RE-p2-command-menu.md, RE-p2-command-menu-tick.md,
// RE-p2-command-menu-input.md, RE-p2-command-menu-lifecycle.md, RE-p2-command-menu-levers.md, RE-p2-field-interact.md.

#include <array>
#include <cstdint>

namespace lss::facts {

inline constexpr std::uint32_t kCmdStoredLeaderType = 0x30;   // u8: its record of its leader's type, written at the end
                                                                  // of each tick (the game's equals the party manager's)
inline constexpr std::uint32_t kCmdMembers          = 0x34;   // i32 x3: leader ordinal, ally, shortcut target
inline constexpr std::uint32_t kCmdSwitchingLayer   = 0x258;  // u8: the member selecter layer
inline constexpr std::uint32_t kCmdBattleInput      = 0x25C;  // u8
inline constexpr std::uint32_t kCmdFieldMenu        = 0x4E8;  // FWeakObjectPtr: its field menu (holds the party list)
inline constexpr std::uint32_t kCmdMenuWidget       = 0x4F0;  // FWeakObjectPtr: the controller's menu
inline constexpr std::uint32_t kCmdHudMode          = 0x540;  // u8: the battle HUD mode (0 out of battle)
inline constexpr std::uint32_t kCmdBlocked          = 0x541;  // u8
inline constexpr std::uint32_t kCmdRequests         = 0x570;  // TArray<int32> {data, num}: the request array
inline constexpr std::uint32_t kCmdOpenPending      = 0x5D0;  // i32: a menu open requested (>= 0), pushed 0.125 s
                                                                  // later; the field poll needs it < 0
inline constexpr std::uint32_t kCmdMenuAllowed      = 0x5E1;  // u8: 0 while the game forbids its menu (minigames,
                                                                  // summons, events; research/RE-p2-field-command-menu.md)
inline constexpr std::uint32_t kCmdWaitFilter       = 0x644;  // i32: the wait mode (the tactical tint's owner)
inline constexpr std::uint32_t kCmdGuardLatch       = 0x760;  // i32: the guard mode, latched
inline constexpr std::uint32_t kCmdSetupPending     = 0x775;  // u8: the tick re-runs Setup
inline constexpr std::uint32_t kCmdSetupDone        = 0x777;  // u8: Setup sets it, only ResetAll clears it; while 0
                                                                  // no menu state can be pushed (research/RE-p2-command-menu-lifecycle.md)
inline constexpr std::uint32_t kCmdReadyB           = 0x782;  // u8
inline constexpr std::uint32_t kCmdMenuLayer        = 0x783;  // u8: the menu's bottom focus layer (priority -1000)

/// The focus bytes the log reads of a controller (RE-p2-command-menu-input.md): battle input, switching layer,
/// setup done, menu layer, menu allowed, HUD mode.
inline constexpr std::array<std::uint32_t, 6> kCmdLogBytes{kCmdBattleInput, kCmdSwitchingLayer,
                                                               kCmdSetupDone, kCmdMenuLayer, kCmdMenuAllowed,
                                                               kCmdHudMode};

}  // namespace lss::facts
