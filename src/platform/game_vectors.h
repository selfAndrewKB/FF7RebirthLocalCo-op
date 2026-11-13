#pragma once

// The engine's FVector2D as this engine lays it out: two 4-byte floats, packed, no padding. Pure.
//
// The one copy of that layout in the mod. A size passed by value to a game function is one of these, so a build of an
// engine whose vectors differ (wider floats, other padding) is refused here instead of misread in one module. It is
// 8 bytes, so the x64 convention passes it packed in one register, as the game's own callers do.

namespace lss::platform {

/// FVector2D: a screen or layout point, or a size.
struct GameVector2D
{
    float x = 0.0f, y = 0.0f;
};
static_assert(sizeof(GameVector2D) == 8 && alignof(GameVector2D) == 4, "the engine's FVector2D: two packed 4-byte floats");

}  // namespace lss::platform
