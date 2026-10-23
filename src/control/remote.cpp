#include "control/remote.h"

#include "platform/command_registry.h"
#include "platform/log.h"

#include <string>
#include <string_view>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::control {

namespace {

using platform::Level;
using platform::Log;

std::wstring FromUtf8(std::string_view text)
{
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

/// Replies about actions are worth the log at Info; queries and their answers are Debug.
Level ReplyLevel(const Reply& reply)
{
    switch (reply.kind)
    {
    case ReplyKind::Value:
    case ReplyKind::Help:
    case ReplyKind::Note: return Level::Debug;
    case ReplyKind::Ok:
        return reply.command.starts_with(L"get ") || reply.command == L"status" || reply.command.starts_with(L"help")
                   ? Level::Debug
                   : Level::Info;
    default: return Level::Info;
    }
}

}  // namespace

bool Remote::Open(Holds& holds) noexcept
{
    try
    {
        for (const auto& name : Global().Rejected())
        {
            platform::LogWarning(L"control: the command '{}' was refused by the registry (a taken name or a malformed "
                                 L"entry)",
                                 name);
        }
        runner_.emplace(Global(), holds);
        const bool inbox = inbox_.Open(*runner_);
        std::wstring error;
        if (server_.Open(kPort, error))
        {
            Log(L"control: console listening on 127.0.0.1:{} ({} commands; tools/remote.py shell)", kPort,
                Global().Entries().size());
        }
        else
        {
            platform::LogWarning(L"control: the console is not listening: {}", error);
        }
        open_ = inbox || server_.IsOpen();
        return open_;
    }
    catch (...)
    {
        platform::LogWarning(L"control: the console could not be opened");
        return false;
    }
}

void Remote::Tick() noexcept
{
    if (!open_)
    {
        return;
    }
    ++tick_;
    try
    {
        inbox_.BeforeRun(tick_);
        for (auto& event : server_.Drain())
        {
            if (event.closed)
            {
                runner_->ClientClosed(event.client);
                platform::LogDebug(L"control: console client {} left", event.client);
            }
            else
            {
                const std::wstring line = FromUtf8(event.line);
                platform::LogDebug(L"control: console client {}: {}", event.client, line);
                runner_->Submit(event.client, line);
            }
        }
        runner_->Tick(tick_);
        for (const auto& reply : runner_->TakeReplies())
        {
            if (CommandChannel::Owns(reply.client))
            {
                inbox_.Take(reply);
                continue;
            }
            const std::wstring text = ConsoleText(reply);
            platform::LogAt(ReplyLevel(reply), L"control: to console client {}: {}", reply.client, text);
            server_.Send(reply.client, ToUtf8(text));
        }
        inbox_.AfterRun();
    }
    catch (...)
    {
        platform::LogWarning(L"control: an exception stopped the console");
        inbox_.Abandon();
        Close();
    }
}

void Remote::Close() noexcept
{
    if (!open_)
    {
        return;
    }
    open_ = false;
    try
    {
        runner_->Shutdown();  // every hold released
        for (const auto& reply : runner_->TakeReplies())
        {
            if (!CommandChannel::Owns(reply.client))
            {
                server_.Send(reply.client, ToUtf8(ConsoleText(reply)));
            }
        }
    }
    catch (...)
    {
    }
    // A running script gets no line here: at unload it is the one whose `quit` closed the game, and its sender
    // reads the game's exit, as before the console.
    server_.Close();
    Log(L"control: console closed");
}

}  // namespace lss::control
