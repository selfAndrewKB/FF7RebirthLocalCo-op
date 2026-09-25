// Host-side tests for hud::SplitLayoutFor, run without the game.

#include "hud/viewport_placement.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

namespace {

using lss::hud::NearestAnchor;
using lss::hud::PlacementKind;
using lss::hud::Plausible;
using lss::hud::SplitLayoutFor;
using lss::hud::ViewportLayout;

int g_failures = 0;

#define EXPECT(condition)                                                                \
    do                                                                                   \
    {                                                                                    \
        if (!(condition))                                                                \
        {                                                                                \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);                 \
            ++g_failures;                                                                \
        }                                                                                \
    } while (false)

/// The stock layout of a root added to fill the viewport (as the game's HUD roots have it).
ViewportLayout Stretch()
{
    ViewportLayout layout;
    layout.anchors[2] = layout.anchors[3] = 1.0f;
    return layout;
}

/// A root the game placed at (x, y) with SetPositionInViewport.
ViewportLayout PointAt(float x, float y)
{
    ViewportLayout layout;
    layout.offsets[0] = x;
    layout.offsets[1] = y;
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
    const auto bottomRight = SplitLayoutFor(PlacementKind::Area, 1.0f, 1.0f, Stretch());
    EXPECT(bottomRight.has_value());
    float x = 0.0f, y = 0.0f;
    TopLeft(*bottomRight, 3840.0f, 1080.0f, 0.0f, 0.0f, x, y);  // top/bottom half's layout
    EXPECT(x == 1920.0f && y == 0.0f);
    TopLeft(*bottomRight, 1920.0f, 2160.0f, 0.0f, 0.0f, x, y);  // side by side half's layout
    EXPECT(x == 0.0f && y == 1080.0f);

    const auto topCentre = SplitLayoutFor(PlacementKind::Area, 0.5f, 0.0f, Stretch());
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
            TopLeft(*SplitLayoutFor(PlacementKind::Area, ax, ay, Stretch()), 1920.0f, 1080.0f, 0.0f, 0.0f, x, y);
            EXPECT(x == 0.0f && y == 0.0f);
        }
    }
    for (const auto& [px, py] : {std::pair{70.0f, 957.0f}, std::pair{960.0f, 40.0f}, std::pair{1800.0f, 500.0f}})
    {
        float x = 0.0f, y = 0.0f;
        TopLeft(*SplitLayoutFor(PlacementKind::Point, 0.0f, 0.0f, PointAt(px, py)), 1920.0f, 1080.0f, 381.0f, 41.0f,
                x, y);
        EXPECT(x == px && y == py);
    }
}

void TestPointKeepsCornerDistance()
{
    std::puts("placement: a point keeps its distance from the frame's nearest corner");
    const auto action = SplitLayoutFor(PlacementKind::Point, 0.0f, 0.0f, PointAt(70.0f, 957.0f));
    EXPECT(action.has_value());
    EXPECT(action->anchors[0] == 0.0f && action->anchors[1] == 1.0f);
    EXPECT(action->offsets[0] == 70.0f && action->offsets[1] == -123.0f);
    float x = 0.0f, y = 0.0f;
    TopLeft(*action, 1920.0f, 2160.0f, 381.0f, 41.0f, x, y);  // side by side
    EXPECT(x == 70.0f && y == 2037.0f);

    // Top right: 120 from the right edge, 40 from the top, in a 3840x1080 top/bottom layout.
    const auto topRight = SplitLayoutFor(PlacementKind::Point, 0.0f, 0.0f, PointAt(1800.0f, 40.0f));
    EXPECT(topRight->anchors[0] == 1.0f && topRight->anchors[1] == 0.0f);
    TopLeft(*topRight, 3840.0f, 1080.0f, 100.0f, 30.0f, x, y);
    EXPECT(x == 3720.0f && y == 40.0f);

    // Centre: a point in the middle third keeps its offset from the centre.
    const auto centre = SplitLayoutFor(PlacementKind::Point, 0.0f, 0.0f, PointAt(900.0f, 500.0f));
    EXPECT(centre->anchors[0] == 0.5f && centre->anchors[1] == 0.5f);
    TopLeft(*centre, 1920.0f, 2160.0f, 100.0f, 30.0f, x, y);
    EXPECT(x == 900.0f && y == 1040.0f);
}

void TestRejected()
{
    std::puts("placement: a point not anchored at (0,0), or an implausible layout, is left alone");
    EXPECT(!SplitLayoutFor(PlacementKind::Point, 0.0f, 0.0f, Stretch()).has_value());
    ViewportLayout broken = Stretch();
    broken.offsets[0]     = std::numeric_limits<float>::quiet_NaN();
    EXPECT(!Plausible(broken));
    EXPECT(!SplitLayoutFor(PlacementKind::Area, 0.0f, 1.0f, broken).has_value());
    ViewportLayout wild = Stretch();
    wild.anchors[1]     = 3.0f;
    EXPECT(!SplitLayoutFor(PlacementKind::Area, 0.0f, 1.0f, wild).has_value());
}

}  // namespace

int main()
{
    TestNearestAnchor();
    TestAreaCorners();
    TestSoloUnchanged();
    TestPointKeepsCornerDistance();
    TestRejected();

    std::printf(g_failures == 0 ? "all tests passed\n" : "%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
