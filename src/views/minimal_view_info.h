#pragma once

// The FMinimalViewInfo fields the mod reads or writes, as ULocalPlayer::GetViewPoint fills the
// struct in this build. Layout read from the FMinimalViewInfo constructor 0x140CC0C98 and
// GetViewPoint's field-by-field copy (struct size 0x6A0); RE-local-player-viewpoint.md.

#include <cstddef>

namespace lss::views::view_info {

inline constexpr std::size_t kLocation = 0x00;  // FVector, three floats
inline constexpr std::size_t kPitch    = 0x0C;  // FRotator::Pitch, degrees
inline constexpr std::size_t kYaw      = 0x10;  // FRotator::Yaw, degrees (Roll +0x14)
inline constexpr std::size_t kFov = 0x18;  // float, horizontal degrees; the constructor sets 90

/// float: the shape the view is held to when kConstrainAspectRatioBit is set (constructor: 4:3).
inline constexpr std::size_t kAspectRatio = 0x2C;

/// The first byte of a uint32 bitfield: bit 0 bConstrainAspectRatio (the side bars), bit 1
/// bUseFieldOfViewForLOD. The other 30 bits are unused, and since the engine builds the struct on
/// the stack and sets the two bits one by one, they hold whatever was there before.
inline constexpr std::size_t   kFlags                   = 0x30;
inline constexpr unsigned char kConstrainAspectRatioBit = 0x01;
inline constexpr unsigned char kKnownFlagBits           = 0x03;

/// PreviousViewTransform's "is set" byte (a TOptional's flag).
inline constexpr std::size_t kPreviousViewTransformSet = 0x690;

}  // namespace lss::views::view_info
