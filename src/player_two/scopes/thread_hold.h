#pragma once

// A game call made with this thread's answer held.
//
// Several of player 2's redirects answer a game read (the leader type, the UCPC test, a coin's pick)
// for the one call they wrap: the answer goes into a thread-local slot, or into one of
// player_two/input/member_movement.h's Exchange* functions, or a module's own set of slots, and must come out again
// on every way out of the call. These functions make that call: the outer value is put back in a
// __finally, which also runs when a structured exception unwinds through the call (destructors would
// not, under /EHsc), so a thread never keeps a stale answer. Wrappers nest: the outer value goes back.
// No object in these functions needs unwinding (the __try rule); the game function's result is passed
// back unchanged.

#include <type_traits>

namespace lss::player_two::thread_hold {

/// `fn(args...)` with `slot` (a thread-local) holding `value`, the outer value put back afterwards.
template <class T, class R, class... P>
R Holding(T& slot, std::type_identity_t<T> value, R (*fn)(P...), std::type_identity_t<P>... args) noexcept
{
    const T outer  = slot;
    R       result = R{};
    slot           = value;
    __try
    {
        result = fn(args...);
    }
    __finally
    {
        slot = outer;
    }
    return result;
}

template <class T, class... P>
void Holding(T& slot, std::type_identity_t<T> value, void (*fn)(P...), std::type_identity_t<P>... args) noexcept
{
    const T outer = slot;
    slot          = value;
    __try
    {
        fn(args...);
    }
    __finally
    {
        slot = outer;
    }
}

/// `fn(args...)` with `exchange(value)` in effect on this thread (an Exchange* function of
/// player_two/input/member_movement.h, which sets its answer and returns the previous one), the previous answer
/// exchanged back afterwards.
template <class R, class... P>
R HoldingExchange(int (*exchange)(int) noexcept, int value, R (*fn)(P...), std::type_identity_t<P>... args) noexcept
{
    const int outer  = exchange(value);
    R         result = R{};
    __try
    {
        result = fn(args...);
    }
    __finally
    {
        exchange(outer);
    }
    return result;
}

template <class... P>
void HoldingExchange(int (*exchange)(int) noexcept, int value, void (*fn)(P...), std::type_identity_t<P>... args) noexcept
{
    const int outer = exchange(value);
    __try
    {
        fn(args...);
    }
    __finally
    {
        exchange(outer);
    }
}

/// `fn(args...)` between a module's own `enter(context)`, which sets its slots and returns what they held, and
/// `leave(held)`, which puts that back. `Held` must need no unwinding (a struct of plain values).
template <class Held, class Context, class R, class... P>
R HoldingBy(Held (*enter)(const Context&) noexcept, void (*leave)(const Held&) noexcept, const Context& context,
            R (*fn)(P...), std::type_identity_t<P>... args) noexcept
{
    static_assert(std::is_trivially_destructible_v<Held>, "the held state must need no unwinding (the __try rule)");
    const Held outer  = enter(context);
    R          result = R{};
    __try
    {
        result = fn(args...);
    }
    __finally
    {
        leave(outer);
    }
    return result;
}

}  // namespace lss::player_two::thread_hold
