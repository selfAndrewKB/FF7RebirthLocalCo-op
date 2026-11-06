#pragma once

// Each further player's widgets tick in their owner's view (RE-p2-party-list-views.md §8, pass S-CMD8).
//
// A few of the game's per-frame widget ticks were written for one command controller: they read the UI
// system's slots (EndUISystem[23] the controller, [53] the key guide) and the party leader's type
// (PM+0x150) rather than the widget's owner. Three of the game's ticks are wrapped so that the owner
// answers those reads while its widget ticks; the game's own code does the rest:
//  - the party-list cells (UEndMenuStatusPaneCell's NativeTick, a vtable entry). A cell names its owner
//    natively (+0x7B0 its party list, whose +0x20 is the controller). While a cell of a seat's
//    controller ticks, EndUISystem[23] answers that controller (its rows stack under its own rows and its
//    refresh requests reach it) and, in battle, the leader type answers the controller's own record of
//    its leader (+0x30), so the seat's member is the highlighted row;
//  - the key guide (MasterHUD_Tick's call of the key-guide tick, a call redirect). The game ticks only
//    EndUISystem[53]; the same tick then runs for each seat's key guide in that seat's view, so its widget
//    hides, shows and animates from its own state;
//  - the synergy pane (UEndCmdLinkupPane's NativeTick, a vtable entry; RE-p2-synergy-pane.md). It rebuilds
//    its rows whenever the leader type differs from the character it lists; its owner is recorded at menu
//    setup (+0x3C8). A seat's pane ticks with the leader type answering the seat's controller, so it
//    keeps listing the seat's member's synergy skills (the controller's own tick already set it so).
// Game thread only, while a seat plays; everything else passes straight through.
//
// Per seat: a widget of any built seat's controller ticks in that seat's view (the seat
// found from the widget's owner, the controller it names); the key guides tick after the game's, in seat order, each
// view closed before the next seat's.

namespace lss::platform {
class GameImage;
}

namespace lss::battle::widget_owner {

/// Each part is optional and logged; false if none could be installed.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

}  // namespace lss::battle::widget_owner
