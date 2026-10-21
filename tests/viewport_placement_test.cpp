// Host-side tests for hud::SplitLayoutFor and the field-action icons' base rule, run without the game.

#include "hud/viewport_placement.h"

#include "expect.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

namespace {

using lss::hud::NearestAnchor;
using lss::hud::AnchorOf;
using lss::hud::KeepDistanceFromAnchor;
using lss::hud::LayoutPoint;
using lss::hud::Plausible;
using lss::hud::SplitLayoutFor;
using lss::hud::ViewportLayout;

/// The stock layout of a root added to fill the viewport (as the game's HUD roots have it).
ViewportLayout Stretch()
{
    ViewportLayout layout;
    layout.anchors[2] = layout.anchors[3] = 1.0f;
    return layout;
}

/// Where a layout puts its widget's top-left corner in a layout of size (w, h), for a widget of
/// size (sw, sh) when the offsets give no size (the UMG viewport rule: anchor point + offset -
/// alignment * size).
void TopLeft(const ViewportLayout& l, float w, float h, float sw, float sh, float& x, float& y)
{
    const float width  = l.offsets[2] != 0.0f ? l.offsets[2] : sw;
    const float height = l.offsets[3] != 0.0f ? l.offsets[3] : sh;
    x = l.anchors[0] * w + l.offsets[0] - l.alignment[0] * width;
    y = l.anchors[1] * h + l.offsets[1] - l.alignment[1] * height;
}

void TestNearestAnchor()
{
    std::puts("placement: the nearest anchor is by thirds of the frame");
    EXPECT(NearestAnchor(70.0f, 1920.0f) == 0.0f);
    EXPECT(NearestAnchor(960.0f, 1920.0f) == 0.5f);
    EXPECT(NearestAnchor(1700.0f, 1920.0f) == 1.0f);
    EXPECT(NearestAnchor(957.0f, 1080.0f) == 1.0f);
    EXPECT(NearestAnchor(40.0f, 1080.0f) == 0.0f);
}

void TestAreaCorners()
{
    std::puts("placement: an area root gets a 1920x1080 area at its corner of the half");
    const auto bottomRight = SplitLayoutFor(1.0f, 1.0f, Stretch());
    EXPECT(bottomRight.has_value());
    float x = 0.0f, y = 0.0f;
    TopLeft(*bottomRight, 3840.0f, 1080.0f, 0.0f, 0.0f, x, y);  // top/bottom half's layout
    EXPECT(x == 1920.0f && y == 0.0f);
    TopLeft(*bottomRight, 1920.0f, 2160.0f, 0.0f, 0.0f, x, y);  // side by side half's layout
    EXPECT(x == 0.0f && y == 1080.0f);

    const auto topCentre = SplitLayoutFor(0.5f, 0.0f, Stretch());
    TopLeft(*topCentre, 3840.0f, 1080.0f, 0.0f, 0.0f, x, y);
    EXPECT(x == 960.0f && y == 0.0f);
}

void TestSoloUnchanged()
{
    std::puts("placement: in a 1920x1080 layout every placement lands where the game's own does");
    for (const float ax : {0.0f, 0.5f, 1.0f})
    {
        for (const float ay : {0.0f, 1.0f})
        {
            float x = 1.0f, y = 1.0f;
            TopLeft(*SplitLayoutFor(ax, ay, Stretch()), 1920.0f, 1080.0f, 0.0f, 0.0f, x, y);
            EXPECT(x == 0.0f && y == 0.0f);
        }
    }
    for (const auto& [px, py] : {std::pair{70.0f, 957.0f}, std::pair{960.0f, 40.0f}, std::pair{1800.0f, 500.0f}})
    {
        const LayoutPoint point{px, py};
        EXPECT(KeepDistanceFromAnchor(point, AnchorOf(point), 1920.0f, 1080.0f) == point);
    }
}

void TestIconBaseKeepsCornerDistance()
{
    std::puts("icon base: the field-action icons' base keeps its distance from the frame's nearest corner");
    const LayoutPoint base{70.0f, 957.0f};  // the Blueprint default of FieldAction_Command_C (slot 0)
    const LayoutPoint anchor = AnchorOf(base);
    EXPECT(anchor == (LayoutPoint{0.0f, 1.0f}));  // bottom left
    EXPECT(KeepDistanceFromAnchor(base, anchor, 1920.0f, 2160.0f) == (LayoutPoint{70.0f, 2037.0f}));  // side by side
    EXPECT(KeepDistanceFromAnchor(base, anchor, 3840.0f, 1080.0f) == base);  // top/bottom: same height, left edge

    // Other corners and the centre, for any icon class whose default sits there.
    EXPECT(AnchorOf(LayoutPoint{1800.0f, 40.0f}) == (LayoutPoint{1.0f, 0.0f}));
    EXPECT(KeepDistanceFromAnchor(LayoutPoint{1800.0f, 40.0f}, LayoutPoint{1.0f, 0.0f}, 3840.0f, 1080.0f) ==
           (LayoutPoint{3720.0f, 40.0f}));
    EXPECT(AnchorOf(LayoutPoint{900.0f, 500.0f}) == (LayoutPoint{0.5f, 0.5f}));
    EXPECT(KeepDistanceFromAnchor(LayoutPoint{900.0f, 500.0f}, LayoutPoint{0.5f, 0.5f}, 1920.0f, 2160.0f) ==
           (LayoutPoint{900.0f, 1040.0f}));

    // A row left at its side-by-side place is outside the solo layout (not painted, so never ticked).
    EXPECT(lss::hud::OutsideLayout(LayoutPoint{70.0f, 2037.0f}, 1920.0f, 1080.0f));
    EXPECT(!lss::hud::OutsideLayout(LayoutPoint{70.0f, 957.0f}, 1920.0f, 1080.0f));
    EXPECT(!lss::hud::OutsideLayout(LayoutPoint{70.0f, 957.0f}, 1920.0f, 2160.0f));
    EXPECT(lss::hud::OutsideLayout(LayoutPoint{-5.0f, 957.0f}, 1920.0f, 1080.0f));
    EXPECT(lss::hud::OutsideLayout(LayoutPoint{std::numeric_limits<float>::quiet_NaN(), 0.0f}, 1920.0f, 1080.0f));
    EXPECT(!lss::hud::OutsideLayout(LayoutPoint{70.0f, 2037.0f}, 0.0f, 0.0f));  // unknown layout

    // An unknown layout leaves the base as it is.
    EXPECT(KeepDistanceFromAnchor(base, anchor, 0.0f, 0.0f) == base);
    EXPECT(KeepDistanceFromAnchor(base, anchor, std::numeric_limits<float>::quiet_NaN(), 1080.0f) == base);
}
void TestRejected()
{
    std::puts("placement: an implausible layout is left alone");
    ViewportLayout broken = Stretch();
    broken.offsets[0]     = std::numeric_limits<float>::quiet_NaN();
    EXPECT(!Plausible(broken));
    EXPECT(!SplitLayoutFor(0.0f, 1.0f, broken).has_value());
    ViewportLayout wild = Stretch();
    wild.anchors[1]     = 3.0f;
    EXPECT(!SplitLayoutFor(0.0f, 1.0f, wild).has_value());
}

}  // namespace

int main()
{
    TestNearestAnchor();
    TestAreaCorners();
    TestSoloUnchanged();
    TestIconBaseKeepsCornerDistance();
    TestRejected();

    return lss::test::Report();
}
