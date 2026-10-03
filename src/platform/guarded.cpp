#include "platform/guarded.h"

#include <iterator>

#include <cstdio>
#include <cstring>
#include <cwchar>

namespace lss::platform {

namespace {

constexpr std::uintptr_t kIdaImageBase = 0x140000000;

thread_local Fault t_lastFault;

}  // namespace

int GuardFilterRecording(const EXCEPTION_POINTERS* info) noexcept
{
    const EXCEPTION_RECORD* record = info != nullptr ? info->ExceptionRecord : nullptr;
    if (record == nullptr)
    {
        return EXCEPTION_EXECUTE_HANDLER;
    }
    Fault fault;
    fault.code = record->ExceptionCode;
    fault.at   = reinterpret_cast<std::uintptr_t>(record->ExceptionAddress);
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
    {
        fault.access = record->ExceptionInformation[0];
        fault.target = record->ExceptionInformation[1];
    }
    t_lastFault = fault;
    return GuardFilter(record->ExceptionCode);
}

Fault LastFault() noexcept
{
    return t_lastFault;
}

FaultText DescribeFault(const Fault& fault) noexcept
{
    FaultText out;
    if (fault.code == 0)
    {
        std::swprintf(out.text, std::size(out.text), L"no exception recorded");
        return out;
    }
    wchar_t where[96] = L"an unknown module";
    HMODULE module    = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(fault.at), &module) &&
        module != nullptr)
    {
        const auto offset = fault.at - reinterpret_cast<std::uintptr_t>(module);
        if (module == GetModuleHandleW(nullptr))
        {
            std::swprintf(where, std::size(where), L"the game 0x%llX (IDA)",
                          static_cast<unsigned long long>(kIdaImageBase + offset));
        }
        else
        {
            wchar_t path[MAX_PATH] = {};
            GetModuleFileNameW(module, path, MAX_PATH);
            const wchar_t* slash = std::wcsrchr(path, L'\\');
            std::swprintf(where, std::size(where), L"%ls+0x%llX", slash != nullptr ? slash + 1 : path,
                          static_cast<unsigned long long>(offset));
        }
    }
    if (fault.code == EXCEPTION_ACCESS_VIOLATION)
    {
        const wchar_t* kind = fault.access == 1 ? L"writing" : fault.access == 8 ? L"executing" : L"reading";
        std::swprintf(out.text, std::size(out.text), L"exception 0x%08lX at %ls %ls 0x%llX", fault.code, where, kind,
                      static_cast<unsigned long long>(fault.target));
    }
    else
    {
        std::swprintf(out.text, std::size(out.text), L"exception 0x%08lX at %ls", fault.code, where);
    }
    return out;
}

bool CopyGuarded(void* out, const void* from, std::size_t length) noexcept
{
    __try
    {
        std::memcpy(out, from, length);
        return true;
    }
    __except (GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

bool WriteGuarded(void* to, const void* from, std::size_t length) noexcept
{
    __try
    {
        std::memcpy(to, from, length);
        return true;
    }
    __except (GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

}  // namespace lss::platform
