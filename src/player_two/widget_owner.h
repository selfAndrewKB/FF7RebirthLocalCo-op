#pragma once

// Player 2's widgets tick in their owner's view (RE-p2-party-list-views.md §8, pass S-CMD8).
//
// A few of the game's per-frame widget ticks were written for one command controller: they read the UI
// system's slots (EndUISystem[23] the controller, [53] the key guide) and the party leader's type
// (PM+0x150) rather than the widget's owner. Two of the game's ticks are wrapped so that the owner
// answers those reads while its widget ticks; the game's own code does the rest:
//  - the party-list cells (UEndMenuStatusPaneCell's NativeTick, a vtable entry). A cell names its owner
//    natively (+0x7B0 its party list, whose +0x20 is the controller). While a cell of player 2's
//    controller ticks, EndUISystem[23] answers that controller (its rows stack under its own rows and its
//    refresh requests reach it) and, in battle, the leader type answers the controller's own record of
//    its leader (+0x30), so player 2's member is the highlighted row;
//  - the key guide (MasterHUD_Tick's call of the key-guide tick, a call redirect). The game ticks only
//    EndUISystem[53]; the same tick then runs for player 2's key guide in player 2's view, so its widget
//    hides, shows and animates from its own state.
// Game thread only, while player 2 plays; everything else passes straight through.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::widget_owner {

/// Each part is optional and logged; false if neither could be installed.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

}  // namespace lss::player_two::widget_owner
