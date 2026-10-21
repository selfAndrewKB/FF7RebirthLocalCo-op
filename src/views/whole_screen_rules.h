#pragma once

// Pure pieces of the whole-screen reads (views/whole_screen.cpp), tested on the host (tests/whole_screen_test.cpp):
// which location names are the title and the minigame-only maps, the Chadley manager's record check, and the
// encounter states that are a battle.

#include <array>
#include <cstdint>
#include <string_view>

namespace lss::views {

/// The title's location name, which the game compares against itself (RE-p2-single-view.md §7.3).
inline constexpr std::wstring_view kTitleName = L"0005-TITLB";

/// The minigame-only maps (the game state's location).
inline constexpr std::array<std::wstring_view, 4> kMinigameMaps = {L"2351-CRACE", L"2352-SCOST", L"2353-GBIKE",
                                                                   L"7110-MOGHS"};

struct LocationClass
{
    bool title       = false;
    bool minigameMap = false;
};

constexpr LocationClass ClassifyLocation(std::wstring_view name) noexcept
{
    LocationClass location;
    location.title = name == kTitleName;
    for (const std::wstring_view map : kMinigameMaps)
    {
        location.minigameMap = location.minigameMap || name == map;
    }
    return location;
}

/// A Chadley record accepted only inside the manager's own table: `count` records of `recordSize` bytes from `table`,
/// at most eight, and `record` on one of them.
constexpr bool ChadleyRecordValid(std::uintptr_t table, std::uintptr_t record, std::int32_t count,
                                  std::uint32_t recordSize) noexcept
{
    return table != 0 && record >= table && count > 0 && count <= 8 &&
           record < table + recordSize * static_cast<std::uint32_t>(count) && (record - table) % recordSize == 0;
}

/// A Chadley record's id as the log shows it (0..127).
constexpr bool ChadleyIdValid(std::int32_t id) noexcept
{
    return id >= 0 && id <= 127;
}

/// The encounter states UEndBattleAPI::IsInBattle counts as a battle.
constexpr bool EncounterInBattle(std::uint8_t state) noexcept
{
    return state == 3 || state == 4;
}

}  // namespace lss::views
