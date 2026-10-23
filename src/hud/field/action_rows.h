#pragma once

// The field-action row list, read and laid out once for its two users (hud/field/prompt_rows.h: each player's own rows;
// hud/field/action_icon_base.h: the rows' base in a split half). Every prompt row the game shows is a widget in one
// global list (EndUISystem+0x208: a pointer to {FWeakObjectPtr* data, int32 num}), laid out by the game's own
// FieldActionIconList__SortAndLayout(list, placeNow): slot i gets the point `_BasePosY + i * _PaddingY`, the row named
// by `placeNow` goes to its slot at once, the others slide there (RE-p2-field-prompts.md §1.2, §4;
// RE-p2-hud-view-levers.md §1). A row whose current place is outside the layout it is laid out for is not painted
// there, so it is not ticked and would never slide: it is named in a call of its own. Game thread only.

#include "hud/layout/viewport_placement.h"

#include <cstdint>
#include <span>
#include <vector>

namespace lss::platform {
class GameImage;
}

namespace lss::hud::action_rows {

/// Finds the list pointer and the game's layout function; logged by the callers. Idempotent.
bool Install(const platform::GameImage& image) noexcept;

/// The list pointer was found.
bool ListKnown() noexcept;

/// The game's layout function was found and has not raised an exception.
bool LayoutAvailable() noexcept;

inline constexpr std::int32_t kMaxRows = 64;

/// A TArray as the game's layout reads it: the list it is given.
struct ListHeader
{
    std::uint64_t* data = nullptr;
    std::int32_t   num  = 0;
    std::int32_t   max  = 0;
};

/// One row of the game's list: its FWeakObjectPtr as stored, and the widget it resolves to.
struct RowEntry
{
    std::uint64_t weak   = 0;
    void*         widget = nullptr;
};

/// The game's list header (0 if unreadable).
std::uintptr_t List() noexcept;

/// The list's rows that resolve to a widget, in the list's order, at most kMaxRows. False if the list is unreadable
/// (`rows` is then empty).
bool Rows(std::vector<RowEntry>& rows);

/// The row's viewport position (UUserWidget's viewport offsets Left, Top: what the game's slide sets).
LayoutPoint PositionOf(const void* widget) noexcept;

/// The game's layout of `list`. With `again`, each of `rows` outside `width` x `height` is named in a call of its
/// own; otherwise, or when none is outside, one call naming `placeNow` (null: none placed at once). False if the
/// game's layout raised an exception: the layout is unavailable from then on (the callers log).
bool LayOut(void* list, std::span<void* const> rows, bool again, float width, float height, void* placeNow) noexcept;

}  // namespace lss::hud::action_rows
