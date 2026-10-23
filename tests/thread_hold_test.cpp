// Host-side tests for player_two/thread_hold, run without the game.
//
// A game call with this thread's answer held: the slot holds the value during the call and the outer value after
// it, also when a structured exception unwinds through the call and when holds nest; the exchange form puts the
// previous answer back through the exchange function; the enter/leave form puts a module's own state back.

#include "player_two/scopes/thread_hold.h"

#include "expect.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstdint>
#include <cstdio>

namespace {

using namespace lss::player_two::thread_hold;

thread_local const void* t_slot = nullptr;
thread_local int         t_exchanged = -1;

int g_seenInside = 0;
const void* g_seenSlot = nullptr;

const int kA = 1, kB = 2;

std::uint64_t ReadsTheSlot(std::uint64_t a, std::uint64_t b) noexcept
{
    g_seenSlot = t_slot;
    return a + b;
}

void ReadsTheSlotVoid(int value) noexcept
{
    g_seenSlot    = t_slot;
    g_seenInside = value;
}

std::uint64_t Nested(std::uint64_t a, std::uint64_t b) noexcept
{
    const void* const atEntry = t_slot;
    const auto        inner   = Holding(t_slot, static_cast<const void*>(&kB), &ReadsTheSlot, a, b);
    return inner + (t_slot == atEntry ? 100 : 0);
}

std::uint64_t Raises(std::uint64_t, std::uint64_t) noexcept
{
    RaiseException(0xE0000001, 0, 0, nullptr);
    return 0;
}

int Exchange(int value) noexcept
{
    const int previous = t_exchanged;
    t_exchanged        = value;
    return previous;
}

int ReadsExchanged(int add) noexcept
{
    return t_exchanged + add;
}

struct Context
{
    int type;
};
struct Held
{
    int type;
};
thread_local int t_module = 0;

Held Enter(const Context& context) noexcept
{
    const Held outer{t_module};
    t_module = context.type;
    return outer;
}
void Leave(const Held& outer) noexcept
{
    t_module = outer.type;
}
int ReadsModule(int add) noexcept
{
    return t_module + add;
}

/// Runs `call` inside a structured-exception guard; true if an exception was raised.
template <class Call>
bool Raised(Call call) noexcept
{
    __try
    {
        call();
        return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return true;
    }
}

void TestSlot()
{
    std::puts("thread hold: the slot holds the value for the call and the outer value after it; the result passes through");
    t_slot = &kA;
    EXPECT(Holding(t_slot, static_cast<const void*>(&kB), &ReadsTheSlot, 3, 4) == 7);
    EXPECT(g_seenSlot == &kB);
    EXPECT(t_slot == &kA);
    Holding(t_slot, static_cast<const void*>(nullptr), &ReadsTheSlotVoid, 9);
    EXPECT(g_seenSlot == nullptr && g_seenInside == 9);
    EXPECT(t_slot == &kA);
}

void TestNesting()
{
    std::puts("thread hold: an inner hold sees its own value and gives the outer hold's back");
    t_slot = nullptr;
    EXPECT(Holding(t_slot, static_cast<const void*>(&kA), &Nested, 1, 2) == 103);
    EXPECT(g_seenSlot == &kB);
    EXPECT(t_slot == nullptr);
}

void TestException()
{
    std::puts("thread hold: a structured exception inside the call unwinds through the hold; the outer value is back");
    t_slot = &kA;
    EXPECT(Raised([] { Holding(t_slot, static_cast<const void*>(&kB), &Raises, 0, 0); }));
    EXPECT(t_slot == &kA);
    t_exchanged = 5;
    EXPECT(Raised([] { HoldingExchange(&Exchange, 7, &Raises, 0, 0); }));
    EXPECT(t_exchanged == 5);
}

void TestExchange()
{
    std::puts("thread hold: the exchange form sets the answer for the call and exchanges the previous one back");
    t_exchanged = 5;
    EXPECT(HoldingExchange(&Exchange, 7, &ReadsExchanged, 10) == 17);
    EXPECT(t_exchanged == 5);
}

void TestEnterLeave()
{
    std::puts("thread hold: the enter/leave form puts a module's own state back after the call");
    t_module = 1;
    EXPECT(HoldingBy(&Enter, &Leave, Context{4}, &ReadsModule, 10) == 14);
    EXPECT(t_module == 1);
}

}  // namespace

int main()
{
    TestSlot();
    TestNesting();
    TestException();
    TestExchange();
    TestEnterLeave();

    return lss::test::Report();
}
