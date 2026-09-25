#pragma once

// Where the HUD's main widgets sit in a split half: as in single player, each in its corner or edge
// of the half (the user's reference, 2026-09-25): compass top centre, command menu and button
// prompts bottom left, the Members panel bottom right.
//
// Each of these UUserWidgets is designed for a 1920x1080 frame (RE-p2-hud.md pass 6 and the slot
// dumps): the command menu and prompts keep a centred 1920x1080 box, the compass anchors its parts
// to the centre, the Members panel places them at absolute solo positions. In single player the
// widget's own area is the whole 1920x1080 layout, so all of that lands in the corners; in a
// 3840x1080 or 1920x2160 half it does not.
//
// The game gives every root widget on its HUD overlay a viewport slot: anchors, alignment and
// offsets, which the overlay's canvas reads each frame (UUserWidget's viewport layout, as
// AddToViewport uses it). While the HUD is laid out in split halves, each of these widgets gets,
// through its own reflected setters (SetAnchorsInViewport, SetAlignmentInViewport,
// SetDesiredSizeInViewport, SetPositionInViewport), a 1920x1080 area anchored to its corner or
// edge of the half. Its contents keep their own design, so the padding is solo's. In a 1920x1080
// layout every such area is the whole layout, so single player is unchanged. The original viewport
// layout is read first and set back through the same setters when the split ends.
//
// Some roots the game places itself at a point of the 1920x1080 frame, with SetPositionInViewport
// (anchors (0,0)): the field action prompt ("Call Chocobo") at (70,957). Such a point keeps its
// distance from the frame's nearest corner or edge: anchored there, offset by the same amount. The
// game may set the point again at any time (which resets the anchors), so every placed widget is
// compared each frame, just before the overlay renders, with what was set, and placed again from the
// game's new layout.

namespace RC::Unreal {
class UObject;
}

namespace lss::hud {

/// Game thread, every tick: whether the HUD is laid out in split halves (player 2 in and the band
/// on). Places new instances of the widgets while active, and restores every placed one when not.
void UpdateHudPlacement(bool splitLayout) noexcept;

/// Game thread, each frame just before the HUD overlay renders (hud/root_overlay_render.h): places
/// again any widget the game set during this frame's world tick, so the frame draws it placed. Once
/// this has run, UpdateHudPlacement leaves following the game's re-sets to it.
void FollowHudPlacementBeforeRender() noexcept;

/// Game thread, before the overlay renders: places `widget` now if it is one of the kinds placed
/// here and not followed yet. For widgets the game creates between scans that are known at once (a
/// new prompt row), which would otherwise be drawn at the game's own point until the next scan.
void PlaceNewHudWidget(RC::Unreal::UObject* widget) noexcept;

}  // namespace lss::hud
