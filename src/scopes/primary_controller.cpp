#include "scopes/primary_controller.h"

#include "facts/party.h"
#include "platform/image.h"
#include "platform/verified_code.h"
#include "players/generation.h"
#include "players/seats.h"
#include "players/weak_object.h"

#include <atomic>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::scopes::primary_controller {

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
    if (!manager || !players::weak_object::Known())
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
    const std::uint64_t weak = players::weak_object::WeakOf(controller);
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
    return Installed() ? players::weak_object::WeakOf(object) : 0;
}

bool Is(const void* controller) noexcept
{
    return controller != nullptr && Current() == controller;
}

void* Current() noexcept
{
    const std::uint64_t* field = Installed() ? PrimaryField() : nullptr;
    return players::weak_object::ObjectAt(field);
}

int OpenSeat() noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (players::generation::Active(seat) && Is(players::generation::ControllerOf(seat)))
        {
            return seat;
        }
    }
    return 0;
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
    if (players::weak_object::ObjectAt(&bound.weak) != bound.controller)
    {
        refusal_ = Refusal::SeatStale;
        return;
    }
    const std::uint64_t current = std::atomic_ref<std::uint64_t>(*field).load();
    if (current != bound.weak && players::weak_object::ObjectAt(field) != bound.playerOne)
    {
        refusal_ = Refusal::NotPlayerOne;
        return;
    }
    Swap(field, current, bound.weak);
}

Scope::Scope(void* playerOne, void* controller) noexcept
{
    const std::uint64_t weak =
        Installed() && playerOne != nullptr && controller != nullptr ? players::weak_object::WeakOf(controller) : 0;
    if (weak == 0)
    {
        return;  // refusal_ stays NotBound
    }
    std::uint64_t* field = PrimaryField();
    if (field == nullptr)
    {
        refusal_ = Refusal::NoPartyManager;
        return;
    }
    const std::uint64_t current = std::atomic_ref<std::uint64_t>(*field).load();
    if (players::weak_object::ObjectAt(field) != playerOne)
    {
        refusal_ = Refusal::NotPlayerOne;
        return;
    }
    Swap(field, current, weak);
}

void Scope::Swap(std::uint64_t* field, std::uint64_t current, std::uint64_t replacement) noexcept
{
    switch (window_.Open(field, current, replacement))
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
        players::generation::Fault(L"the party manager's primary controller changed while it named another player than "
                                   L"player 1");
    }
}

const wchar_t* Describe(Scope::Refusal refusal) noexcept
{
    switch (refusal)
    {
        case Scope::Refusal::None: return L"held";
        case Scope::Refusal::NotBound: return L"not bound to the player";
        case Scope::Refusal::NoPartyManager: return L"no party manager";
        case Scope::Refusal::SeatStale: return L"the player's controller is gone";
        case Scope::Refusal::NotPlayerOne: return L"the primary controller is neither player 1's nor that player's";
        case Scope::Refusal::Changed: return L"the primary controller changed at the swap";
        case Scope::Refusal::Misaligned: return L"the primary-controller field is misaligned";
    }
    return L"unknown";
}

}  // namespace lss::scopes::primary_controller
