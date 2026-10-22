#include "players/held_setting.h"

#include <format>

namespace lss::players {

std::wstring Describe(const HeldChange& change, const wchar_t* label, const wchar_t* name, ValueText text)
{
    using Kind = HeldChange::Kind;
    const auto of = [text](std::int32_t value) { return text != nullptr ? text(value) : std::to_wstring(value); };
    switch (change.kind)
    {
        case Kind::None: return {};
        case Kind::Chosen: return std::format(L"{}: {} {} from the next join", label, name, of(change.held));
        case Kind::Unreadable: return std::format(L"{}: {} cannot be read; nothing chosen", label, name);
        case Kind::Applied:
            return std::format(L"{}: {} {} -> {} until the leave", label, name, of(change.value), of(change.held));
        case Kind::AlreadySet: return std::format(L"{}: {} is already {}; left as it is", label, name, of(change.held));
        case Kind::ApplyFailed:
            return std::format(L"{}: could not set {} to {}; left as it is", label, name, of(change.held));
        case Kind::Restored: return std::format(L"{}: {} restored to {}", label, name, of(change.value));
        case Kind::LeftAlone:
            return std::format(L"{}: {} was changed to {} meanwhile; left as it is", label, name, of(change.value));
        case Kind::RestoreFailed:
            return std::format(L"{}: could not restore {} to {}; tried again at the next leave", label, name,
                               of(change.value));
    }
    return {};
}

}  // namespace lss::players
