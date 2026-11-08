// Host-side tests for views/view_shape.h, run without the game: the view info's recognition, the FOV ratios, the
// viewport-size watch and the settings words of the split FOV's weight.
//
// The view info is a zeroed 0x6A0-byte block (FMinimalViewInfo's size in this build) given the
// shape the game's cameras produce: 16:9, bars on. A block with implausible numbers is not
// recognised, so it is never written. The FOV rules themselves are views/split_fov_rules.h's
// (tests/split_fov_rules_test.cpp).

#include "views/minimal_view_info.h"
#include "views/view_shape.h"

#include "expect.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

using lss::views::LooksLikeViewInfo;
namespace view_info = lss::views::view_info;

struct ViewInfo
{
    std::array<std::byte, 0x6A0> bytes{};

    ViewInfo(float aspectRatio, unsigned char flags, float fov)
    {
        Set(view_info::kAspectRatio, aspectRatio);
        Set(view_info::kFov, fov);
        bytes[view_info::kFlags] = static_cast<std::byte>(flags);
    }
    void Set(std::size_t offset, float value) { std::memcpy(bytes.data() + offset, &value, sizeof(value)); }
};

constexpr float kSixteenByNine = 16.0f / 9.0f;

void TestGameCameraIsRecognised()
{
    // The flag bytes are ones the game produced: the unused bitfield bits hold stack leftovers.
    std::puts("shape: the game's camera (16:9, FOV 60) is recognised whatever the unused flag bits hold");
    for (const unsigned char flags : {0x00, 0x01, 0x03, 0xA3, 0xFB, 0x1B, 0x47})
    {
        EXPECT(LooksLikeViewInfo(ViewInfo(kSixteenByNine, flags, 60.0f).bytes.data()));
    }
}

void TestUnknownShapesAreRefused()
{
    std::puts("shape: implausible ratios or FOVs, and null, are refused");
    EXPECT(!LooksLikeViewInfo(ViewInfo(0.0f, 0x01, 90.0f).bytes.data()));
    EXPECT(!LooksLikeViewInfo(ViewInfo(100.0f, 0x01, 90.0f).bytes.data()));
    EXPECT(!LooksLikeViewInfo(ViewInfo(std::numeric_limits<float>::quiet_NaN(), 0x01, 90.0f).bytes.data()));
    EXPECT(!LooksLikeViewInfo(ViewInfo(kSixteenByNine, 0x01, 0.0f).bytes.data()));
    EXPECT(!LooksLikeViewInfo(ViewInfo(kSixteenByNine, 0x01, 180.0f).bytes.data()));
    EXPECT(!LooksLikeViewInfo(ViewInfo(kSixteenByNine, 0x01, std::numeric_limits<float>::infinity()).bytes.data()));
    EXPECT(!LooksLikeViewInfo(nullptr));
}

void TestRatiosAndWatch()
{
    std::puts("view shape: the width ratio and vertical FOV; the viewport-size watch resets, paces and logs on change");
    using lss::views::kViewportSizeIntervalTicks;
    using lss::views::VerticalFovDegrees;
    using lss::views::ViewportSizeWatch;
    using lss::views::WidthRatio;
    EXPECT(std::fabs(WidthRatio(90.0f, 90.0f) - 1.0f) < 1e-6f);
    EXPECT(WidthRatio(90.0f, 60.0f) > 1.7f && WidthRatio(90.0f, 60.0f) < 1.74f);  // tan 45 / tan 30
    EXPECT(WidthRatio(0.0f, 60.0f) == 1.0f);  // a zero ratio is unusable
    EXPECT(WidthRatio(std::numeric_limits<float>::quiet_NaN(), 60.0f) == 1.0f);
    EXPECT(std::fabs(VerticalFovDegrees(90.0f, 1.0f) - 90.0f) < 1e-3f);
    EXPECT(std::fabs(VerticalFovDegrees(90.0f, 16.0f / 9.0f) - 58.716f) < 0.01f);

    ViewportSizeWatch watch;
    EXPECT(watch.Next(false) == ViewportSizeWatch::Step::Reset);
    EXPECT(watch.Next(true) == ViewportSizeWatch::Step::Read);  // the first tick after a reset
    for (std::uint32_t i = 1; i < kViewportSizeIntervalTicks; ++i)
    {
        EXPECT(watch.Next(true) == ViewportSizeWatch::Step::Skip);
    }
    EXPECT(watch.Next(true) == ViewportSizeWatch::Step::Read);
    EXPECT(watch.SizeChanged(2560, 1440));
    EXPECT(!watch.SizeChanged(2560, 1440));
    EXPECT(watch.SizeChanged(8560, 1440));
    EXPECT(watch.SizeChanged(0, 0));  // unread: logged as a change
    EXPECT(watch.Next(false) == ViewportSizeWatch::Step::Reset);
    EXPECT(watch.SizeChanged(0, 0));  // the reset forgets the logged size
}

void TestSplitFovWords()
{
    std::puts("split FOV: every settings word maps to its weight and back; other text is the default; no word for the stock rule");
    using lss::platform::settings::kFovWeightWords;
    using lss::views::SplitFovWeightOf;
    using lss::views::SplitFovWordOf;
    using lss::views::split_fov_rules::kDefaultWeight;
    for (std::size_t step = 0; step < kFovWeightWords.size(); ++step)
    {
        const float weight = SplitFovWeightOf(kFovWeightWords[step]);
        EXPECT(std::fabs(weight - static_cast<float>(step) * 0.05f) < 1e-6f);
        EXPECT(SplitFovWordOf(weight) == kFovWeightWords[step]);
    }
    EXPECT(SplitFovWeightOf(L"0.50") == 0.5f && SplitFovWeightOf(L"1.00") == 1.0f && SplitFovWeightOf(L"0.00") == 0.0f);
    // Another word: the option's own default (the vertical weight's is 1.00, the horizontal's 0.50).
    EXPECT(SplitFovWeightOf(L"x", 1.0f) == 1.0f && SplitFovWeightOf(L"x") == 0.5f);
    EXPECT(SplitFovWeightOf(L"0.5") == kDefaultWeight);
    EXPECT(SplitFovWeightOf(L"") == kDefaultWeight);
    EXPECT(SplitFovWordOf(-1.0f).empty());
    EXPECT(SplitFovWordOf(1.5f).empty());
    EXPECT(SplitFovWordOf(std::numeric_limits<float>::quiet_NaN()).empty());
    EXPECT(SplitFovWordOf(0.51f) == L"0.50" && SplitFovWordOf(0.526f) == L"0.55");  // the nearest step
}

}  // namespace

int main()
{
    TestGameCameraIsRecognised();
    TestUnknownShapesAreRefused();
    TestRatiosAndWatch();
    TestSplitFovWords();

    return lss::test::Report();
}
