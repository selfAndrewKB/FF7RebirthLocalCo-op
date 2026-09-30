#include "players/single_view.h"

namespace lss::players {

SingleViewChange SingleView::Update(bool playerTwoIn, bool wholeScreenShown) noexcept
{
    const bool wanted = playerTwoIn && (mode_ == SingleViewMode::Single ||
                                        (mode_ == SingleViewMode::Auto && wholeScreenShown));
    if (wanted != wanted_)
    {
        wanted_ = wanted;
        failed_ = false;
    }
    if (failed_ || wanted == single_)
    {
        return {};
    }
    return wanted ? SetSingle() : SetSplit();
}

SingleViewChange SingleView::SetSingle() noexcept
{
    const auto current = access_.read();
    if (!current)
    {
        failed_ = true;
        return {SingleViewChange::Kind::SingleFailed};
    }
    if (*current)
    {
        single_ = true;
        owned_  = false;  // someone else's: never cleared by the mod
        return {SingleViewChange::Kind::AlreadySingle};
    }
    const auto after = access_.write(true) ? access_.read() : std::nullopt;
    if (!after || !*after)
    {
        failed_ = true;
        return {SingleViewChange::Kind::SingleFailed};
    }
    single_ = true;
    owned_  = true;
    return {SingleViewChange::Kind::Single};
}

SingleViewChange SingleView::SetSplit() noexcept
{
    if (!owned_)
    {
        single_ = false;  // not the mod's switch: left as it was found
        return {SingleViewChange::Kind::LeftAlone};
    }
    const auto current = access_.read();
    if (!current)
    {
        failed_ = true;
        return {SingleViewChange::Kind::SplitFailed};
    }
    if (!*current)
    {
        single_ = owned_ = false;  // someone cleared it meanwhile
        return {SingleViewChange::Kind::LeftAlone};
    }
    const auto after = access_.write(false) ? access_.read() : std::nullopt;
    if (!after || *after)
    {
        failed_ = true;
        return {SingleViewChange::Kind::SplitFailed};
    }
    single_ = owned_ = false;
    return {SingleViewChange::Kind::Split};
}

}  // namespace lss::players
