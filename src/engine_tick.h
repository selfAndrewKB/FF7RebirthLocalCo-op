#pragma once

// The mod's heartbeat: a call before each UGameEngine::Tick, on the game thread, before any world ticks. UE4SS
// c0a8d4c (the build players get from Nexus Mods) offers no engine-tick callback, and its viewport-client tick runs
// after the worlds have ticked (research/.ida-mcp/1005_UGameEngine_Tick.c: world ticks, viewport tick, redraw), so
// the mod sends the engine's Tick entry through itself (platform/entry_redirect.h), as the newer loader's hook did.
// The redirect stays for the session (the DLL is pinned); Start and Stop set what it calls.

#include "platform/image.h"

namespace lss::engine_tick {

using Callback = void (*)(void* self);

/// Places the redirect on UGameEngine::Tick, after checking the function's bytes and that the engine object's
/// vtable holds it at the loader's layout slot. False (logged) when any check fails; nothing is placed then.
bool Install(const platform::GameImage& image) noexcept;

/// `callback(self)` runs before each engine tick from now on (any thread may call this; the next tick sees it). Once
/// per run, after everything the callback touches is in place.
void Start(Callback callback, void* self) noexcept;

/// No further calls; the redirect passes the tick straight through. Called off the game thread, it returns only once
/// a tick already inside the callback has left it, so the caller may tear down what the callback used.
void Stop() noexcept;

}  // namespace lss::engine_tick
