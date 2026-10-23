#include "control/command_channel.h"

#include "platform/log.h"
#include "platform/module_folder.h"

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
using platform::DllFolder;
using platform::Log;

/// How often an idle channel looks in its inbox (about four times a second at 60 ticks per second).
constexpr std::uint64_t kScanTicks = 15;

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

/// One reply as the result file and the log write it, in the channel's line format of before the registry, so the
/// regression's expectations (`layout side-by-side -> ok \(side by side\)`) read the same.
std::wstring ResultText(std::uint64_t tick, const Reply& reply)
{
    const std::wstring detail = reply.detail.empty() ? L"" : L" (" + reply.detail + L")";
    switch (reply.kind)
    {
    case ReplyKind::Ok: return std::format(L"tick {} line {}: {} -> ok{}", tick, reply.line, reply.command, detail);
    case ReplyKind::Failed:
        return std::format(L"tick {} line {}: {} -> FAILED{}", tick, reply.line, reply.command, detail);
    case ReplyKind::Dropped:
        return std::format(L"tick {} line {}: {} -> dropped{}", tick, reply.line, reply.command, detail);
    case ReplyKind::Rejected:
        return std::format(L"tick {} line {}: {} -> rejected{}", tick, reply.line, reply.command, detail);
    case ReplyKind::Value: return std::format(L"tick {}: value {} {}", tick, reply.command, reply.detail);
    case ReplyKind::Help: return std::format(L"tick {}: help {}{}", tick, reply.command, reply.detail);
    case ReplyKind::Note: return std::format(L"tick {} line {}: note {} -> ok", tick, reply.line, reply.detail);
    }
    return L"?";
}

}  // namespace

bool CommandChannel::Open(Runner& runner) noexcept
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
        if (!RetireLeftovers())
        {
            return false;
        }
        runner_ = &runner;
        open_   = true;
        Log(L"control: command channel open; scripts go in {}", inbox_.wstring());  // the harness waits on it
        return true;
    }
    catch (...)
    {
        Log(L"control: the command channel could not be opened");
        return false;
    }
}

void CommandChannel::BeforeRun(std::uint64_t tick)
{
    if (!open_)
    {
        return;
    }
    tick_ = tick;
    if (!running_ && tick_ % kScanTicks == 0)
    {
        StartNext();  // its queries may answer at once
    }
}

void CommandChannel::AfterRun()
{
    if (open_ && running_ && !runner_->Busy(running_->client))
    {
        End();
    }
}

void CommandChannel::Abandon() noexcept
{
    if (running_)
    {
        // The sender waits for a `finished:` line; without one it would wait out its whole timeout.
        Append(running_->results, L"an exception stopped the console");
        Append(running_->results, L"finished: failed");
        running_.reset();
    }
    open_ = false;
}

bool CommandChannel::RetireLeftovers()
{
    std::vector<fs::path> leftovers;
    std::error_code       error;
    for (fs::directory_iterator it(inbox_, error), end; !error && it != end; it.increment(error))
    {
        if (it->is_regular_file(error) && it->path().extension() == L".txt")
        {
            leftovers.push_back(it->path());
        }
    }
    for (const auto& script : leftovers)
    {
        const fs::path taken   = done_ / script.filename();
        const fs::path results = done_ / (script.stem().wstring() + L".result.txt");
        fs::remove(taken, error);
        fs::remove(results, error);
        fs::rename(script, taken, error);
        if (error)
        {
            fs::remove(script, error);
        }
        if (error || fs::exists(script, error))
        {
            Log(L"control: {} was left in the inbox before this launch and cannot be moved or removed; the command "
                L"channel stays closed so it never runs",
                script.filename().wstring());
            return false;
        }
        Append(results, L"left in the inbox before this launch (its sender timed out or the game closed first)");
        Append(results, L"finished: skipped");
        Log(L"control: {} was left in the inbox before this launch; retired unrun", script.filename().wstring());
    }
    return true;
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

    // Every line is checked before any runs: a script with one bad line is rejected whole.
    const std::vector<std::wstring> lines    = Lines(FromUtf8(bytes));
    bool                            rejected = false;
    std::size_t                     commands = 0;
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        const auto parsed = ParseLine(Global(), lines[i]);
        if (parsed.command && parsed.command->verb == Verb::Abort)
        {
            // A script is submitted whole, so its abort would drop every line before it and report success; a
            // script stops at its first failure instead. Abort is the console's, typed while something runs.
            rejected = true;
            Append(results, std::format(L"line {}: abort is for the console; a script stops at its first failure",
                                        i + 1));
        }
        else if (parsed.command)
        {
            ++commands;
        }
        else if (!parsed.error.empty())
        {
            rejected = true;
            Append(results, std::format(L"line {}: {}", i + 1, parsed.error));
        }
    }
    if (rejected)
    {
        Append(results, L"finished: rejected");
        Log(L"control: {} rejected (see {}); nothing ran", name, results.filename().wstring());
        return;
    }
    Log(L"control: running {} ({} command(s))", name, commands);
    running_.emplace(Running{++scripts_, results, name});
    for (const auto& line : lines)
    {
        runner_->Submit(running_->client, line);
    }
}

void CommandChannel::Take(const Reply& reply)
{
    if (!running_ || reply.client != running_->client)
    {
        return;  // one script runs at a time: a reply to an ended one has nowhere to go
    }
    if (reply.kind == ReplyKind::Note)
    {
        Log(L"control note: {}", reply.detail);
    }
    // A dropped line never ran, so the script did not do what it says, whatever dropped it.
    running_->failed = running_->failed || reply.kind == ReplyKind::Failed || reply.kind == ReplyKind::Rejected ||
                       reply.kind == ReplyKind::Dropped;
    const auto text = ResultText(tick_, reply);
    Append(running_->results, text);
    Log(L"control {}: {}", running_->name, text);
}

void CommandChannel::End()
{
    const bool failed = running_->failed;
    runner_->ClientClosed(running_->client);  // nothing is queued or held by now
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
