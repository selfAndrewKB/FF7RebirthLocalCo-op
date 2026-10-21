// Host-side tests for views::LooksLikeViewInfo and the split FOV, run without the game.
//
// The view info is a zeroed 0x6A0-byte block (FMinimalViewInfo's size in this build) given the
// shape the game's cameras produce: 16:9, bars on. A block with implausible numbers is not
// recognised, so it is never written.

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

bool Near(float value, float expected, float tolerance = 0.05f)
{
    return std::fabs(value - expected) <= tolerance;
}

/// The vertical FOV (degrees) a view `horizontal` degrees wide has at `aspect`.
float Vertical(float horizontal, float aspect)
{
    constexpr float kHalfRadians = 3.14159265f / 360.0f;
    return 2.0f * std::atan(std::tan(horizontal * kHalfRadians) / aspect) / (2.0f * kHalfRadians);
}

void TestHalfShape()
{
    std::puts("split FOV: a half's aspect and height ratio from the viewport and the layout");
    const auto topBottom = lss::views::HalfOf(2560, 1440, false);
    EXPECT(Near(topBottom.aspect, 32.0f / 9.0f, 0.001f) && topBottom.heightRatio == 0.5f);
    const auto sideBySide = lss::views::HalfOf(2560, 1440, true);
    EXPECT(Near(sideBySide.aspect, 8.0f / 9.0f, 0.001f) && sideBySide.heightRatio == 1.0f);
    EXPECT(lss::views::HalfOf(0, 1440, false).aspect == 0.0f);
    EXPECT(lss::views::HalfOf(2560, -1, true).heightRatio == 0.0f);
}

void TestSplitFovEnds()
{
    std::puts("split FOV: weight 0 keeps the single-player framing, weight 1 its scale");
    using lss::views::SplitFov;
    const auto topBottom  = lss::views::HalfOf(2560, 1440, false);
    const auto sideBySide = lss::views::HalfOf(2560, 1440, true);
    const float solo      = Vertical(60.0f, kSixteenByNine);  // the field camera: 60 wide, 36 tall
    EXPECT(Near(solo, 36.0f, 0.1f));
    // Framing: the solo vertical FOV in both layouts.
    EXPECT(Near(Vertical(SplitFov(60.0f, kSixteenByNine, topBottom, 0.0f), topBottom.aspect), solo));
    EXPECT(Near(Vertical(SplitFov(60.0f, kSixteenByNine, sideBySide, 0.0f), sideBySide.aspect), solo));
    // Scale: a half-height view shows the solo width, so a character is as many pixels tall.
    EXPECT(Near(SplitFov(60.0f, kSixteenByNine, topBottom, 1.0f), 60.0f));
    // Side by side the two agree: the solo vertical FOV whatever the weight.
    for (const float weight : {0.0f, 0.5f, 1.0f})
    {
        EXPECT(Near(Vertical(SplitFov(60.0f, kSixteenByNine, sideBySide, weight), sideBySide.aspect), solo));
    }
}

void TestSplitFovDefault()
{
    std::puts("split FOV: the default weight gives what was chosen by eye (top/bottom 78.5 x 25.9)");
    const auto  topBottom = lss::views::HalfOf(2560, 1440, false);
    const float wide      = lss::views::SplitFov(60.0f, kSixteenByNine, topBottom, lss::views::kDefaultSplitFovWeight);
    EXPECT(Near(wide, 78.5f, 0.1f));
    EXPECT(Near(Vertical(wide, topBottom.aspect), 25.9f, 0.1f));
}

void TestSplitFovRefusesBadInputs()
{
    std::puts("split FOV: unusable inputs return the camera's FOV unchanged");
    using lss::views::SplitFov;
    const auto half = lss::views::HalfOf(2560, 1440, false);
    EXPECT(SplitFov(60.0f, kSixteenByNine, lss::views::HalfShape{}, 0.5f) == 60.0f);
    EXPECT(SplitFov(60.0f, kSixteenByNine, half, 1.5f) == 60.0f);
    EXPECT(SplitFov(60.0f, kSixteenByNine, half, -0.1f) == 60.0f);
    EXPECT(SplitFov(60.0f, std::numeric_limits<float>::quiet_NaN(), half, 0.5f) == 60.0f);
    EXPECT(SplitFov(0.5f, kSixteenByNine, half, 0.5f) == 0.5f);
}

}  // namespace

int main()
{
    TestGameCameraIsRecognised();
    TestUnknownShapesAreRefused();
    TestHalfShape();
    TestSplitFovEnds();
    TestSplitFovDefault();
    TestSplitFovRefusesBadInputs();

    return lss::test::Report();
}
