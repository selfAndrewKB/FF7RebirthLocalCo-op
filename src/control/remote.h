#pragma once

// The console: one runner over the mod's registry, fed by two front ends on the game thread. The socket
// (control/server.h) on 127.0.0.1:kPort takes a command per line and answers a line per reply (the runner's
// ConsoleText); each connection is its own client. The file inbox (control/command_channel.h) runs its scripts as
// further clients until the regression moves to the socket. Both share the queue rules, so their commands interleave
// by tick and an `abort` reaches every client.
//
// Open after every module has registered its commands (the registry's refusals are logged then); Tick from the engine
// tick, before the session's; Close before the dev tools go (it joins the socket's thread).
//
// Driving it: `python tools/remote.py shell` (or `-c "<line>"`, `run <file>`, `status`).
//
// Taken from Remake's control/remote (2026-10-23). Changed: the port (Remake's plus one, so both games' consoles can
// listen at once); the inbox as a second front end on the one runner; a reply's log level read from its kind and
// command, not its text; `log-level` stays the platform's (platform::RegisterLogCommands); no `tick` reading.

#include "control/command_channel.h"
#include "control/runner.h"
#include "control/server.h"

#include <cstdint>
#include <optional>

namespace lss::control {

class Remote
{
public:
    static constexpr std::uint16_t kPort = 4419;

    /// The runner over the registry with `holds` for the scripts' stick holds, then the inbox and the socket, each
    /// optional (logged). False if neither opens; Tick then does nothing.
    bool Open(Holds& holds) noexcept;

    /// The lines received since the last tick (socket and inbox) to the runner, one runner tick, the replies out.
    void Tick() noexcept;

    /// Drops every queued command, releases every hold and closes the socket. Safe to call twice.
    void Close() noexcept;

private:
    std::optional<Runner> runner_;
    Server                server_;
    CommandChannel        inbox_;
    std::uint64_t         tick_ = 0;
    bool                  open_ = false;
};

}  // namespace lss::control
