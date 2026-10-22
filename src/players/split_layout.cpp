#include "players/split_layout.h"

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

HeldChange SplitLayout::Update(bool playerTwoIn) noexcept
{
    return held_.Update(playerTwoIn, chosen_);  // nothing chosen: the game's layout stays
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

}  // namespace lss::players
