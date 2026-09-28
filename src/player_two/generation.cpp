#include "player_two/generation.h"

#include <atomic>

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

}  // namespace

void Begin(void* playerOneController, void* playerTwoController) noexcept
{
    g_active.store(false);  // wrappers see no half-written generation
    g_playerOne.store(playerOneController);
    g_playerTwo.store(playerTwoController);
    g_member.store(nullptr);
    g_fault.store(nullptr);
    g_active.store(playerOneController != nullptr && playerTwoController != nullptr);
}

void SetMember(void* member) noexcept
{
    g_member.store(member);
}

void Withdraw() noexcept
{
    g_active.store(false);
}

void Fault(const wchar_t* reason) noexcept
{
    g_active.store(false);
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

}  // namespace lss::player_two::generation
