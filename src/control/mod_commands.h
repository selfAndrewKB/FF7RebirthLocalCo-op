#pragma once

// The mod's control commands, registered in one place for now (phase 8, row 2 of the control port): each does what
// its key does, but asks for a state (own camera on, per-pad sticks on) rather than switching it, and says why when it
// cannot. The later rows move each group beside its owner (plan/cleanup-checklist.md, phase 8). Game thread only;
// called once, before the command channel opens.
//
// The readings the waits compare: `p2.in` (on while player 2's local player exists), `p2.state` (out, awaiting, in,
// playing), `field.ready` (a save's field is up and nothing is over it).

namespace lss::player_two {
class Session;
}

namespace lss::players {
class SingleView;
class SplitLayout;
}

namespace lss::control {

void RegisterModCommands(player_two::Session& session, players::SplitLayout& layout,
                         players::SingleView& singleView) noexcept;

}  // namespace lss::control
