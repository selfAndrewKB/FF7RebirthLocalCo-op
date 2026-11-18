#include "platform/game_folder_tools.h"

#include "platform/log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace lss::platform {

namespace {

// The DLLs the game ships beside its exe, and UE4SS's proxy.
constexpr std::array<std::wstring_view, 4> kKnown{L"dstorage.dll", L"dstoragecore.dll", L"turbojpeg.dll",
                                                  L"dwmapi.dll"};

std::wstring Lower(std::wstring text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}

/// A file's version information, read once.
class VersionInfo
{
public:
    explicit VersionInfo(const std::filesystem::path& file)
    {
        DWORD       ignored = 0;
        const DWORD size    = GetFileVersionInfoSizeW(file.c_str(), &ignored);
        if (size == 0)
        {
            return;
        }
        data_.resize(size);
        Translation* translations   = nullptr;
        UINT         translationLen = 0;
        if (!GetFileVersionInfoW(file.c_str(), 0, size, data_.data()) ||
            !VerQueryValueW(data_.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations),
                            &translationLen) ||
            translationLen < sizeof(Translation))
        {
            data_.clear();
            return;
        }
        language_ = translations[0];
    }

    /// One string (its first language), or empty.
    std::wstring String(const wchar_t* name) const
    {
        if (data_.empty())
        {
            return {};
        }
        const std::wstring key = std::format(L"\\StringFileInfo\\{:04x}{:04x}\\{}", language_.language,
                                             language_.codePage, name);
        wchar_t* value    = nullptr;
        UINT     valueLen = 0;
        if (!VerQueryValueW(const_cast<std::byte*>(data_.data()), key.c_str(), reinterpret_cast<void**>(&value), &valueLen) ||
            value == nullptr || valueLen == 0)
        {
            return {};
        }
        std::wstring text(value, wcsnlen(value, valueLen));
        while (!text.empty() && text.back() == L' ')
        {
            text.pop_back();
        }
        return text;
    }

private:
    struct Translation
    {
        WORD language = 0;
        WORD codePage = 0;
    };
    std::vector<std::byte> data_;
    Translation            language_;
};

/// "name (description, maker)" for a DLL, the parts it has.
std::wstring Describe(const std::filesystem::path& file)
{
    const VersionInfo  info(file);
    const std::wstring description = info.String(L"FileDescription");
    const std::wstring maker       = info.String(L"CompanyName");
    std::wstring       about;
    if (!description.empty())
    {
        about = description;
    }
    if (!maker.empty())
    {
        about += (about.empty() ? L"" : L", ") + maker;
    }
    return file.filename().wstring() + (about.empty() ? std::wstring{} : L" (" + about + L")");
}

}  // namespace

void LogGameFolderTools() noexcept
{
    try
    {
        wchar_t     exe[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
        {
            Log(L"other tools: the game's folder could not be found");
            return;
        }
        const std::filesystem::path folder = std::filesystem::path(exe).parent_path();
        std::vector<std::wstring>   dlls;
        std::vector<std::wstring>   asis;
        // Each folder walked with error codes, so one unreadable entry ends that folder's walk, not the line.
        const auto walk = [](const std::filesystem::path& dir, auto&& visit) {
            std::error_code error;
            for (std::filesystem::directory_iterator it(dir, error), end; !error && it != end; it.increment(error))
            {
                std::error_code entryError;
                if (it->is_regular_file(entryError))
                {
                    visit(it->path());
                }
            }
        };
        walk(folder, [&](const std::filesystem::path& file) {
            const std::wstring name = Lower(file.filename().wstring());
            const std::wstring ext  = Lower(file.extension().wstring());
            if (ext == L".dll" && std::find(kKnown.begin(), kKnown.end(), name) == kKnown.end())
            {
                dlls.push_back(Describe(file));
            }
            else if (ext == L".asi")
            {
                asis.push_back(file.filename().wstring());
            }
        });
        for (const wchar_t* sub : {L"scripts", L"plugins"})
        {
            walk(folder / sub, [&](const std::filesystem::path& file) {
                if (Lower(file.extension().wstring()) == L".asi")
                {
                    asis.push_back(std::wstring(sub) + L"\\" + file.filename().wstring());
                }
            });
        }
        const auto join = [](const std::vector<std::wstring>& items) {
            std::wstring text;
            for (const auto& item : items)
            {
                text += (text.empty() ? L"" : L"; ") + item;
            }
            return text.empty() ? std::wstring(L"none") : text;
        };
        Log(L"other tools beside the game: DLLs {}; ASI plugins {}", join(dlls), join(asis));
    }
    catch (...)
    {
        Log(L"other tools: the game's folder could not be read");
    }
}

}  // namespace lss::platform
