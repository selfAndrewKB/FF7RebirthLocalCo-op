#include "player_two/generation.h"

#include "player_two/engine_facts.h"
#include "player_two/object_fields.h"
#include "player_two/weak_object.h"

#include <atomic>
#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::player_two::generation {

namespace {

std::atomic<DWORD> g_gameThread{0};

std::atomic<bool>           g_active{false};
std::atomic<void*>          g_playerOne{nullptr};
std::atomic<void*>          g_playerTwo{nullptr};
std::atomic<void*>          g_member{nullptr};
std::atomic<const wchar_t*> g_fault{nullptr};

// The tick's identities (BeginTick), for the wrappers on any thread.
std::atomic<std::uint64_t> g_memberWeak{0};
std::atomic<std::uint64_t> g_playerOnePawnWeak{0};
std::atomic<const void*>   g_chocobo{nullptr};
std::atomic<std::uint64_t> g_chocoboWeak{0};

void ClearIdentities() noexcept
{
    g_memberWeak.store(0);
    g_playerOnePawnWeak.store(0);
    g_chocobo.store(nullptr);
    g_chocoboWeak.store(0);
}

}  // namespace

void Begin(void* playerOneController, void* playerTwoController) noexcept
{
    g_active.store(false);  // wrappers see no half-written generation
    g_playerOne.store(playerOneController);
    g_playerTwo.store(playerTwoController);
    g_member.store(nullptr);
    g_fault.store(nullptr);
    ClearIdentities();
    g_active.store(playerOneController != nullptr && playerTwoController != nullptr);
}

void SetMember(void* member) noexcept
{
    g_member.store(member);
    g_memberWeak.store(member != nullptr && g_active.load() ? weak_object::WeakOf(member) : 0);
}

void Withdraw() noexcept
{
    g_active.store(false);
    ClearIdentities();
}

void End() noexcept
{
    g_active.store(false);
    ClearIdentities();
    g_playerTwo.store(nullptr);
    g_member.store(nullptr);
}

void Fault(const wchar_t* reason) noexcept
{
    g_active.store(false);
    ClearIdentities();
    const wchar_t* none = nullptr;
    g_fault.compare_exchange_strong(none, reason);
}

const wchar_t* FaultReason() noexcept
{
    return g_fault.load();
}

void SetGameThread() noexcept
{
    g_gameThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
}

bool OnGameThread() noexcept
{
    return GetCurrentThreadId() == g_gameThread.load(std::memory_order_relaxed);
}

bool GameThreadKnown() noexcept
{
    return g_gameThread.load(std::memory_order_relaxed) != 0;
}

bool Active() noexcept
{
    return g_active.load();
}

void* PlayerOneController() noexcept
{
    return g_playerOne.load();
}

void* PlayerTwoController() noexcept
{
    return g_playerTwo.load();
}

void* Member() noexcept
{
    return g_member.load();
}

void BeginTick(const void* drivenChocobo) noexcept
{
    if (!g_active.load())
    {
        ClearIdentities();
        return;
    }
    g_memberWeak.store(weak_object::WeakOf(g_member.load()));
    g_playerOnePawnWeak.store(weak_object::WeakOf(ReadPointer(g_playerOne.load(), facts::kControllerPawn)));
    g_chocobo.store(drivenChocobo);
    g_chocoboWeak.store(weak_object::WeakOf(const_cast<void*>(drivenChocobo)));
}

std::uint64_t MemberWeak() noexcept
{
    return g_memberWeak.load();
}

std::uint64_t PlayerOnePawnWeak() noexcept
{
    return g_playerOnePawnWeak.load();
}

const void* Chocobo() noexcept
{
    return g_chocobo.load();
}

std::uint64_t ChocoboWeak() noexcept
{
    return g_chocoboWeak.load();
}

}  // namespace lss::player_two::generation
