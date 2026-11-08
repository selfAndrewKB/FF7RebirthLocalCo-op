// Host-side tests for where the title menu's Co-op row goes (menu/title_row_rules.h): the start menu's list as the game
// builds it, a list rebuilt or not, a list without Options.

#include "menu/title_row_rules.h"

#include "expect.h"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

namespace {

using lss::menu::title_row_rules::PlaceBelow;

constexpr std::int32_t kOptions = 3, kCoop = 9;

std::optional<std::size_t> Place(std::vector<std::int32_t> kinds)
{
    return PlaceBelow(kinds, kOptions, kCoop);
}

}  // namespace

int main()
{
    std::puts("title row: the Co-op row goes directly below Options, once");
    EXPECT(Place({0, 1, 2, 3, 6, 4}) == 4);   // the full game's list: below Options
    EXPECT(Place({0, 1, 2, 3, 4}) == 4);
    EXPECT(!Place({0, 1, 2, 3, 9, 6, 4}));    // already in: never twice
    EXPECT(!Place({0, 1, 2, 4}));             // no Options row
    EXPECT(!Place({}));
    return lss::test::Report();
}
