#include "players/split_screen_settings.h"

namespace lss::players {

SplitScreenSettings::SplitScreenSettings(SettingAccess<std::int32_t> access) noexcept
    : settings_{HeldSetting<std::int32_t>(access, SettingOf(SettingsStep::Resolution).name),
                HeldSetting<std::int32_t>(access, SettingOf(SettingsStep::Timing).name),
                HeldSetting<std::int32_t>(access, SettingOf(SettingsStep::Gap).name)}
{
}

void SplitScreenSettings::Update(bool splitShown, void (*log)(const std::wstring& line)) noexcept
{
    for (const auto step : SettingsOrder(splitShown))
    {
        auto& setting = settings_[static_cast<std::size_t>(step)];
        const auto line = Describe(setting.Update(splitShown, SettingOf(step).value), L"split screen", setting.Name());
        if (!line.empty())
        {
            log(line);
        }
    }
}

}  // namespace lss::players
