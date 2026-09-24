#include "players/split_layout.h"

namespace lss::players {

namespace {

std::uint8_t Other(std::uint8_t layout) noexcept
{
    return layout == static_cast<std::uint8_t>(SplitLayoutKind::SideBySide)
               ? static_cast<std::uint8_t>(SplitLayoutKind::TopBottom)
               : static_cast<std::uint8_t>(SplitLayoutKind::SideBySide);
}

}  // namespace

LayoutChange SplitLayout::Update(bool playerTwoIn) noexcept
{
    if (playerTwoIn == playerTwoIn_)
    {
        return {};
    }
    playerTwoIn_ = playerTwoIn;
    if (!playerTwoIn)
    {
        return Restore();
    }
    return chosen_ ? Apply(*chosen_) : LayoutChange{};  // nothing chosen: the game's layout stays
}

LayoutChange SplitLayout::Toggle() noexcept
{
    // Switch from what is on screen: the chosen layout if there is one, otherwise the game's.
    std::uint8_t from = 0;
    if (chosen_)
    {
        from = *chosen_;
    }
    else if (const auto current = access_.read())
    {
        from = *current;
    }
    else
    {
        return {LayoutChange::Kind::Unreadable, 0};  // the other layout is unknown; nothing chosen
    }
    chosen_ = Other(from);
    return playerTwoIn_ ? Apply(*chosen_) : LayoutChange{LayoutChange::Kind::Chosen, *chosen_};
}

LayoutChange SplitLayout::Apply(std::uint8_t value) noexcept
{
    const auto current = access_.read();
    if (!current)
    {
        return {LayoutChange::Kind::ApplyFailed, value};
    }
    if (*current != value)
    {
        if (!access_.write(value))
        {
            return {LayoutChange::Kind::ApplyFailed, value};
        }
        if (!restoreTo_)
        {
            restoreTo_ = *current;  // the game's value, from before the mod's first write
        }
    }
    if (restoreTo_ == value)
    {
        restoreTo_.reset();  // switched back to the game's own: nothing to restore
    }
    const auto after = access_.read();
    return after && *after == value ? LayoutChange{LayoutChange::Kind::Applied, value}
                                    : LayoutChange{LayoutChange::Kind::ApplyFailed, value};
}

LayoutChange SplitLayout::Restore() noexcept
{
    if (!restoreTo_)
    {
        return {};  // the game's value is in place
    }
    const std::uint8_t previous = *restoreTo_;

    const auto current = access_.read();
    if (!current)
    {
        return {LayoutChange::Kind::RestoreFailed, previous};  // unknown: kept, retried next leave
    }
    if (*current == previous)
    {
        restoreTo_.reset();
        return {};
    }
    if (!chosen_ || *current != *chosen_)
    {
        restoreTo_.reset();  // someone set another value after the mod
        return {LayoutChange::Kind::LeftAlone, *current};
    }
    if (!access_.write(previous))
    {
        return {LayoutChange::Kind::RestoreFailed, previous};
    }
    const auto after = access_.read();
    if (!after || *after != previous)
    {
        return {LayoutChange::Kind::RestoreFailed, previous};
    }
    restoreTo_.reset();
    return {LayoutChange::Kind::Restored, previous};
}

}  // namespace lss::players
