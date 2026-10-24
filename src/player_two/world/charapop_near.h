#pragma once

// The characters player 2 is near keep the detail the game would give them from player 2's own camera
// (plan §3 item 7, the user's choice of 2026-10-02; RE-p2-charapop-per-view.md §1.5, §1.8 lever 2).
//
// The game's character LOD (CharaPop) judges every entry from player 1's camera only and has no path for a
// second view; running its evaluation again for player 2 would flip each entry between the two results.
// Its own per-entry lever is the minimum level: CharaPop_SetEntryMinLODLevel_ByKey, which event and
// cutscene scripts call with 3 to keep a character awake and shown and 0 to clear it. While the split is on
// screen the mod calls it the same way for the entries player 2's view needs.
//
// The one recreated piece, recorded in the plan as the rule requires: which entries player 2's view
// needs. It is the evaluator's own rule (Light or Middle by the entry's level: awake within the
// should-tick distance, or within the in-camera distance inside the FOV cone and the out-of-camera
// distance outside it; shown within the draw distance for the entries that hide), with the game's own
// parameters and overrides read from the manager, applied to player 2's camera pose instead of player
// 1's. Only entries player 1's camera does not already keep are raised (the same rule applied to player
// 1's camera, which is the one the game itself uses), so with the players together almost nothing is: the
// game alone already gives those characters what player 2's view needs. A raised entry is released once
// it has been outside player 2's distances, or well inside player 1's, by a margin for a while, so an entry
// on the edge does not toggle.
//
// Never touches an entry whose minimum the game set (only entries reading 0 are raised), and clears only
// entries the mod raised that still read 3. Everything raised is cleared when the split closes; the
// record is dropped without calls when the map's CharaPop changes. Game thread only.
//
// Per seat (players/seats.h): each playing seat's camera is judged on its own ("player 2" above is any seat's);
// an entry is raised when any seat's view needs it and kept while any does (the union). One record list and one
// minimum level per entry, shared: the game has one.

#include "platform/dev_switch.h"
#include "players/seats.h"

#include <array>
#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::charapop_near {

/// How far player 2's view reaches: the game's distances (full, at start), half of them (reduced, for
/// performance), or not at all (off: characters near player 2 get only what player 1's camera gives them).
enum class Detail : std::uint8_t
{
    Off,
    Reduced,
    Full,
};

/// Finds the game's setter; logged either way. False if a check failed (nothing is raised then).
bool Install(const platform::GameImage& image) noexcept;

/// Each engine tick, with whether the split is on screen and, by seat, whether the seat's player plays a member
/// (index 0 unused).
void Tick(bool splitShown, const std::array<bool, players::kMaxPlayers>& plays) noexcept;

/// The comparison switch (Ctrl+F11 cycles full -> reduced -> off; `npc-detail full|reduced|off`). Off clears
/// what was raised; reduced releases what is past its reach over the next second. Logged.
void SetDetail(Detail detail) noexcept;
Detail CurrentDetail() noexcept;
Detail NextDetail(Detail detail) noexcept;
const wchar_t* DetailName(Detail detail) noexcept;
bool Installed() noexcept;

/// Clears every entry the mod raised (at mod uninstall, after the tick is unregistered; any thread).
void Release() noexcept;

#if LSS_DEV
/// The `npc-detail` command and reading. May throw (out of memory).
void RegisterCommands();
#endif

}  // namespace lss::player_two::charapop_near
