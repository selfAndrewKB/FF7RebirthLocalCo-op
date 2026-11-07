#pragma once

// Which pass of the overlay render is running, by the seat whose view it draws (plan/seats.md step 4): first one extra
// render per seat with an image this frame, highest seat first (the mod's, hud/passes/column_hud_images.h), then
// the game's own, player 1's (seat 0), last; kNone outside the render. Set by root_overlay_render, the one driver of
// the passes; read by everything that paints into a menu-renderer slot during them (hud/passes/shared_slot.h,
// hud/layout/overlay_layout.cpp). The rules the slot follows across the passes are pure functions here
// (tests/hud_rules_test.cpp). Game thread only.
//
// Keyed by seat, not by column: each seat's pass draws its seat's content into an entry named for that seat, and the
// render thread finds each view's seat from the view itself (hud/passes/column_hud_images.cpp), so the HUD needs no map
// of columns. A column with no seat (a mirror view, views/mirror_player.h) draws player 1's entry.

namespace lss::hud::overlay_pass {

/// Outside the overlay render.
inline constexpr int kNone = -1;

namespace detail {
inline int g_seat = kNone;
}

/// The seat whose pass runs: 0 the game's own render (player 1's), 1.. a seat's, kNone outside the render.
inline void Set(int seat) noexcept
{
    detail::g_seat = seat >= 0 ? seat : kNone;
}

inline int Seat() noexcept
{
    return detail::g_seat;
}

/// A seat's pass runs: the mod's extra render.
inline bool IsSeatPass() noexcept
{
    return detail::g_seat >= 1;
}

/// What a widget's acquire of a ring slot does in a pass, for a slot that carries every pass's entry and is submitted
/// once a frame (pool 2's overlay slot, pool 0's navimap circle): the first seat pass acquires the slot and
/// holds it, each later seat's pass takes the held slot, and player 1's pass, the last, takes it back and submits it
/// with every entry. A slot left acquired and unsubmitted would hang the ring when it comes round again.
enum class SlotAction
{
    Acquire,         // the game's own acquire
    AcquireAndHold,  // the first seat pass the slot takes part in
    TakeHeld,        // a later seat's pass: the same slot, still held
    TakeBack,        // player 1's pass: the held slot, no longer held once its submit sends it
};

/// `active`: the slot takes part in this seat's pass (its widget paints for the pass's seat). `held`: a seat's pass
/// holds the slot. `takenThisPass`: this pass already has it, so a second acquire in one pass is the game's own.
constexpr SlotAction AcquireAction(int seat, bool active, bool held, bool takenThisPass) noexcept
{
    if (seat == 0)
    {
        return held ? SlotAction::TakeBack : SlotAction::Acquire;
    }
    if (seat < 0 || !active || takenThisPass)
    {
        return SlotAction::Acquire;
    }
    return held ? SlotAction::TakeHeld : SlotAction::AcquireAndHold;
}

/// The entry a seat's pass adds to the held slot is its seat's: it gets the seat's name, and its submit is left to
/// player 1's pass (which submits the slot with every entry).
constexpr bool IsSeatEntry(int seat, bool heldSlot) noexcept
{
    return seat >= 1 && heldSlot;
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

/// What a seat's pass of the overlay render draws this frame.
enum class SeatPassKind
{
    None,       // no pass: the seat's view draws player 1's image (or the seat is out, or one view shows)
    Own,        // the seat's own HUD in its own view
    PlayerOnes, // player 1's HUD laid out for the seat's view: its column shows player 1's picture on a monitor of
                // another shape than player 1's, where player 1's image would be cut or inset
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

}  // namespace lss::hud::overlay_pass
