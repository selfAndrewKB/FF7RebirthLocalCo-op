#include "control/command_channel.h"

#include "platform/log.h"

#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::control {

namespace {

namespace fs = std::filesystem;
using platform::Log;

/// How often an idle channel looks in its inbox (about four times a second at 60 ticks per second).
constexpr std::uint64_t kScanTicks = 15;

/// The folder the loaded DLL sits in (Mods/LocalSplitScreen/dlls), or empty.
fs::path DllFolder()
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&DllFolder), &module))
    {
        return {};
    }
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
            return fs::path(path).parent_path();
        }
        path.resize(path.size() * 2);
    }
}

std::wstring FromUtf8(const std::string& bytes)
{
    std::string_view text(bytes);
    if (text.starts_with("\xEF\xBB\xBF"))
    {
        text.remove_prefix(3);
    }
    if (text.empty())
    {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string ToUtf8(const std::wstring& wide)
{
    if (wide.empty())
    {
        return {};
    }
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), bytes.data(), length, nullptr, nullptr);
    return bytes;
}

/// The inbox's first script by file name, or empty.
std::optional<fs::path> FirstScript(const fs::path& inbox)
{
    std::error_code         error;
    std::optional<fs::path> first;
    for (fs::directory_iterator it(inbox, error), end; !error && it != end; it.increment(error))
    {
        const auto& path = it->path();
        if (it->is_regular_file(error) && path.extension() == L".txt" && (!first || path.filename() < first->filename()))
        {
            first = path;
        }
    }
    return first;
}

std::vector<std::wstring> Lines(const std::wstring& text)
{
    std::vector<std::wstring> lines;
    std::size_t               start = 0;
    while (start <= text.size())
    {
        const auto end = text.find(L'\n', start);
        lines.push_back(text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        if (end == std::wstring::npos)
        {
            break;
        }
        start = end + 1;
    }
    return lines;
}

std::wstring ResultText(const LineResult& result)
{
    const std::wstring verdict = result.outcome.ok ? L"ok" : L"FAILED";
    const std::wstring detail  = result.outcome.detail.empty() ? L"" : L" (" + result.outcome.detail + L")";
    return std::format(L"tick {} line {}: {} -> {}{}", result.tick, result.line, result.command, verdict, detail);
}

}  // namespace

bool CommandChannel::Open() noexcept
{
    try
    {
        const fs::path dlls = DllFolder();
        if (dlls.empty())
        {
            Log(L"control: the mod's own folder cannot be found; the command channel is closed");
            return false;
        }
        const fs::path root = dlls.parent_path() / L"control";
        inbox_              = root / L"inbox";
        done_               = root / L"done";
        std::error_code error;
        fs::create_directories(inbox_, error);
        fs::create_directories(done_, error);
        if (!fs::is_directory(inbox_, error) || !fs::is_directory(done_, error))
        {
            Log(L"control: could not create {}; the command channel is closed", root.wstring());
            return false;
        }
        open_ = true;
        Log(L"control: command channel open; scripts go in {}", inbox_.wstring());
        return true;
    }
    catch (...)
    {
        Log(L"control: the command channel could not be opened");
        return false;
    }
}

void CommandChannel::Tick(Target& target) noexcept
{
    if (!open_)
    {
        return;
    }
    ++tick_;
    try
    {
        if (!running_)
        {
            if (tick_ % kScanTicks == 0)
            {
                StartNext();
            }
            if (!running_)
            {
                return;
            }
        }
        Report(running_->runner.Step(target, tick_));
        if (running_->runner.Finished())
        {
            End();
        }
    }
    catch (...)
    {
        Log(L"control: an exception stopped the command channel");
        if (running_)
        {
            running_->runner.Abort(target);
            running_.reset();
        }
        open_ = false;
    }
}

void CommandChannel::StartNext()
{
    const auto script = FirstScript(inbox_);
    if (!script)
    {
        return;
    }
    std::string bytes;
    {
        std::ifstream file(*script, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    // The script leaves the inbox before anything runs, so it can never run twice.
    std::error_code error;
    const fs::path  taken   = done_ / script->filename();
    const fs::path  results = done_ / (script->stem().wstring() + L".result.txt");
    fs::remove(taken, error);
    fs::remove(results, error);
    fs::rename(*script, taken, error);
    const std::wstring name = script->filename().wstring();
    if (error)
    {
        Log(L"control: could not move {} out of the inbox ({}); it is skipped until it can be", name,
            FromUtf8(error.message()));
        return;
    }

    if (bytes.starts_with("\xFF\xFE") || bytes.starts_with("\xFE\xFF"))
    {
        Append(results, L"the script is UTF-16; save it as UTF-8");
        Append(results, L"finished: rejected");
        Log(L"control: {} rejected (UTF-16); nothing ran", name);
        return;
    }

    std::vector<ScriptLine> lines;
    bool                    rejected = false;
    std::uint32_t           number   = 0;
    for (const auto& text : Lines(FromUtf8(bytes)))
    {
        ++number;
        auto parsed = ParseLine(text);
        if (parsed.command)
        {
            lines.push_back({number, std::move(*parsed.command)});
        }
        else if (!parsed.error.empty())
        {
            rejected = true;
            Append(results, std::format(L"line {}: {}", number, parsed.error));
        }
    }
    if (rejected)
    {
        Append(results, L"finished: rejected");
        Log(L"control: {} rejected (see {}); nothing ran", name, results.filename().wstring());
        return;
    }
    Log(L"control: running {} ({} command(s))", name, lines.size());
    running_.emplace(Running{Runner(std::move(lines)), results, name});
}

void CommandChannel::Report(const std::vector<LineResult>& results)
{
    for (const auto& result : results)
    {
        const auto text = ResultText(result);
        Append(running_->results, text);
        Log(L"control {}: {}", running_->name, text);
    }
}

void CommandChannel::End()
{
    const bool failed = running_->runner.Failed();
    Append(running_->results, failed ? L"finished: failed" : L"finished: ok");
    Log(L"control: {} finished {}", running_->name, failed ? L"with a failure" : L"ok");
    running_.reset();
}

void CommandChannel::Append(const fs::path& file, const std::wstring& line) noexcept
{
    try
    {
        std::ofstream out(file, std::ios::binary | std::ios::app);
        out << ToUtf8(line) << "\r\n";
    }
    catch (...)
    {
        Log(L"control: could not write to {}", file.filename().wstring());
    }
}

}  // namespace lss::control
