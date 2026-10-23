#pragma once

// The control commands not yet beside their owners (phase 8 of the cleanup, rows 5 to 7, each moving one group): the
// HUD, the renderer and player 2's modules. Each does what its key or setting
// does, but asks for a state rather than switching it, and says why when it cannot. Called once by the module table's
// file (modules::RegisterCommands), before the command channel opens. Game thread only.

namespace lss::control {

void RegisterModCommands();

}  // namespace lss::control
