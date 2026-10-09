#include "players/split_screen_settings.h"

#include <cwchar>
#include <iterator>

namespace lss::players {

// Two rules keep a failed read from ever being mistaken for a known value:
//   - once a set command has RUN, the previous value is remembered, whatever the read-back says:
//     the setting may have changed even when confirming it failed;
//   - a value that cannot be read is never overwritten on the way out: the restore is reported as
//     failed and retried at the next leave, because the value may be someone else's choice.

SettingsChange SplitScreenSettings::Update(bool playerTwoIn) noexcept
{
    if (playerTwoIn == playerTwoIn_)
    {
        return {};
    }
    playerTwoIn_ = playerTwoIn;
    return playerTwoIn ? Apply() : Restore();
}

SettingsChange SplitScreenSettings::Apply() noexcept
{
    const auto current = console_.readInt(name_);
    if (!current)
    {
        return {SettingsChange::Kind::ApplyFailed, 0};  // nothing ran
    }
    if (*current == value_)
    {
        // Already at the value: nothing to set. A value still owed from a failed restore is kept.
        return {SettingsChange::Kind::AlreadySet, *current};
    }
    if (!Execute(value_))
    {
        return {SettingsChange::Kind::ApplyFailed, *current};  // the command never ran
    }
    restoreTo_ = *current;
    const auto after = console_.readInt(name_);
    return after && *after == value_ ? SettingsChange{SettingsChange::Kind::Applied, *current}
                                                    : SettingsChange{SettingsChange::Kind::ApplyFailed, *current};
}

SettingsChange SplitScreenSettings::Restore() noexcept
{
    if (!restoreTo_)
    {
        return {};  // this object never ran a command
    }
    const std::int32_t previous = *restoreTo_;

    const auto current = console_.readInt(name_);
    if (!current)
    {
        return {SettingsChange::Kind::RestoreFailed, previous};  // unknown: kept, retried next leave
    }
    if (*current == previous)
    {
        restoreTo_.reset();  // our command did not take, or it was already put back
        return {};
    }
    if (*current != value_)
    {
        restoreTo_.reset();  // someone chose another value after us
        return {SettingsChange::Kind::LeftAlone, *current};
    }
    if (!Execute(previous))
    {
        return {SettingsChange::Kind::RestoreFailed, previous};
    }
    const auto after = console_.readInt(name_);
    if (!after || *after != previous)
    {
        return {SettingsChange::Kind::RestoreFailed, previous};  // kept: the next leave checks again
    }
    restoreTo_.reset();
    return {SettingsChange::Kind::Restored, previous};
}

bool SplitScreenSettings::Execute(std::int32_t value) noexcept
{
    wchar_t command[96]{};
    if (std::swprintf(command, std::size(command), L"%ls %d", name_, value) < 0)
    {
        return false;
    }
    return console_.execute(command);
}

}  // namespace lss::players
