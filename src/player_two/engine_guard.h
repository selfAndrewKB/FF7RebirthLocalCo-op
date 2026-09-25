#pragma once

// The exception filter for engine calls made inside a structured-exception guard.
//
// A player-2 stage that calls into the engine in a state the engine never produces itself guards
// the call with __try/__except, so a fault is reported and the stage stops, instead of the game
// ending. MSVC allows __try only in functions without objects that need unwinding, so each guard is
// a small function of its own. A stack overflow is passed on: the guard frame cannot recover it.

#include <Windows.h>

namespace lss::player_two {

inline int EngineGuardFilter(unsigned long code) noexcept
{
    return code == EXCEPTION_STACK_OVERFLOW ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace lss::player_two
