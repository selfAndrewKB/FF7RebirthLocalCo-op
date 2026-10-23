#include "control/runner.h"

#include <algorithm>
#include <format>
#include <utility>

namespace lss::control {

namespace {

template <class T>
bool Compares(Compare compare, const T& left, const T& right)
{
    switch (compare)
    {
    case Compare::Equal: return left == right;
    case Compare::NotEqual: return left != right;
    case Compare::Less: return left < right;
    case Compare::Greater: return left > right;
    case Compare::LessOrEqual: return left <= right;
    case Compare::GreaterOrEqual: return left >= right;
    }
    return false;
}

/// A query answers from the registry and changes nothing in the game.
bool IsQuery(Verb verb) noexcept
{
    return verb == Verb::Get || verb == Verb::Status || verb == Verb::Help || verb == Verb::Note;
}

}  // namespace

std::wstring ConsoleText(const Reply& reply)
{
    const std::wstring tail = reply.detail.empty() ? std::wstring{} : L" -- " + reply.detail;
    switch (reply.kind)
    {
    case ReplyKind::Ok: return std::format(L"ok {} {}{}", reply.line, reply.command, tail);
    case ReplyKind::Failed: return std::format(L"failed {} {}{}", reply.line, reply.command, tail);
    case ReplyKind::Rejected: return std::format(L"rejected {} {}{}", reply.line, reply.command, tail);
    case ReplyKind::Dropped: return std::format(L"dropped {} {}{}", reply.line, reply.command, tail);
    case ReplyKind::Value: return std::format(L"value {} {}", reply.command, reply.detail);
    case ReplyKind::Help: return std::format(L"help {}{}", reply.command, reply.detail);
    case ReplyKind::Note: return std::format(L"note {} {}", reply.line, reply.detail);
    }
    return L"?";
}

Runner::Client& Runner::Find(ClientId id)
{
    for (auto& client : clients_)
    {
        if (client.id == id)
        {
            return client;
        }
    }
    clients_.push_back({id});
    return clients_.back();
}

void Runner::Send(ClientId client, ReplyKind kind, std::uint32_t line, std::wstring command, std::wstring detail)
{
    replies_.push_back({client, kind, line, std::move(command), std::move(detail)});
}

std::vector<Reply> Runner::TakeReplies()
{
    std::vector<Reply> out;
    out.swap(replies_);
    return out;
}

bool Runner::Idle() const noexcept
{
    return held_.empty() &&
           std::all_of(clients_.begin(), clients_.end(), [](const Client& c) { return c.queue.empty(); });
}

bool Runner::Busy(ClientId id) const noexcept
{
    const bool queued = std::any_of(clients_.begin(), clients_.end(),
                                    [id](const Client& c) { return c.id == id && !c.queue.empty(); });
    return queued || std::any_of(held_.begin(), held_.end(), [id](const Hold& h) { return h.client == id; });
}

void Runner::Submit(ClientId id, std::wstring_view line)
{
    Client&             client = Find(id);
    const std::uint32_t number = ++client.lines;  // every line, so a script's numbers are its file lines
    auto                parsed = ParseLine(registry_, line);
    if (!parsed.command)
    {
        if (!parsed.error.empty())
        {
            Send(id, ReplyKind::Rejected, number, std::wstring(line), parsed.error);
        }
        return;  // a blank line or a comment
    }
    const Command& command = *parsed.command;
    if (command.verb == Verb::Abort)
    {
        for (auto& each : clients_)
        {
            DropQueue(each, L"aborted");
        }
        ReleaseHolds(0, true);
        Send(id, ReplyKind::Ok, number, L"abort");
        return;
    }
    if (IsQuery(command.verb) && client.queue.empty())
    {
        Answer(client, number, command);
        return;
    }
    client.queue.push_back({number, command});
}

void Runner::Answer(const Client& client, std::uint32_t line, const Command& command)
{
    switch (command.verb)
    {
    case Verb::Get:
        if (const Entry* entry = registry_.Find(command.name); entry != nullptr && entry->get)
        {
            Send(client.id, ReplyKind::Value, 0, entry->name, Format(*entry, entry->get()));
        }
        break;
    case Verb::Status:
        for (const auto& entry : registry_.Entries())
        {
            if (entry.get)
            {
                Send(client.id, ReplyKind::Value, 0, entry.name, Format(entry, entry.get()));
            }
        }
        break;
    case Verb::Help:
        for (const auto& entry : registry_.Entries())
        {
            if (command.name.empty() || entry.name == command.name)
            {
                const std::wstring shape = ShapeText(entry);
                const wchar_t*     kind  = entry.set ? (entry.get ? L"" : L"(action) ") : L"(read only) ";
                Send(client.id, ReplyKind::Help, 0, entry.name,
                     std::format(L"{}{} -- {}{}", shape.empty() ? L"" : L" ", shape, kind, entry.help));
            }
        }
        break;
    case Verb::Note:
        Send(client.id, ReplyKind::Note, line, L"note", command.text);
        return;
    default:
        break;
    }
    Send(client.id, ReplyKind::Ok, line, Describe(registry_, command));
}

void Runner::ClientClosed(ClientId id)
{
    for (auto it = clients_.begin(); it != clients_.end(); ++it)
    {
        if (it->id == id)
        {
            DropQueue(*it, L"the client went away");
            clients_.erase(it);
            break;
        }
    }
    ReleaseHolds(id, false);
}

void Runner::Shutdown()
{
    for (auto& client : clients_)
    {
        DropQueue(client, L"the console closed");
    }
    ReleaseHolds(0, true);
}

void Runner::DropQueue(Client& client, std::wstring_view reason)
{
    for (const auto& queued : client.queue)
    {
        Send(client.id, ReplyKind::Dropped, queued.line, Describe(registry_, queued.command), std::wstring(reason));
    }
    client.queue.clear();
    client.waiting   = false;
    client.waitTicks = 0;
}

void Runner::ReleaseHolds(ClientId client, bool everyClient)
{
    std::vector<Hold> kept;
    for (const auto& hold : held_)
    {
        if (everyClient || hold.client == client)
        {
            holds_.ReleaseStick(hold.player);
        }
        else
        {
            kept.push_back(hold);
        }
    }
    held_.swap(kept);
}

void Runner::Fail(Client& client, const Queued& failed, const std::wstring& reason)
{
    Send(client.id, ReplyKind::Failed, failed.line, Describe(registry_, failed.command), reason);
    DropQueue(client, L"after a failure");
    ReleaseHolds(client.id, false);
}

bool Runner::ConditionHolds(const Command& command) const
{
    const Entry* entry = registry_.Find(command.name);
    if (entry == nullptr || !entry->get)
    {
        return false;
    }
    const Value current = entry->get();
    if (const auto* on = std::get_if<bool>(&current); on && std::holds_alternative<bool>(command.value))
    {
        return Compares(command.compare, *on, std::get<bool>(command.value));
    }
    if (const auto* n = std::get_if<std::int64_t>(&current); n && std::holds_alternative<std::int64_t>(command.value))
    {
        return Compares(command.compare, *n, std::get<std::int64_t>(command.value));
    }
    if (const auto* x = std::get_if<double>(&current); x && std::holds_alternative<double>(command.value))
    {
        return Compares(command.compare, *x, std::get<double>(command.value));
    }
    if (const auto* s = std::get_if<std::wstring>(&current); s && std::holds_alternative<std::wstring>(command.value))
    {
        return Compares(command.compare, *s, std::get<std::wstring>(command.value));
    }
    // A number against a named value (split-fov 0.5 against native): different kinds are never equal.
    return command.compare == Compare::NotEqual && !std::holds_alternative<std::monostate>(current);
}

void Runner::Tick(std::uint64_t)
{
    for (std::size_t i = 0; i < held_.size();)
    {
        if (!holds_.Held(held_[i].player))
        {
            // The mod released it on its own (the join it was made in ended): the client waits for it no longer.
            held_.erase(held_.begin() + static_cast<std::ptrdiff_t>(i));
        }
        else if (--held_[i].ticks == 0)
        {
            holds_.ReleaseStick(held_[i].player);
            held_.erase(held_.begin() + static_cast<std::ptrdiff_t>(i));
        }
        else
        {
            ++i;
        }
    }
    for (std::size_t i = 0; i < clients_.size(); ++i)
    {
        Advance(clients_[i]);  // by index: nothing here adds a client, but keep clients_ stable anyway
    }
}

void Runner::Advance(Client& client)
{
    if (client.waiting)
    {
        const Queued head = client.queue.front();
        if (head.command.verb == Verb::WaitUntil && ConditionHolds(head.command))
        {
            client.waiting = false;
        }
        else if (--client.waitTicks > 0)
        {
            return;
        }
        else if (head.command.verb == Verb::WaitUntil)
        {
            client.queue.pop_front();
            client.waiting = false;
            Fail(client, head, std::format(L"timed out after {} ticks", head.command.ticks));
            return;
        }
        else
        {
            client.waiting = false;
        }
        client.queue.pop_front();
        Send(client.id, ReplyKind::Ok, head.line, Describe(registry_, head.command));
    }
    for (std::size_t ran = 0; ran < kMaxCommandsPerTick && !client.queue.empty(); ++ran)
    {
        if (!Run(client))
        {
            return;
        }
    }
}

bool Runner::Run(Client& client)
{
    const Queued   head    = client.queue.front();  // a copy: pop_front below ends the element
    const Command& command = head.command;
    if (command.verb == Verb::Wait || (command.verb == Verb::WaitUntil && !ConditionHolds(command)))
    {
        client.waiting   = true;
        client.waitTicks = command.ticks;
        return false;
    }
    client.queue.pop_front();
    if (IsQuery(command.verb))
    {
        Answer(client, head.line, command);  // its turn has come: the commands before it have run
        return true;
    }

    Outcome outcome;
    switch (command.verb)
    {
    case Verb::Entry:
        if (const Entry* entry = registry_.Find(command.name); entry != nullptr && entry->set)
        {
            outcome = entry->set(command.value);
        }
        else
        {
            outcome = {false, L"no longer registered"};
        }
        break;
    case Verb::Hold:
        outcome = holds_.HoldStick(command.player, command.axes);
        if (outcome.ok)
        {
            std::erase_if(held_, [&](const Hold& hold) { return hold.player == command.player; });
            held_.push_back({client.id, command.player, command.ticks});
        }
        break;
    default:
        break;  // a wait-until that held at once
    }
    if (!outcome.ok)
    {
        Fail(client, head, outcome.detail);
        return true;
    }
    Send(client.id, ReplyKind::Ok, head.line, Describe(registry_, command), outcome.detail);
    return true;
}

}  // namespace lss::control
