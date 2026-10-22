#pragma once

// The engine's split-screen settings the mod reads and writes through reflection: the two- and three-player
// layouts on UGameMapsSettings' class default object (the value the engine reads every frame), the offset of
// ULocalPlayer::AspectRatioAxisConstraint (the FOV rule, views/player_view_point.h), and the game viewport's
// split switch (bDisableSplitScreenOverride, players/single_view.h) through UGameplayStatics. Each byte field is
// taken only where reflection finds it as one byte at the offset the engine reads (facts/engine.h), so a
// different engine layout is refused, not written. Game thread.

#include <cstdint>
#include <optional>

namespace RC::Unreal {
class UObject;
}

namespace lss::players::split_settings {

/// UGameMapsSettings::TwoPlayerSplitscreenLayout on the class default object, or empty if reflection cannot find
/// it where the engine reads it (facts::kTwoPlayerSplitscreenLayoutOffset, one byte).
std::optional<std::uint8_t> TwoPlayerLayout() noexcept;

/// Writes it; false (writing nothing) under the same conditions.
bool SetTwoPlayerLayout(std::uint8_t value) noexcept;

/// UGameMapsSettings::ThreePlayerSplitscreenLayout, as the two-player one
/// (facts::kThreePlayerSplitscreenLayoutOffset): the layout the engine uses with three local players.
std::optional<std::uint8_t> ThreePlayerLayout() noexcept;
bool SetThreePlayerLayout(std::uint8_t value) noexcept;

/// The offset of ULocalPlayer::AspectRatioAxisConstraint as reflection reports it, if it is the
/// one-byte field the engine reads (facts::kAspectRatioAxisConstraintOffset).
std::optional<std::uint32_t> AspectRatioAxisConstraintOffset() noexcept;

/// UGameplayStatics::IsSplitscreenForceDisabled: whether the game viewport's stock
/// bDisableSplitScreenOverride is set (players/single_view.h), or empty if the call failed.
std::optional<bool> ForceDisabled(RC::Unreal::UObject* worldContext) noexcept;

/// UGameplayStatics::SetForceDisableSplitscreen: sets or clears that switch; the engine lays the
/// players out again in the same call. False if it could not be called.
bool SetForceDisabled(RC::Unreal::UObject* worldContext, bool disable) noexcept;

}  // namespace lss::players::split_settings
