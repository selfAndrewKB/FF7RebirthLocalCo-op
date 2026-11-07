#include "players/split_layout.h"

#include "platform/log.h"
#include "platform/settings.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

#include <format>

namespace lss::players {

namespace {

std::uint8_t Other(std::uint8_t layout) noexcept
{
    return layout == static_cast<std::uint8_t>(SplitLayoutKind::SideBySide)
               ? static_cast<std::uint8_t>(SplitLayoutKind::TopBottom)
               : static_cast<std::uint8_t>(SplitLayoutKind::SideBySide);
}

}  // namespace

HeldChange SplitLayout::Update(bool twoColumns) noexcept
{
    return held_.Update(twoColumns, chosen_);  // nothing chosen: the game's layout stays
}

HeldChange SplitLayout::Toggle() noexcept
{
    // Switch from what is on screen: the chosen layout if there is one, otherwise the game's.
    std::uint8_t from = 0;
    if (chosen_)
    {
        from = *chosen_;
    }
    else if (const auto current = held_.Read())
    {
        from = *current;
    }
    else
    {
        return {HeldChange::Kind::Unreadable};  // the other layout is unknown; nothing chosen
    }
    return Choose(Other(from));
}

HeldChange SplitLayout::Choose(std::uint8_t value) noexcept
{
    chosen_ = value;
    return held_.In() ? held_.Hold(value) : HeldChange{HeldChange::Kind::Chosen, 0, value};
}

std::wstring LayoutText(std::int32_t value)
{
    const wchar_t* name = value == static_cast<std::int32_t>(SplitLayoutKind::SideBySide) ? L"side by side"
                          : value == static_cast<std::int32_t>(SplitLayoutKind::TopBottom) ? L"top/bottom"
                                                                                           : L"another value";
    return std::format(L"{} ({})", value, name);
}

SplitLayoutKind LayoutOfWord(std::wstring_view word) noexcept
{
    return word == L"vertical" ? SplitLayoutKind::SideBySide : SplitLayoutKind::TopBottom;
}

const wchar_t* WordOfLayout(std::uint8_t value) noexcept
{
    return value == static_cast<std::uint8_t>(SplitLayoutKind::SideBySide) ? L"vertical" : L"horizontal";
}

void ChooseFromSettings(SplitLayout& layout) noexcept
{
    namespace settings = platform::settings;
    const std::wstring& word = settings::Of(settings::kTwoPlayerSplit).word;
    const auto          kind = LayoutOfWord(word);
    (void)layout.Choose(static_cast<std::uint8_t>(kind));  // nothing is in at start: the choice waits for the first join
    try
    {
        platform::Log(L"split layout: two players in one window: {} ({}; {})", word,
                      kind == SplitLayoutKind::SideBySide ? L"side by side" : L"top/bottom",
                      settings::Why(settings::kTwoPlayerSplit));
    }
    catch (...)
    {
        // Out of memory building the line: the choice is made, the log line is not.
    }
}

#if LSS_DEV

void RegisterCommands(SplitLayout& layout)
{
    using control::Outcome;
    control::RegisterOption(
        platform::settings::kTwoPlayerSplit,
        [&layout](const std::wstring& word) {
            using Kind          = HeldChange::Kind;
            const auto kind     = LayoutOfWord(word);
            const auto change   = layout.Choose(static_cast<std::uint8_t>(kind));
            const wchar_t* name = kind == SplitLayoutKind::SideBySide ? L"side by side" : L"top/bottom";
            switch (change.kind)
            {
                case Kind::Applied:
                case Kind::AlreadySet: return Outcome{true, name};
                case Kind::Chosen: return Outcome{true, std::wstring(name) + L" from the next join"};
                case Kind::ApplyFailed: return Outcome{false, L"TwoPlayerSplitscreenLayout could not be read or set"};
                default: return Outcome{false, L"unexpected layout outcome"};
            }
        },
        [&layout] {
            // Always one of the option's words, so `wait-until two-player-split == ...` compares; empty only when
            // the game's setting cannot be read.
            const auto current = layout.Current();
            return current ? std::wstring(WordOfLayout(*current)) : std::wstring();
        });
}

#endif

}  // namespace lss::players
