#pragma once

// Where the HUD's main widgets sit in a split half: as in single player, each in its corner or edge
// of the half: compass top centre, command menu and button
// prompts bottom left, the Members panel bottom right; and the World Intel
// panel top left and the minimap top right, which the band layout alone leaves at the corners of the
// centred 16:9 band.
//
// Each of these UUserWidgets is designed for a 1920x1080 frame (research/RE-p2-hud.md and the slot
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
//
// The HUD size (the settings file's [Views] hud-scale, and its keys for players 2 and 3: each player's in their own
// view, the roots being one set painted again in each view's pass, SizeForPass) scales these roots alone, each about the corner it
// is anchored to (SetRenderTransformPivot at that corner, allowing for a root's own render translation,
// and SetRenderScale), so each stays in its corner of the view; the summon gauges, which stand between
// the minimap and the Members panel, scale about a height between the two so they keep their share of it:
// in a split window, never on a row of monitors. Everything else on the overlay (enemy names, damage
// numbers, lock-on marks, prompts over objects) keeps the game's size. The game's own scale and pivot
// are read first and set back at size 1 and when the split ends; a root is left at the game's
// transform while the game scales it. Game thread, but for the setting's console command, which only
// stores the size.

#include "hud/layout/viewport_placement.h"

namespace lss::hud {

/// Game thread, every tick: whether the HUD is laid out in split halves (more than one view shown and the
/// band on). Places new instances of the widgets while active, and restores every placed one when not.
void UpdateHudPlacement(bool splitLayout) noexcept;

/// Game thread, each frame just before the HUD overlay renders (hud/passes/root_overlay_render.h): places
/// again any widget the game set during this frame's world tick, so the frame draws it placed. Once
/// this has run, UpdateHudPlacement leaves following the game's re-sets to it.
void FollowHudPlacementBeforeRender() noexcept;

/// The corner of the frame the summon gauge (the widget whose tick places the glow) is anchored to in a split half: the
/// UI particles' boxes follow it (hud/layout/viewport_placement.h EffectViewport).
LayoutPoint GlowAnchor() noexcept;

/// The point of that widget's 1920x1080 area (fractions of it) the HUD size scales it about: its glow's box with it.
LayoutPoint GlowScaleCentre() noexcept;

/// The HUD size that widget was drawn at in `column`'s pass this frame (1 while it is not placed or not sized): the
/// particles' boxes scale with it.
float GlowSizeOf(int column) noexcept;

/// Game thread, in the overlay render: just before `column`'s pass paints, the held roots take `player`'s size (the
/// view's own player's, 0 for player 1's pass and a mirror's), each set through the game's setter only when it differs
/// and only while the root still shows the scale the mod set last (a root the game set during the frame is left to the
/// next tick's hand-back). The game's own pass, the frame's last, is given player 1's, so every tick sees player 1's.
void SizeForPass(int column, int player) noexcept;

/// Reads the HUD size from the settings file and registers its console option.
void InstallHudSize() noexcept;

}  // namespace lss::hud
