#pragma once

// The HUD's root widgets and whose they are (lever L1 of hud/passes/seat_hud_content.h).
//
// The HUD roots are the UUserWidgets that sit on the overlay itself (no slot) and belong to the
// game instance. Each carries a player context (UUserWidget+0x248: weak local player, weak world),
// which the marker list and the world-anchored widgets read to choose the controller they project
// with. All roots are player 1's (research/RE-p2-hud.md). UUserWidget::SetPlayerContext (stock UE4)
// sets it, and walks the widget's child user widgets; it sends no events. The command menu and the party status never
// read it while they tick or paint (RE-hud-context-readers.md), so they are left on player 1's (hud-context-skip).
//
// Roots come and go (the game adds markers as it needs them), so the list is rescanned from
// hud/user_widgets.h, which needs no walk of every UObject. Game thread only.

#include "views/columns.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace RC::Unreal {
class UObject;
}

namespace lss::platform {
class GameImage;
}

namespace lss::hud::hud_roots {

/// Checks SetPlayerContext's prologue. False (logged by the caller) if it differs.
bool Install(const platform::GameImage& image) noexcept;

/// Each column's local player ([0] player 1's; a seat's or a mirror's in the others), null for a column not listed.
using LocalPlayerSet = std::array<RC::Unreal::UObject*, views::columns::kMaxColumns>;

/// The game instance's local players by column (the list's order is the columns', views/columns.h), or false without
/// player 1's first. The game instance is found once and then only checked for liveness.
bool LocalPlayers(LocalPlayerSet& byColumn);

/// Refreshes the root list and every listed column's context. False if player 1's cannot be built yet.
bool Scan(const LocalPlayerSet& byColumn);

/// The last scan built the column's context.
bool HasContext(int column) noexcept;

/// Adds widget to the root list at once if it is a root not listed yet (a root the game made since
/// the last scan and that is known now, e.g. an enemy's ability-name label), so this frame's context
/// swap reaches it. Game thread, before a seat's SetContexts.
void Adopt(RC::Unreal::UObject* widget);

/// Adopt, and marks the root as self-placing for this frame: a widget its own run shows or collapses
/// from its context's projection (the field-target markers, EndUI__M98_ProjectEntry). A seat's
/// SetContexts gives it the seat's context even while collapsed, else a marker collapsed by player 1's
/// projection would be projected through player 1 again in the seat's run and never come back
/// (research/RE-p2-field-interact.md). True if it is a root. Game thread, before a seat's SetContexts.
bool AdoptSelfPlacing(RC::Unreal::UObject* widget);

/// Gives a further column's context to every live root shown in its pass and to the self-placing ones that do not
/// carry it already (hud/context_owners.h), or player 1's (column 0) back to every root a further column's reached
/// (and clears the self-placing marks). A root a pass does not need keeps an earlier pass's context until the way back;
/// a root of a context-free class keeps player 1's while the skip is on (SetSkipContextFree).
/// False if the engine raised an exception inside SetPlayerContext, or the column has no context.
bool SetContexts(int column) noexcept;

/// The census since the last call, for the perf log: the setter's calls per frame each way, its time, and the
/// costliest root classes. Counts start afresh. May throw (out of memory).
std::wstring TakeCensus(std::uint32_t frames);

/// The census's counts start afresh, unlogged (the perf log off).
void ResetCensus() noexcept;

/// The `hud-context-skip` switch (on from the start): the roots of a context-free class (context_owners.h; the command
/// menu and the party status) keep player 1's context through the seats' passes. Takes effect from the next
/// switch toward a seat; a root a seat's reached still goes back to player 1 with the others. Game thread.
void SetSkipContextFree(bool on) noexcept;
bool SkipContextFree() noexcept;

/// How many roots the last scan found.
std::size_t Count() noexcept;

/// How many self-placing roots the last SetContexts toward a seat gave the seat's context while collapsed.
std::size_t HiddenSwapped() noexcept;

}  // namespace lss::hud::hud_roots
