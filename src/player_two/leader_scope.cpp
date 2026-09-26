#include "player_two/leader_scope.h"

#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/verified_code.h"
#include "player_two/engine_facts.h"

#include <cstdint>

namespace lss::player_two::leader_scope {

namespace {

// The party manager's leader, an FWeakObjectPtr {index, serial} read by ResolvePartyLeaderWeakPtr.
constexpr std::uint32_t kPartyManagerLeader = 0x148;

using WeakGetFn    = void* (*)(const std::uint64_t* weak);
using WeakAssignFn = void (*)(std::uint64_t* weak, void* object);

// Set once at install.
WeakGetFn      g_weakGet      = nullptr;
WeakAssignFn   g_weakAssign   = nullptr;
std::uintptr_t g_partyManager = 0;  // the address holding the party manager

std::uint64_t WeakOfGuarded(void* object) noexcept
{
    __try
    {
        std::uint64_t weak = 0;
        g_weakAssign(&weak, object);
        return (weak >> 32) != 0 && g_weakGet(&weak) == object ? weak : 0;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return 0;
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto get     = platform::FindFunction(image, facts::kWeakObjectGet);
    const auto assign  = platform::FindFunction(image, facts::kWeakObjectAssign);
    const auto manager = image.Rebase(facts::kPartyManagerGlobal, sizeof(void*));
    if (!get || !assign || !manager)
    {
        return false;
    }
    g_weakGet      = reinterpret_cast<WeakGetFn>(*get);
    g_weakAssign   = reinterpret_cast<WeakAssignFn>(*assign);
    g_partyManager = *manager;
    return true;
}

std::uint64_t WeakOf(void* object) noexcept
{
    return object != nullptr && g_weakAssign != nullptr ? WeakOfGuarded(object) : 0;
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

}  // namespace lss::player_two::leader_scope
