#pragma once

// The game's own way to put a character beside another on the field, as its battle gather calls it
// (research/RE-member-story-boxes.md §9):
//   - a navmesh point at a distance from an origin along a direction: the game's `around` query (the point projected to
//     the navmesh and reached by a navmesh raycast from the origin, with the querier's own nav agent);
//   - the game's battle-warp move on a character there: a queued warp request (the movement applies it as a one-frame
//     move) and a short fade-in. Nothing battle-only runs inside it.
// The character's own trigger tracker sweeps the whole path of the move on its next pass (§9.1): a caller that must not
// see a box on the way fire keeps its own hold in force over that pass.
// Found and verified once at install; callers check Installed(). Game thread.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::world::catch_up_move {

bool Install(const platform::GameImage& image) noexcept;
bool Installed() noexcept;

/// A following companion's distance from the character it is brought to.
inline constexpr float kFollowerCm = 150.0f;

/// A navmesh point `distanceCm` from `origin` (world cm) along the flat direction `yawRadians`, for `querier`'s nav agent
/// (the game's query with one distance and one angle; it may answer a nearer point when a wall is near). False when the
/// game finds none or the call raises.
bool PointAround(void* querier, const float origin[3], float yawRadians, float distanceCm, float out[3]) noexcept;

/// Moves `character` to stand on `point`, facing `yawDegrees`, with the game's fade-in. `look`: the request's own
/// location (the battle gather passes its anchor's; null: `point`). `resetControlHolder`: the character's control holder
/// back to its first state, as the gather does in a fight. False when the call raised (the character may stand where
/// it was).
bool Move(void* character, const float point[3], float yawDegrees, const float* look = nullptr,
          bool resetControlHolder = false) noexcept;

/// Another player's character than `except` stands within 1 m of `at`: not a spot to bring `except` to.
bool SpotTaken(const float at[3], const void* except) noexcept;

}  // namespace lss::world::catch_up_move
