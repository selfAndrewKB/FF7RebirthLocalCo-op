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

SingleViewChange SingleView::Update(bool playerTwoIn, const SingleViewInputs& inputs) noexcept
{
    const bool waits  = inputs.waitsForMember && mode_ == SingleViewMode::Auto;
    auto       change = Update(playerTwoIn, SingleViewWanted(mode_ == SingleViewMode::Auto, inputs.wholeScreenShown,
                                                            inputs.waitsForMember, inputs.standsAside));
    if (waits != waited_)
    {
        waited_      = waits;
        change.waits = waits ? SingleViewChange::Waits::Began : SingleViewChange::Waits::Ended;
    }
    return change;
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

std::wstring Describe(const SingleViewChange& change)
{
    using Kind = SingleViewChange::Kind;
    switch (change.kind)
    {
        case Kind::None: return {};
        case Kind::Single:
            return L"single view: player 1's view alone (SetForceDisableSplitscreen(true)); player 2 stays in";
        case Kind::AlreadySingle:
            return L"single view: the split was already force-disabled by someone else; used, and not cleared by the mod";
        case Kind::SingleFailed: return L"single view: the split switch could not be read or set; the split stays";
        case Kind::Split: return L"single view: over, the split is back (SetForceDisableSplitscreen(false))";
        case Kind::LeftAlone:
            return L"single view: over; the split switch is not the mod's (or was cleared meanwhile), left as it is";
        case Kind::SplitFailed:
            return L"single view: the split switch could not be read or cleared; the screen stays single";
    }
    return {};
}

std::wstring DescribeWaits(const SingleViewChange& change)
{
    switch (change.waits)
    {
        case SingleViewChange::Waits::Same: return {};
        case SingleViewChange::Waits::Began:
            return L"player 2 waits for a party member: player 1's single view until player 2 takes one";
        case SingleViewChange::Waits::Ended: return L"player 2 no longer waits for a party member";
    }
    return {};
}

}  // namespace lss::players
