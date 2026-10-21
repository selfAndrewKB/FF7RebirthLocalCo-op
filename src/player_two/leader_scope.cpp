#include "player_two/leader_scope.h"

#include "facts/party.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/verified_code.h"

#include <cstdint>

namespace lss::player_two::leader_scope {

namespace {

// The party manager's leader, an FWeakObjectPtr {index, serial} read by ResolvePartyLeaderWeakPtr.
constexpr std::uint32_t kPartyManagerLeader = 0x148;
constexpr std::uint32_t kPartyManagerLeaderType = 0x150;  // u8

// Set once at install.
std::uintptr_t g_partyManager = 0;  // the address holding the party manager

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto manager = image.Rebase(facts::kPartyManagerGlobal, sizeof(void*));
    if (!weak_object::Known() || !manager)
    {
        return false;
    }
    g_partyManager = *manager;
    return true;
}

LeaderScope::LeaderScope(std::uint64_t memberWeak) noexcept
{
    std::uintptr_t manager = 0;
    if (memberWeak == 0 || g_partyManager == 0 ||
        !platform::CopyGuarded(&manager, reinterpret_cast<const void*>(g_partyManager), sizeof(manager)) || manager == 0 ||
        !platform::CopyGuarded(&savedLeader_, reinterpret_cast<const void*>(manager + kPartyManagerLeader),
                               sizeof(savedLeader_)))
    {
        return;
    }
    leader_ = manager + kPartyManagerLeader;
    held_   = platform::WriteGuarded(reinterpret_cast<void*>(leader_), &memberWeak, sizeof(memberWeak));
}

LeaderScope::~LeaderScope()
{
    if (leader_ != 0)
    {
        platform::WriteGuarded(reinterpret_cast<void*>(leader_), &savedLeader_, sizeof(savedLeader_));
    }
}

LeaderTypeScope::LeaderTypeScope(std::uint8_t type) noexcept
{
    std::uintptr_t manager = 0;
    if (g_partyManager == 0 ||
        !platform::CopyGuarded(&manager, reinterpret_cast<const void*>(g_partyManager), sizeof(manager)) || manager == 0 ||
        !platform::CopyGuarded(&saved_, reinterpret_cast<const void*>(manager + kPartyManagerLeaderType), sizeof(saved_)))
    {
        return;
    }
    address_ = manager + kPartyManagerLeaderType;
    held_    = platform::WriteGuarded(reinterpret_cast<void*>(address_), &type, sizeof(type));
}

LeaderTypeScope::~LeaderTypeScope()
{
    if (held_)
    {
        platform::WriteGuarded(reinterpret_cast<void*>(address_), &saved_, sizeof(saved_));
    }
}

}  // namespace lss::player_two::leader_scope