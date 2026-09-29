#pragma once

// The field-action icons ("Call Chocobo" and the other prompt rows, UEndFieldActionIcon /
// FieldAction_Command_C) keep their single-player corner in a split half, by the game's own layout.
//
// The game places these icons itself (RE-p2-hud-view-levers.md §1): FieldActionIconList__
// SortAndLayout gives slot i the point `_BasePosY + i * _PaddingY`, read from each icon's own
// reflected fields (+0x3C8, +0x3D0), and the icon slides there through SetPositionInViewport, which
// also resets the viewport anchors. So the anchors cannot hold the corner; the base can. The base
// comes from the Blueprint class default (70, 957): the bottom-left corner of the 1920x1080 frame.
//
// While the overlay is laid out in a band (a split half), the layout is larger than 1920x1080
// (e.g. 1920x2160 side by side). When the layout size changes, each icon class's base is moved so
// it keeps its solo distance from that corner (hud/viewport_placement.h, KeepDistanceFromAnchor):
//   - on every live icon whose base is its class's (an icon with a base of its own is logged and
//     left alone), and on the class default object, so icons created during the split inherit it;
//   - then the rows are laid out again with the game's layout (each player's rows through
//     hud/prompt_rows.h while player 2 is in, the whole list otherwise), and they slide there.
// When the split ends, the same step puts every base back (layout 1920x1080). Nothing is written
// per frame; each frame only the list's rows are checked, and a row whose base is not what this
// module left (e.g. a Blueprint graph wrote it) is logged once.
// Game thread only.

#include "hud/overlay_band.h"

namespace lss::platform {
class GameImage;
}

namespace lss::hud::action_icon_base {

/// Checks the game's layout function. False (logged): the icons keep the game's base.
bool Install(const platform::GameImage& image) noexcept;

/// Game thread, before the overlay renders: the layout size the icons are placed for
/// (kSoloLayout when not split). Moves the bases when it changed; checks the rows every call.
void Update(LayoutSize layout) noexcept;

}  // namespace lss::hud::action_icon_base
