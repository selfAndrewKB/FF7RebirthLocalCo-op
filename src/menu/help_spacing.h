#pragma once

// A Co-op page help of several lines (a help with line breaks, platform/settings.h) stands above the help bar's
// underline, as a one-line help does.
//
// The help bar is the game's footer (Foot_C, UEndFootWindow, one widget the game instance keeps): its help text
// (Txt_Help) sits in a panel (Pnl_HelpTxt) that grows with the text and draws the dark band behind it, and the
// underline (Line_Left, with its shadow Line_Left_s) is set a fixed distance below the panel's top. A text of several
// lines grows downward, so its lines after the first hang below the underline. While the Co-op page is open the panel
// is raised by the extra lines' height and the underline lowered by as much, each through its canvas slot's reflected
// SetPosition, as the game's own menus place their widgets: the band and the text rise together, the underline stays
// where it is on the screen, under the last line. One line's height is the help text's least desired height seen on
// the page (its tabs' helps are one line). Everything is put back when the page closes. Game thread. Logged: Info when
// the footer's parts are found (or why not), Debug at each change of the lift.

namespace lss::menu::help_spacing {

/// Each tick: while the Co-op page is open (`pageOpen`), the lift that its help's lines need; at the close, the
/// footer's own places again, if it is still alive. (The mod's unload may run off the game thread, where no UI call
/// is made: a page open then keeps its lift until the game rebuilds the footer.)
void Tick(bool pageOpen) noexcept;

}  // namespace lss::menu::help_spacing
