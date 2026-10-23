#include "player_two/scopes/leader_scope.h"

#include "facts/party.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"

#include <cstdint>
#include <cstring>

namespace lss::player_two::leader_scope {

namespace {

// The party manager's leader, an FWeakObjectPtr {index, serial} read by ResolvePartyLeaderWeakPtr, and its type.
using facts::kPartyManagerLeader;
using facts::kPartyManagerLeaderType;

using Kind = platform::ByteContext::Kind;

// Set once at install.
std::uintptr_t g_partyManager = 0;  // the address holding the party manager

/// The party manager object now, or 0.
std::uintptr_t Manager() noexcept
{
    std::uintptr_t manager = 0;
    return g_partyManager != 0 &&
                   platform::CopyGuarded(&manager, reinterpret_cast<const void*>(g_partyManager), sizeof(manager))
               ? manager
               : 0;
}

/// `value` written into the manager's field at `offset` for the scope's length: one Set range, opened. False if the
/// manager is missing or the field could not be read or written (then nothing is left changed).
template <class T>
bool OpenSet(platform::ByteContext& context, std::uint32_t offset, const T& value) noexcept
{
    const std::uintptr_t manager = Manager();
    if (manager == 0)
    {
        return false;
    }
    const platform::ByteContext::Range range{manager + offset, sizeof(T), Kind::Set};
    if (!context.Define(std::span<const platform::ByteContext::Range>(&range, 1)))
    {
        return false;
    }
    std::memcpy(context.Value(0).data(), &value, sizeof(T));
    return context.Open();
}

/// A Set range the game wrote inside the call keeps the game's newer value (platform/byte_scope.h): noted.
void Kept(const wchar_t* what) noexcept
{
    platform::LogDebug(L"leader scope: the game wrote the party manager's {} inside player 2's call; the game's value "
                       L"kept",
                       what);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto manager = image.Rebase(facts::kPartyManagerGlobal, sizeof(void*));
    g_partyManager     = weak_object::Known() && manager ? *manager : 0;
    platform::Log(L"leader scope: {}",
                  g_partyManager != 0 ? L"ready (player 2's member answers as the party leader inside the game's calls)"
                                      : L"UNAVAILABLE (the party manager or the weak pointers were not found)");
    return g_partyManager != 0;
}

bool Installed() noexcept
{
    return g_partyManager != 0;
}

LeaderScope::LeaderScope(std::uint64_t memberWeak) noexcept
{
    held_ = memberWeak != 0 && OpenSet(context_, kPartyManagerLeader, memberWeak);
}

LeaderScope::~LeaderScope()
{
    if (held_ && context_.Close() != 0)
    {
        Kept(L"leader");
    }
}

LeaderTypeScope::LeaderTypeScope(std::uint8_t type) noexcept
{
    held_ = OpenSet(context_, kPartyManagerLeaderType, type);
}

LeaderTypeScope::~LeaderTypeScope()
{
    if (held_ && context_.Close() != 0)
    {
        Kept(L"leader type");
    }
}

}  // namespace lss::player_two::leader_scope
