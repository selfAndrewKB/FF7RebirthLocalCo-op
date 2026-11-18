#include "platform/module_folder.h"

#include <string>
#include <system_error>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::platform {

namespace {

/// The loaded module that holds `address`, or null.
HMODULE ModuleAt(const void* address) noexcept
{
    HMODULE module = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              static_cast<LPCWSTR>(address), &module)
               ? module
               : nullptr;
}

/// The module's full path, or empty.
std::wstring PathOf(HMODULE module)
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0)
        {
            return {};
        }
        if (length < path.size())
        {
            path.resize(length);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

}  // namespace

std::filesystem::path DllFolder()
{
    const HMODULE module = ModuleAt(reinterpret_cast<const void*>(&DllFolder));
    const auto    path   = module != nullptr ? PathOf(module) : std::wstring{};
    return path.empty() ? std::filesystem::path{} : std::filesystem::path(path).parent_path();
}

std::filesystem::path ModFolder()
{
    const auto dlls = DllFolder();
    return dlls.empty() ? dlls : dlls.parent_path();
}

std::wstring ModuleNameAt(std::uintptr_t address) noexcept
{
    try
    {
        const HMODULE module = ModuleAt(reinterpret_cast<const void*>(address));
        return module != nullptr ? std::filesystem::path(PathOf(module)).filename().wstring() : std::wstring{};
    }
    catch (...)
    {
        return {};
    }
}

bool FlagFile(const wchar_t* name) noexcept
{
    std::error_code missing;
    return std::filesystem::exists(ModFolder() / name, missing);
}

bool WatchOnly() noexcept
{
    static const bool on = FlagFile(L"watch_only.txt");
    return on;
}

}  // namespace lss::platform
