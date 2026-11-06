#pragma once

// The field prompt rows (FieldAction_Command_C): each player sees their own, in their own view
// (RE-p2-field-prompts.md; per seat since plan/seats.md step 4).
//
// Every prompt row the game shows is a widget in one global list (EndUISystem+0x208: a pointer to
// {FWeakObjectPtr* data, int32 num}). The rows have no link back to their owner, but their owners
// keep weak pointers to them:
//   - each player controller's leader-action component (PC+0x598) has 14 row handles: a request
//     byte at +0x9A8+12*i and the row's weak widget at +0x9AC+12*i (hookshot/"Terrain Action",
//     parkour, catch, memo...). A seat's component detects from the seat's own character, so its
//     rows are the seat's;
//   - a seat's driven chocobo's row handles, and the seat's copy of the field-prompt manager's slot
//     rows (world/field_pickup.h: the pickup rows the seat's pass of the game's update made);
//   - everything else (the talk/examine manager, which follows the leader; the move-state summons;
//     minigame and encounter registrars) is player 1's.
// A row is a seat's while the seat's component names it, and stays so until it leaves the list
// (its removal animation runs after the component has let go of it).
//
// Once per frame before the overlay renders (while a seat is in):
//   - Refresh reads and classifies the list. When it changed, each player's rows are laid out on
//     their own with the game's own layout (FieldActionIconList__SortAndLayout on a list of that
//     player's rows), so each half stacks only its own rows from the top slot; the game lays out the
//     whole list only when a row is added or removed, and this runs after it in the same frame.
//     Changes are logged.
//   - Around each overlay pass, ShowOnly makes the other players' rows fully transparent
//     (UWidget::SetRenderOpacity 0) and Restore puts back each row's own opacity. Not Hidden: a
//     UMG widget is ticked only while it is painted, and a seat's pass runs with DeltaTime 0, so a
//     row hidden in player 1's pass would never advance its slide and fade-out timers and would
//     never be removed (seen in v0.12.3: player 2's rows stayed on screen). A transparent row is
//     still painted and ticked in player 1's pass, with the real DeltaTime.
// Game thread only.

namespace lss::platform {
class GameImage;
}

namespace lss::hud::prompt_rows {

bool Install(const platform::GameImage& image) noexcept;

/// Before the overlay renders, while a seat is in.
void Refresh() noexcept;

/// Before an overlay pass: only the seat's rows (0 player 1's) stay shown.
void ShowOnly(int seat) noexcept;

/// After the passes: every row made transparent here gets its own opacity back.
void Restore() noexcept;

/// Every seat is out: forgets every row and owner (widgets are remembered by address).
void Forget() noexcept;

/// The seat is out while another stays: the rows it owned are forgotten.
void Forget(int seat) noexcept;

/// The next Refresh lays each player's rows out again with the game's layout, even if the list did
/// not change (the rows' base changed: hud/field/action_icon_base.h), for a layout of `width` x `height`
/// units. Rows slide to their new slots as the game slides them, except a row whose current place
/// is outside that layout: it is not painted there, so it is not ticked and would never slide, and
/// is placed at once instead (as the game places a row it adds).
void LayOutAgain(float width, float height) noexcept;

}  // namespace lss::hud::prompt_rows
