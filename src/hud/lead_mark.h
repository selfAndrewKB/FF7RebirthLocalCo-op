#pragma once

// Player 2's or 3's own marker for the walk they keep (`lead-holder keeps`, session/lead_keep_rules.h;
// research/RE-lead-mark.md): the game's own main-objective diamond and minimap pin at the walk's stop, in that player's
// view only.
//
// While a seat's member runs a walk the lead gate judges KEEP (lead_gate::KeptMark), the stop carries the game's own
// map icon, kind 2 (the main objective: the diamond with its line, the minimap pin), under the mod's key
// LSS_LeadMark_<seat>, one the game's keys (story rows, map icon rows) cannot meet. Rebirth has one store of map icons
// that every view's icon manager and the one minimap holder read, so the icon is added once (the game's AddIcon, the
// compass byte off) and withheld from the other views' paints: after each other view's icon manager ticks and after
// each minimap run not centred on the owner, the key's widget is set Hidden as the game sets it
// (hud/minimap/column_area_icons HideIcon, hud/minimap/minimap HidePin); the game's own next tick shows it again where
// it belongs. Re-added at every check (the game updates a key it holds in place, and a field end empties the store);
// moved with the stop; removed when the verdict ends (the character brought to the stop, the setting's end, the player
// letting go). Checked every 10 ticks per seat. In the owner's own view the marker is drawn larger: the diamond three
// times the game's size (a render transform on its image, the line unchanged), the pin twice the scale the minimap run
// gives it (the game's own pin setter, after that run; research/RE-lead-marker-size.md). Player 1's objective markers
// keep the game's size. Without this module the lead gate hands every needed walk to its AI. Game thread.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::hud::lead_mark {

/// Finds AddIcon and RemoveIcon and the visibility setter, and tells the lead gate a kept walk can be marked.
bool Install(const platform::GameImage& image) noexcept;
/// Removes every seat's marker; the lead gate hands needed walks to the AI again.
void Remove() noexcept;

/// Each engine tick, game thread, outside the HUD's ticks: each seat's marker added, moved or removed.
void Tick() noexcept;

/// In a column's overlay pass, after its icon manager ticked and its minimap ran, before its paint: every marker whose
/// owner is not `shown` (the player the column shows; 0 player 1) withheld from it.
void BeforeColumnPaint(int column, int shown) noexcept;

/// Before player 1's paint (the game's own overlay render): every marker withheld from player 1's view.
void BeforePlayerOnePaint() noexcept;

}  // namespace lss::hud::lead_mark
