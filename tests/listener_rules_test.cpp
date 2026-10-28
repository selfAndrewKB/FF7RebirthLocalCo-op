// Host-side tests for player_two::listener_rules, run without the game.
//
// The nearest-of-N choice with its hold and hand-over, the choice table between a sound's test and its update, the
// gates' nearest listener, the quaternion rotation the device's push uses, the audibility compare and the LOD rules.
// Remake's tests (audio/listener_rules_test.cpp) for the rules taken from it.

#include "player_two/feedback/listener_rules.h"

#include "expect.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>

namespace {

namespace rules = lss::player_two::listener_rules;

bool Near(const rules::Vec3& a, const rules::Vec3& b)
{
    return rules::Distance(a, b) < 1e-5f;
}

void TestWithinAndDistance()
{
    std::puts("listener: a sound is heard closer than its range, never at it, never with no range");
    EXPECT(rules::Within(1000.0f, 999.0f));
    EXPECT(!rules::Within(1000.0f, 1000.0f));
    EXPECT(!rules::Within(1000.0f, 1500.0f));
    EXPECT(!rules::Within(0.0f, 0.0f));
    EXPECT(!rules::Within(-5.0f, 0.0f));
    EXPECT(!rules::Within(std::nanf(""), 0.0f));
    EXPECT(rules::Within(100.0f * 2.0f, 150.0f));  // a moving sound's range times the magnification
    EXPECT(rules::Distance({0, 0, 0}, {300, 400, 0}) == 500.0f);
}

void TestRotation()
{
    std::puts("rotate: the identity keeps a vector; a 90 degree yaw turns x into y; the push's front and up");
    const float identity[4] = {0, 0, 0, 1};
    EXPECT(Near(rules::RotateVector(identity, {1, 2, 3}), {1, 2, 3}));
    const float s        = std::sqrt(0.5f);
    const float yaw90[4] = {0, 0, s, s};  // about z by 90 degrees
    EXPECT(Near(rules::RotateVector(yaw90, {1, 0, 0}), {0, 1, 0}));
    EXPECT(Near(rules::RotateVector(yaw90, {0, 1, 0}), {-1, 0, 0}));
    EXPECT(Near(rules::SeadFront(identity), {1, 0, 0}));
    EXPECT(Near(rules::SeadUp(identity), {0, 0, -1}));
    EXPECT(Near(rules::SeadFront(yaw90), {0, 1, 0}));
    EXPECT(Near(rules::SeadUp(yaw90), {0, 0, -1}));
}

void TestNearest()
{
    std::puts("gates: the nearest player's listener; a tie or none on is player 1's");
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT(rules::Nearest(std::array{500.0f, 200.0f, 900.0f}) == 1);
    EXPECT(rules::Nearest(std::array{500.0f, 900.0f, 200.0f}) == 2);
    EXPECT(rules::Nearest(std::array{300.0f, 300.0f, 300.0f}) == 0);
    EXPECT(rules::Nearest(std::array{500.0f, inf, nan}) == 0);
    EXPECT(rules::Nearest(std::array{nan, 700.0f, inf}) == 1);  // player 1's unreadable: the nearest on
    EXPECT(rules::Nearest(std::array{nan, nan, inf}) == 0);
}

void TestChooseNearest()
{
    std::puts("listener: a new sound takes the nearest listener; ties go to player 1");
    const float inf  = std::numeric_limits<float>::infinity();
    const float nan  = std::numeric_limits<float>::quiet_NaN();
    const float wide = 100000.0f;  // a range every listener is within
    EXPECT(rules::ChooseNearest(rules::kNoChoice, std::array{500.0f, 200.0f, 900.0f}, wide) == 1);
    EXPECT(rules::ChooseNearest(rules::kNoChoice, std::array{300.0f, 300.0f, 300.0f}, wide) == 0);
    EXPECT(rules::ChooseNearest(rules::kNoChoice, std::array{500.0f, inf, 100.0f}, wide) == 2);

    std::puts("listener: a chosen listener within range is kept until another is 20 % nearer");
    EXPECT(rules::ChooseNearest(0, std::array{500.0f, 450.0f, inf}, wide) == 0);  // 450 is not below 400
    EXPECT(rules::ChooseNearest(0, std::array{500.0f, 390.0f, inf}, wide) == 1);
    EXPECT(rules::ChooseNearest(0, std::array{500.0f, 400.0f, inf}, wide) == 0);  // not strictly nearer: no flip
    EXPECT(rules::ChooseNearest(1, std::array{390.0f, 450.0f, inf}, wide) == 1);  // back to player 1 needs below 360
    EXPECT(rules::ChooseNearest(1, std::array{350.0f, 450.0f, inf}, wide) == 0);
    EXPECT(rules::ChooseNearest(2, std::array{900.0f, 100.0f, 110.0f}, wide) == 2);  // between two seats: held
    EXPECT(rules::ChooseNearest(0, std::array{500.0f, 450.0f, inf}, 0.0f) == 1);  // no range known yet: the nearest

    std::puts("listener: a chosen listener out of the sound's range is left for the nearest");
    EXPECT(rules::ChooseNearest(1, std::array{900.0f, 1050.0f, inf}, 1000.0f) == 0);
    EXPECT(rules::ChooseNearest(1, std::array{900.0f, 1000.0f, inf}, 1000.0f) == 0);  // at the range is out (strict)
    EXPECT(rules::ChooseNearest(1, std::array{900.0f, 990.0f, inf}, 1000.0f) == 1);   // within: held
    EXPECT(rules::ChooseNearest(2, std::array{3000.0f, 2500.0f, 2600.0f}, 1000.0f) == 1);  // nobody hears it: nearest

    std::puts("listener: a listener that goes off is left at once; none on is player 1's");
    EXPECT(rules::ChooseNearest(2, std::array{900.0f, 800.0f, inf}, wide) == 1);
    EXPECT(rules::ChooseNearest(1, std::array{900.0f, nan, inf}, wide) == 0);
    EXPECT(rules::ChooseNearest(1, std::array{nan, nan, nan}, wide) == 0);
    EXPECT(rules::ChooseNearest(7, std::array{900.0f, 100.0f}, wide) == 1);  // a stale index outside the list
}

void TestChoiceTable()
{
    std::puts("listener: a choice is the update's in its frame, held into the next, gone after");
    rules::ChoiceTable<4, 4> table;
    int a = 0, b = 0;
    EXPECT(table.Held(&a, 10) == rules::kNoChoice);
    EXPECT(table.Record(&a, 2, 10));
    EXPECT(table.ChosenIn(&a, 10) == 2);
    EXPECT(table.Held(&a, 11) == 2);
    EXPECT(table.ChosenIn(&a, 11) == rules::kNoChoice);  // its test has not run in frame 11
    EXPECT(table.Held(&a, 12) == rules::kNoChoice);      // not refreshed in frame 11: lapsed
    EXPECT(table.Record(&b, 1, 20));
    EXPECT(table.Held(&b, 21) == 1);  // a new sound at a freed sound's address inherits its hold (the range decides)

    std::puts("listener: sounds that share a home slot keep their own choices");
    rules::ChoiceTable<1, 2> two;  // two entries, each sound's probe covers both
    int c = 0;
    EXPECT(two.Record(&a, 1, 5));
    EXPECT(two.Record(&b, 2, 5));
    EXPECT(!two.Record(&c, 0, 5));  // no free entry: refused, the others' choices kept
    EXPECT(two.ChosenIn(&a, 5) == 1 && two.ChosenIn(&b, 5) == 2);
    EXPECT(two.ChosenIn(&c, 5) == rules::kNoChoice);
    EXPECT(two.Record(&c, 2, 7));  // a's and b's lapsed: c takes one
    EXPECT(two.ChosenIn(&c, 7) == 2);
    EXPECT(two.Held(&a, 7) == rules::kNoChoice);

    rules::ChoiceTable<4, 16> many;
    std::array<int, 16>       sounds{};
    bool                      all = true;
    for (std::size_t i = 0; i < sounds.size(); ++i)
    {
        all = many.Record(&sounds[i], static_cast<int>(i % 3), 1) && all;
    }
    EXPECT(all);
    for (std::size_t i = 0; i < sounds.size(); ++i)
    {
        EXPECT(many.ChosenIn(&sounds[i], 1) == static_cast<int>(i % 3));
    }
}

void TestLod()
{
    std::puts("lod: kept at the finest or with no filter; lowered only by a lower level");
    EXPECT(rules::LodKeepsFirst(0));
    EXPECT(rules::LodKeepsFirst(-1));
    EXPECT(!rules::LodKeepsFirst(2));
    EXPECT(rules::LodLowered(2, 1));
    EXPECT(rules::LodLowered(2, 0));
    EXPECT(!rules::LodLowered(2, 2));
    EXPECT(!rules::LodLowered(2, 3));
    EXPECT(rules::LodLowered(1, -1));  // -1 (no filter from that player's place) is lower still: no cut
}

}  // namespace

int main()
{
    TestWithinAndDistance();
    TestRotation();
    TestNearest();
    TestChooseNearest();
    TestChoiceTable();
    TestLod();

    return lss::test::Report();
}
