#pragma once

// How many times the game has registered an actor tag, for the join and leave log.
//
// The party-member switch adds the member's first tag to a per-world registry when player 2 takes
// the member (and removes the previous one), but never removes it when the member is given back;
// the trigger actor adds its own tag when it is created. The registry is a TMap<FName, int32> held
// by the world's EndCharacterMovementActor (+0x278), which a global weak pointer names. Counts are
// read with the game's own lookup, which only reads. A missing entry means a count of zero.
//
// Game thread only; the registry is resolved afresh on every read (it belongs to the world).

#include <cstdint>
#include <optional>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::tag_counts {

bool Install(const platform::GameImage& image) noexcept;

/// An actor's first tag (an FName as its 8 raw bytes), or empty if it has none.
std::optional<std::uint64_t> FirstTagOf(const void* actor) noexcept;

/// The tag the trigger actor registers.
std::optional<std::uint64_t> TriggerTag() noexcept;

/// The registry's count for `tag`, or empty if the registry cannot be read now.
std::optional<std::int32_t> CountOf(std::uint64_t tag) noexcept;

/// "member N, trigger M" for the log (unknown values shown as "?").
struct Counts
{
    std::optional<std::int32_t> member;
    std::optional<std::int32_t> trigger;
};
Counts Read(const void* member) noexcept;

}  // namespace lss::player_two::tag_counts
