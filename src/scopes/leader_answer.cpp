#include "scopes/leader_answer.h"

#include "facts/party.h"
#include "input/climb_context.h"
#include "platform/every.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/generation.h"
#include "players/object_fields.h"

#include <atomic>
#include <cstdint>

namespace lss::scopes::leader_answer {

namespace {

using LeaderTestFn = std::uint8_t (*)(const void* component);
using PlayerTypeFn = std::uint8_t (*)(const void* component);

// Set once at install.
platform::VTableEntry g_slot;
LeaderTestFn          g_engineTest = nullptr;

// Source 4 and 5: written on the game thread (Scope); the test is also called on other threads, which read nothing of
// them (Decide asks for the game thread).
std::atomic<int>         g_gameType{kNoType};
std::atomic<const void*> g_component{nullptr};
std::atomic<unsigned>    g_answers{0};
// Sources 3 and 1, this thread's.
thread_local int t_threadType = kNoType;
thread_local int t_onlyType   = kNoType;

// The slot holds LeaderTest (Replaced); any thread reads it.
std::atomic<bool> g_replaced{false};

int PlayerTypeOfComponent(const void* component) noexcept
{
    const auto vtable = reinterpret_cast<const void*>(VTableOf(component));
    const auto fn     = vtable == nullptr ? nullptr : ReadField<PlayerTypeFn>(vtable, facts::kPlayerTypeSlotOffset);
    return fn == nullptr ? kNoType : fn(component);  // an unreadable slot reads null
}

/// The slot's test: Decide over the five sources, else the engine's own test.
std::uint8_t LeaderTest(const void* component)
{
    const int   type  = g_gameType.load(std::memory_order_relaxed);
    const void* extra = g_component.load(std::memory_order_relaxed);
    // The climb's type is asked only past source 1 (its ask is counted by the climb context, a dev count).
    const int    climbType = t_onlyType != kNoType ? kNoType : input::climb_context::LeaderTypeHere();
    const Answer answer    = Decide(t_onlyType, climbType, t_threadType, type, extra != nullptr,
                                    extra != nullptr && component == extra, players::generation::OnGameThread(),
                                    [component] { return PlayerTypeOfComponent(component); });
    if (answer.counted)
    {
        g_answers.fetch_add(1, std::memory_order_relaxed);
    }
    switch (answer.verdict)
    {
        case Verdict::Yes: return 1;
        case Verdict::No: return 0;
        case Verdict::Engine: break;
    }
    return g_engineTest(component);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto leaderTest = platform::FindFunction(image, facts::kIsPartyLeader.target);
    if (!leaderTest)
    {
        return false;
    }
    g_engineTest        = reinterpret_cast<LeaderTestFn>(*leaderTest);
    const bool replaced = platform::ReplaceSlot(image, facts::kIsPartyLeader, reinterpret_cast<std::uintptr_t>(&LeaderTest),
                                                g_slot);
    g_replaced.store(replaced, std::memory_order_release);
    return replaced;
}

void Remove() noexcept
{
    if (g_slot.Restore())
    {
        g_replaced.store(false, std::memory_order_release);
    }
    else
    {
        platform::Log(L"could not restore the leader-test slot; the replacement stays in place");
    }
}

bool Replaced() noexcept
{
    return g_replaced.load(std::memory_order_acquire);
}

Scope::Scope(int type, const void* component) noexcept
    : savedType_(g_gameType.exchange(type, std::memory_order_relaxed)),
      savedComponent_(g_component.exchange(component, std::memory_order_relaxed))
{
}

Scope::~Scope()  // what was answered before (none outside a scope): nesting keeps the outer answer
{
    g_gameType.store(savedType_, std::memory_order_relaxed);
    g_component.store(savedComponent_, std::memory_order_relaxed);
}

int GameType() noexcept
{
    return g_gameType.load(std::memory_order_relaxed);
}

int ActingType() noexcept
{
    if (t_onlyType != kNoType)
    {
        return kNoType;
    }
    if (t_threadType != kNoType)
    {
        return t_threadType;
    }
    return players::generation::OnGameThread() ? g_gameType.load(std::memory_order_relaxed) : kNoType;
}

ThreadScope::ThreadScope(int type) noexcept : saved_(t_threadType)
{
    t_threadType = type;
}

ThreadScope::~ThreadScope()
{
    t_threadType = saved_;
}

int ExchangeThreadOnly(int type) noexcept
{
    const int previous = t_onlyType;
    t_onlyType         = type;
    return previous;
}

int ExchangeThread(int type) noexcept
{
    const int previous = t_threadType;
    t_threadType       = type;
    return previous;
}

int ComponentPlayerType(const void* component) noexcept
{
    return component == nullptr ? kNoType : PlayerTypeOfComponent(component);
}

unsigned TakeAnswers() noexcept
{
    return platform::Take(g_answers);
}

unsigned Answers() noexcept
{
    return g_answers.load(std::memory_order_relaxed);
}

void Uncount(unsigned answers) noexcept
{
    g_answers.fetch_sub(answers, std::memory_order_relaxed);
}

}  // namespace lss::scopes::leader_answer
