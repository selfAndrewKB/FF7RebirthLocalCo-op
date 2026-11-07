#pragma once

// Which text blocks the game lays out again, where in the frame, and why (logging only; RE-perf-solo-vs-split §12).
// A dev tool: dev/dev_tools.h installs it; the minimap modules mark the scopes and tick it through the stubs below
// in a player's build.
//
// A capture found two text relayouts every frame in a seat's HUD pass (5.7 ms a frame on the game thread): one in
// the seat's minimap holder run, through the game's SetText, and one in the seat's overlay paint. This
// module watches both ways a Square Enix text block (SEndTextBlock) is laid out again, without changing either:
// - SetText (RE-p2-minimap-cost.md §7.1): the game stores the new text and relayouts unless the text is the same (and
//   short). minimap_labels' SetText redirect reports each call; the stored text pointer tells whether it relayouted;
// - Tick, slot 1 of the text block's vtable, which the paint calls: it relayouts if a relayout is pending (+0x565), or
//   if wrapping or justification is on and the allotted width differs from the last one (+0x560). The slot is
//   replaced by a function that calls the game's own and compares those two fields around it.
// Per text block it counts relayouts by where they ran (each player's holder run, each seat's paint, anywhere else)
// and keeps the text and the reason. Every 5 s: one Debug summary line; an Info line when a block starts or stops
// relaying out about every frame. Each relayout is a Trace line. Watches only while a seat's minimap is on. Game
// thread.

#include "platform/dev_switch.h"

namespace lss::platform {
class GameImage;
}

namespace lss::hud::label_relayouts {

/// Where in the frame a relayout runs.
enum class Scope : unsigned char
{
    Other,  // outside the mod's runs and the seats' paints (player 1's paint, other UI)
    Run,    // the mod's minimap holder run for a player (0 player 1, else a seat)
    Paint,  // a seat's overlay render
};

/// What the text block held before the game's SetText.
struct Before
{
    const void*   data   = nullptr;  // the FString's buffer
    int           num    = 0;        // its Num (characters + the null)
    bool          read   = false;
    bool          same   = false;    // the new text equals the old one
    bool          locked = false;    // +0x564: the relayout only marks itself pending
};

#if LSS_DEV

/// Replaces the text block's Tick slot. Logs either way; without it only SetText relayouts are counted.
bool Install(const platform::GameImage& image) noexcept;
void Remove() noexcept;

/// Where the frame is (hud/minimap/minimap.cpp and minimap_labels around the runs and a seat's paint). `player`:
/// whose run or paint (0 player 1, else a seat); not read for Other.
void SetScope(Scope scope, int player = 0) noexcept;

/// minimap_labels' SetText redirect, around the game's SetText (`text` is the FString* it passes).
Before BeforeSetText(const void* textBlock, const void* text) noexcept;
void   AfterSetText(const void* textBlock, const Before& before, bool seatState) noexcept;

/// Each engine tick (hud/minimap/minimap.cpp): whether to watch (a seat's minimap is on), and the 5-second lines.
void Tick(bool watch) noexcept;

#else

inline bool   Install(const platform::GameImage&) noexcept { return false; }
inline void   Remove() noexcept {}
inline void   SetScope(Scope, int = 0) noexcept {}
inline Before BeforeSetText(const void*, const void*) noexcept { return {}; }
inline void   AfterSetText(const void*, const Before&, bool) noexcept {}
inline void   Tick(bool) noexcept {}

#endif

}  // namespace lss::hud::label_relayouts
