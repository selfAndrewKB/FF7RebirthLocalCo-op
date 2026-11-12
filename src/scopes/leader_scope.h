#pragma once

// The party leader answering a seat's member for one engine call (research/RE-p2-targeting.md).
//
// ResolvePartyLeaderWeakPtr reads the party manager's leader, an FWeakObjectPtr {index, serial} at
// +0x148. While a LeaderScope lives it holds the seat's member (built and checked with the game's
// own weak pointer functions; a zero serial is never written), so the game's leader-bound code picks,
// corrects, clamps or tests around the seat's member. Player 1's leader is put back when it ends,
// unless the game itself wrote the field inside the call: then the game's newer value is kept and
// noted (a Set range of platform/byte_scope.h). Game thread only.

#include "platform/byte_scope.h"
#include "players/weak_object.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::scopes::leader_scope {

/// Finds the party manager (after the weak pointers, players/weak_object.h); logged. The module table's first
/// row: every module that holds a scope comes after it.
bool Install(const platform::GameImage& image) noexcept;

/// Whether Install found the party manager; the modules that hold a scope check it at their own install.
bool Installed() noexcept;

// The weak-pointer helpers under this namespace's names, for the modules that use them here; their home is
// players/weak_object.h.
inline std::uint64_t WeakOf(void* object) noexcept
{
    return players::weak_object::WeakOf(object);
}
inline bool StillNames(std::uint64_t weak, const void* object) noexcept
{
    return players::weak_object::StillNames(weak, object);
}
inline void* ObjectAt(const void* field) noexcept
{
    return players::weak_object::ObjectAt(field);
}

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
    platform::ByteStore<1, sizeof(std::uint64_t)> context_;
    bool                                          held_ = false;
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
    platform::ByteStore<1, 1> context_;
    bool                      held_ = false;
};

}  // namespace lss::scopes::leader_scope
