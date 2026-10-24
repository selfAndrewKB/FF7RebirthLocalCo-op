#include "player_two/scopes/primary_controller.h"

#include "facts/party.h"
#include "platform/image.h"
#include "platform/verified_code.h"
#include "players/seats.h"
#include "player_two/generation.h"
#include "player_two/weak_object.h"

#include <atomic>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::player_two::primary_controller {

namespace {

/// FWeakObjectPtr::Get(this) and FWeakObjectPtr::operator=(UObject*).
// Set once at install.
void* const* g_partyManager = nullptr;  // the global that holds the party manager

// Game thread: set by Bind, per seat (index 0 unused).
struct Bound
{
    void*                    playerOne  = nullptr;
    void*                    controller = nullptr;
    alignas(8) std::uint64_t weak       = 0;
};
Bound g_bound[players::kMaxPlayers];

std::uint64_t* PrimaryField() noexcept
{
    auto* manager = g_partyManager == nullptr ? nullptr : static_cast<std::byte*>(*g_partyManager);
    return manager == nullptr ? nullptr : reinterpret_cast<std::uint64_t*>(manager + facts::kPartyManagerPrimaryController);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto manager = image.Rebase(facts::kPartyManagerGlobal, sizeof(void*));
    if (!manager || !weak_object::Known())
    {
        return false;
    }
    g_partyManager = reinterpret_cast<void* const*>(*manager);
    return true;
}

bool Installed() noexcept
{
    return g_partyManager != nullptr;
}

bool Bind(int seat, void* playerOneController, void* controller) noexcept
{
    Unbind(seat);
    if (!players::SeatIndex(seat) || !Installed() || playerOneController == nullptr || controller == nullptr)
    {
        return false;
    }
    const std::uint64_t weak = weak_object::WeakOf(controller);
    if (weak == 0)
    {
        return false;
    }
    g_bound[seat] = {playerOneController, controller, weak};
    return true;
}

void Unbind(int seat) noexcept
{
    if (players::SeatIndex(seat))
    {
        g_bound[seat] = {};
    }
}

std::uint64_t WeakValueOf(void* object) noexcept
{
    return Installed() ? weak_object::WeakOf(object) : 0;
}

bool Is(const void* controller) noexcept
{
    return controller != nullptr && Current() == controller;
}

void* Current() noexcept
{
    const std::uint64_t* field = Installed() ? PrimaryField() : nullptr;
    return weak_object::ObjectAt(field);
}

Scope::Scope(int seat) noexcept
{
    if (!players::SeatIndex(seat) || g_bound[seat].controller == nullptr)
    {
        return;  // refusal_ stays NotBound
    }
    Bound&         bound = g_bound[seat];
    std::uint64_t* field = PrimaryField();
    if (field == nullptr)
    {
        refusal_ = Refusal::NoPartyManager;
        return;
    }
    if (weak_object::ObjectAt(&bound.weak) != bound.controller)
    {
        refusal_ = Refusal::PlayerTwoStale;
        return;
    }
    const std::uint64_t current = std::atomic_ref<std::uint64_t>(*field).load();
    if (current != bound.weak && weak_object::ObjectAt(field) != bound.playerOne)
    {
        refusal_ = Refusal::NotPlayerOne;
        return;
    }
    switch (window_.Open(field, current, bound.weak))
    {
        case SwapWindow::Opened::Swapped:
        case SwapWindow::Opened::AlreadyHeld: refusal_ = Refusal::None; break;
        case SwapWindow::Opened::Changed: refusal_ = Refusal::Changed; break;
        case SwapWindow::Opened::Misaligned: refusal_ = Refusal::Misaligned; break;
    }
}

Scope::~Scope()
{
    if (!window_.Close())
    {
        generation::Fault(L"the party manager's primary controller changed while it named a further player");
    }
}

const wchar_t* Describe(Scope::Refusal refusal) noexcept
{
    switch (refusal)
    {
        case Scope::Refusal::None: return L"held";
        case Scope::Refusal::NotBound: return L"not bound to the seat";
        case Scope::Refusal::NoPartyManager: return L"no party manager";
        case Scope::Refusal::PlayerTwoStale: return L"the seat's controller is gone";
        case Scope::Refusal::NotPlayerOne: return L"the primary controller is neither player 1's nor the seat's";
        case Scope::Refusal::Changed: return L"the primary controller changed at the swap";
        case Scope::Refusal::Misaligned: return L"the primary-controller field is misaligned";
    }
    return L"unknown";
}

}  // namespace lss::player_two::primary_controller
