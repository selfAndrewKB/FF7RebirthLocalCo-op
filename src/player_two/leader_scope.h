#pragma once

// The party leader answering player 2's member for one engine call (RE-p2-targeting.md §5.2, §5.3).
//
// ResolvePartyLeaderWeakPtr reads the party manager's leader, an FWeakObjectPtr {index, serial} at
// +0x148. While a LeaderScope lives it holds player 2's member (built and checked with the game's
// own weak pointer functions; a zero serial is never written), so the game's leader-bound code picks,
// corrects, clamps or tests around player 2's member. Player 1's leader is put back when it ends.
// Game thread only.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::leader_scope {

bool Install(const platform::GameImage& image) noexcept;

/// `object` as an FWeakObjectPtr value {index, serial}, or 0 if it cannot be (or the serial would be 0).
std::uint64_t WeakOf(void* object) noexcept;

/// Whether `weak` (from WeakOf) still resolves to `object`: false once the object is destroyed.
bool StillNames(std::uint64_t weak, const void* object) noexcept;

class LeaderScope
{
public:
    /// `memberWeak` from WeakOf; 0 holds nothing.
    explicit LeaderScope(std::uint64_t memberWeak) noexcept;
    ~LeaderScope();
    LeaderScope(const LeaderScope&)            = delete;
    LeaderScope& operator=(const LeaderScope&) = delete;

    bool Held() const noexcept { return held_; }

private:
    std::uintptr_t leader_      = 0;
    std::uint64_t  savedLeader_ = 0;
    bool           held_        = false;
};

/// The party manager's leader type (u8 at +0x150, read by code that wants the leader's type rather
/// than the leader itself, e.g. the command controller's leader ordinal and Setup) answering `type`
/// while alive; player 1's put back when it ends. Pair it with a LeaderScope.
class LeaderTypeScope
{
public:
    explicit LeaderTypeScope(std::uint8_t type) noexcept;
    ~LeaderTypeScope();
    LeaderTypeScope(const LeaderTypeScope&)            = delete;
    LeaderTypeScope& operator=(const LeaderTypeScope&) = delete;

    bool Held() const noexcept { return held_; }

private:
    std::uintptr_t address_ = 0;
    std::uint8_t   saved_   = 0;
    bool           held_    = false;
};

}  // namespace lss::player_two::leader_scope
