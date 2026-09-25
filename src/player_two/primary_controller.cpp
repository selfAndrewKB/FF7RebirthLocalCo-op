#include "player_two/primary_controller.h"

#include "platform/image.h"
#include "platform/verified_code.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"

#include <atomic>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::player_two::primary_controller {

namespace {

/// FWeakObjectPtr::Get(this) and FWeakObjectPtr::operator=(UObject*).
using WeakGetFn    = void* (*)(const std::uint64_t* weak);
using WeakAssignFn = void (*)(std::uint64_t* weak, void* object);

// Set once at install.
void* const*  g_partyManager = nullptr;  // the global that holds the party manager
WeakGetFn     g_weakGet      = nullptr;
WeakAssignFn  g_weakAssign   = nullptr;

// Game thread: set by Bind.
void*                    g_playerOne = nullptr;
void*                    g_playerTwo = nullptr;
alignas(8) std::uint64_t g_playerTwoWeak = 0;

std::uint64_t* PrimaryField() noexcept
{
    auto* manager = g_partyManager == nullptr ? nullptr : static_cast<std::byte*>(*g_partyManager);
    return manager == nullptr ? nullptr : reinterpret_cast<std::uint64_t*>(manager + facts::kPartyManagerPrimaryController);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto manager = image.Rebase(facts::kPartyManagerGlobal, sizeof(void*));
    const auto get     = platform::FindFunction(image, facts::kWeakObjectGet);
    const auto assign  = platform::FindFunction(image, facts::kWeakObjectAssign);
    if (!manager || !get || !assign)
    {
        return false;
    }
    g_partyManager = reinterpret_cast<void* const*>(*manager);
    g_weakGet      = reinterpret_cast<WeakGetFn>(*get);
    g_weakAssign   = reinterpret_cast<WeakAssignFn>(*assign);
    return true;
}

bool Installed() noexcept
{
    return g_weakGet != nullptr;
}

bool Bind(void* playerOneController, void* playerTwoController) noexcept
{
    Unbind();
    if (!Installed() || playerOneController == nullptr || playerTwoController == nullptr)
    {
        return false;
    }
    std::uint64_t weak = 0;
    g_weakAssign(&weak, playerTwoController);
    if (weak == 0 || g_weakGet(&weak) != playerTwoController)
    {
        return false;
    }
    g_playerTwoWeak = weak;
    g_playerOne     = playerOneController;
    g_playerTwo     = playerTwoController;
    return true;
}

void Unbind() noexcept
{
    g_playerOne     = nullptr;
    g_playerTwo     = nullptr;
    g_playerTwoWeak = 0;
}

namespace {

/// FWeakObjectPtr::operator= then Get, inside a structured-exception guard (MSVC allows __try
/// only in a function with no objects to unwind).
bool AssignAndCheckGuarded(std::uint64_t* weak, void* object) noexcept
{
    __try
    {
        g_weakAssign(weak, object);
        return g_weakGet(weak) == object;
    }
    __except (GetExceptionCode() == EXCEPTION_STACK_OVERFLOW ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

}  // namespace

std::uint64_t WeakValueOf(void* object) noexcept
{
    if (!Installed() || object == nullptr)
    {
        return 0;
    }
    std::uint64_t weak = 0;
    return AssignAndCheckGuarded(&weak, object) ? weak : 0;
}

bool Is(const void* controller) noexcept
{
    return controller != nullptr && Current() == controller;
}

void* Current() noexcept
{
    const std::uint64_t* field = Installed() ? PrimaryField() : nullptr;
    return field == nullptr ? nullptr : g_weakGet(field);
}

Scope::Scope() noexcept
{
    if (g_playerTwo == nullptr)
    {
        return;  // refusal_ stays NotBound
    }
    std::uint64_t* field = PrimaryField();
    if (field == nullptr)
    {
        refusal_ = Refusal::NoPartyManager;
        return;
    }
    if (g_weakGet(&g_playerTwoWeak) != g_playerTwo)
    {
        refusal_ = Refusal::PlayerTwoStale;
        return;
    }
    const std::uint64_t current = std::atomic_ref<std::uint64_t>(*field).load();
    if (current != g_playerTwoWeak && g_weakGet(field) != g_playerOne)
    {
        refusal_ = Refusal::NotPlayerOne;
        return;
    }
    switch (window_.Open(field, current, g_playerTwoWeak))
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
        generation::Fault(L"the party manager's primary controller changed while it named player 2");
    }
}

const wchar_t* Describe(Scope::Refusal refusal) noexcept
{
    switch (refusal)
    {
        case Scope::Refusal::None: return L"held";
        case Scope::Refusal::NotBound: return L"not bound to player 2";
        case Scope::Refusal::NoPartyManager: return L"no party manager";
        case Scope::Refusal::PlayerTwoStale: return L"player 2's controller is gone";
        case Scope::Refusal::NotPlayerOne: return L"the primary controller is not player 1's";
        case Scope::Refusal::Changed: return L"the primary controller changed at the swap";
        case Scope::Refusal::Misaligned: return L"the primary-controller field is misaligned";
    }
    return L"unknown";
}

}  // namespace lss::player_two::primary_controller
