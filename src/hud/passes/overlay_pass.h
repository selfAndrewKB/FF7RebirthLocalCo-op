#pragma once

// Which pass of the overlay render is running, by the column whose view it draws (as Remake's hud/overlay_pass): first
// one extra render per further column with an image this frame, highest column first (the mod's,
// hud/passes/column_hud_images.h), then the game's own, column 0 (player 1's), last; kNone outside the render. A column
// is a view's place in the engine's list (views/columns.h): a seat's view, or a mirror's (views/mirror_player.h).
// Set by root_overlay_render, the one driver of the passes; read by everything that paints into a menu-renderer slot
// during them (hud/passes/shared_slot.h, hud/layout/overlay_layout.cpp). The rules the slot follows across the passes
// are pure functions here (tests/hud_rules_test.cpp). Game thread only.
//
// Each further column's pass draws into an entry named for that column, and the render thread finds each view's column
// from the view's player (hud/passes/column_hud_images.cpp). What a column's pass shows is its player's HUD, or player
// 1's laid out for the column's view (a copy of player 1's picture on a monitor of another shape).

#include "views/columns.h"

namespace lss::hud::overlay_pass {

/// Player 1's column and the further ones.
inline constexpr int kMaxColumns = views::columns::kMaxColumns;

/// Outside the overlay render.
inline constexpr int kNone = -1;

namespace detail {
inline int                 g_column = kNone;
inline views::columns::Map g_map;  // the engine's list as columns this tick (SetMap)
}  // namespace detail

/// The columns map this tick, set once a tick before the HUD's tick and the frame's passes (modules.cpp TickColumns).
inline void SetMap(const views::columns::Map& map) noexcept
{
    detail::g_map = map;
}

inline const views::columns::Map& Map() noexcept
{
    return detail::g_map;
}

/// The seat in `column` (0 player 1's), or views::columns::kMirror / kUnknown.
inline int SeatAt(int column) noexcept
{
    return column >= 0 && column < kMaxColumns ? detail::g_map.seatAt[static_cast<std::size_t>(column)]
                                               : views::columns::kUnknown;
}

/// The column whose pass runs: 0 the game's own render (player 1's), 1.. a further column's, kNone outside the render.
inline void Set(int column) noexcept
{
    detail::g_column = column >= 0 && column < kMaxColumns ? column : kNone;
}

inline int Column() noexcept
{
    return detail::g_column;
}

/// A further column's pass runs: the mod's extra render.
inline bool IsFurtherPass() noexcept
{
    return detail::g_column >= 1;
}

/// What a widget's acquire of a ring slot does in a pass, for a slot that carries every pass's entry and is submitted
/// once a frame (pool 2's overlay slot, pool 0's navimap circle): the first further pass acquires the slot and
/// holds it, each later further pass takes the held slot, and column 0's pass, the last, takes it back and submits it
/// with every entry. A slot left acquired and unsubmitted would hang the ring when it comes round again.
enum class SlotAction
{
    Acquire,         // the game's own acquire
    AcquireAndHold,  // the first further pass the slot takes part in
    TakeHeld,        // a later further pass: the same slot, still held
    TakeBack,        // column 0's pass: the held slot, no longer held once its submit sends it
};

/// `active`: the slot takes part in this column's pass (its widget paints for the pass). `held`: a further pass holds
/// the slot. `takenThisPass`: this pass already has it, so a second acquire in one pass is the game's own.
constexpr SlotAction AcquireAction(int column, bool active, bool held, bool takenThisPass) noexcept
{
    if (column == 0)
    {
        return held ? SlotAction::TakeBack : SlotAction::Acquire;
    }
    if (column < 0 || !active || takenThisPass)
    {
        return SlotAction::Acquire;
    }
    return held ? SlotAction::TakeHeld : SlotAction::AcquireAndHold;
}

/// The entry a further column's pass adds to the held slot is its column's: it gets the column's name, and its submit is
/// left to column 0's pass (which submits the slot with every entry).
constexpr bool IsColumnEntry(int column, bool heldSlot) noexcept
{
    return column >= 1 && column < kMaxColumns && heldSlot;
}

/// Whether a view of width over height `aspect` has another shape than player 1's `aspectOne`: more than 1 % apart. The
/// game's composite cover-fits a HUD image to the view it is drawn in, so player 1's image in a wider view shows
/// enlarged with its top and bottom cut, in a narrower one inset. False while either is unknown (0).
constexpr bool OtherShape(float aspect, float aspectOne) noexcept
{
    if (!(aspect > 0.0f) || !(aspectOne > 0.0f))
    {
        return false;
    }
    const float ratio = aspect / aspectOne;
    return ratio > 1.01f || ratio < 1.0f / 1.01f;
}

/// What a further column's pass of the overlay render draws this frame (a seat's column or a mirror's).
enum class SeatPassKind
{
    None,       // no pass: the column's view draws player 1's image (or its seat is out, or one view shows)
    Own,        // the seat's own HUD in its own view
    PlayerOnes, // a copy: player 1's HUD laid out for the column's view, which shows player 1's picture on a monitor
                // of another shape than player 1's, where player 1's image would be cut or inset
};

/// The kind for a seat: `active` the seat in the game; `ownShown` its own view on screen; `onPlayerOne` its column on
/// player 1's picture; `viewsSplit` several views on screen; `aspect` and `aspectOne` its view's and player 1's shapes.
constexpr SeatPassKind PassKindOf(bool active, bool ownShown, bool onPlayerOne, bool viewsSplit, float aspect,
                                  float aspectOne) noexcept
{
    if (!active)
    {
        return SeatPassKind::None;
    }
    if (ownShown && !onPlayerOne)
    {
        return SeatPassKind::Own;
    }
    return viewsSplit && onPlayerOne && OtherShape(aspect, aspectOne) ? SeatPassKind::PlayerOnes : SeatPassKind::None;
}

/// The kind for a mirror's column (a copy of player 1's picture, views/mirror_player.h): player 1's HUD laid out for
/// its view when several views show and its shape differs from player 1's; none otherwise (player 1's image fits).
constexpr SeatPassKind MirrorPassKind(bool viewsSplit, float aspect, float aspectOne) noexcept
{
    return viewsSplit && OtherShape(aspect, aspectOne) ? SeatPassKind::PlayerOnes : SeatPassKind::None;
}

}  // namespace lss::hud::overlay_pass
