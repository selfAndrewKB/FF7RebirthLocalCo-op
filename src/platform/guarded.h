#pragma once

// Structured-exception guards for touching engine memory and calling engine code.
//
// Where the mod reads a field the engine might have freed, or calls the engine in a state it never
// produces itself, it guards the access with __try/__except, so a fault is reported and that feature
// stops instead of the game ending. MSVC allows __try only in functions without objects that need
// unwinding, so each guard is a small function of its own. A stack overflow is passed on: the guard
// frame cannot recover it.

#include <cstddef>
#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX  // this header is included beside UE4SS's, which use std::numeric_limits<>::max()
#endif
#include <Windows.h>

namespace lss::platform {

inline int GuardFilter(unsigned long code) noexcept
{
    return code == EXCEPTION_STACK_OVERFLOW ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER;
}

/// The last exception a CallGuarded on this thread caught.
struct Fault
{
    unsigned long  code   = 0;  // 0: none caught yet
    std::uintptr_t at     = 0;  // the faulting instruction
    std::uintptr_t access = 0;  // access violations: 0 read, 1 write, 8 execute
    std::uintptr_t target = 0;  // access violations: the address touched
};

/// GuardFilter that first records the exception for LastFault().
int GuardFilterRecording(const EXCEPTION_POINTERS* info) noexcept;

/// The last exception a CallGuarded on this thread caught (code 0 if none).
Fault LastFault() noexcept;

/// `fault` for the log: its code, the instruction as an IDA address when it is in the game's image (else its
/// module and offset) and, for an access violation, the address read, written or executed.
struct FaultText
{
    wchar_t text[192] = {};
};
FaultText DescribeFault(const Fault& fault) noexcept;

/// memcpy from memory that may not be readable. False if it faulted.
bool CopyGuarded(void* out, const void* from, std::size_t length) noexcept;

/// memcpy to memory that may not be writable. False if it faulted.
bool WriteGuarded(void* to, const void* from, std::size_t length) noexcept;

/// Calls an engine function, its result into `out`. False if it faulted.
template <class R, class... P, class... A>
bool CallGuarded(R (*fn)(P...), R& out, A... args) noexcept
{
    __try
    {
        out = fn(args...);
        return true;
    }
    __except (GuardFilterRecording(GetExceptionInformation()))
    {
        return false;
    }
}

/// Calls an engine function with no result. False if it faulted.
template <class... P, class... A>
bool CallGuarded(void (*fn)(P...), A... args) noexcept
{
    __try
    {
        fn(args...);
        return true;
    }
    __except (GuardFilterRecording(GetExceptionInformation()))
    {
        return false;
    }
}

}  // namespace lss::platform
