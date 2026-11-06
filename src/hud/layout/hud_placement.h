#pragma once

// Where the HUD's main widgets sit in a split half: as in single player, each in its corner or edge
// of the half (the user's reference, 2026-09-25): compass top centre, command menu and button
// prompts bottom left, the Members panel bottom right; and (the user, 2026-10-26) the World Intel
// panel top left and the minimap top right, which the band layout alone kept at the corners of the
// centred 16:9 band (plan/topics/hud.md Open, the regression's pictures).
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
// The game rarely sets these layouts again; when it does, the widget is placed again from the
// game's new layout (compared each frame just before the overlay renders). The field-action icons
// ("Call Chocobo"), which the game places itself at points of the frame, keep their corner through
// their own base instead (hud/field/action_icon_base.h).

namespace lss::hud {

/// Game thread, every tick: whether the HUD is laid out in split halves (more than one view shown and the
/// band on). Places new instances of the widgets while active, and restores every placed one when not.
void UpdateHudPlacement(bool splitLayout) noexcept;

/// Game thread, each frame just before the HUD overlay renders (hud/passes/root_overlay_render.h): places
/// again any widget the game set during this frame's world tick, so the frame draws it placed. Once
/// this has run, UpdateHudPlacement leaves following the game's re-sets to it.
void FollowHudPlacementBeforeRender() noexcept;

}  // namespace lss::hud
