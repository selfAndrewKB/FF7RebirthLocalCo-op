// Host-side tests for hud::SplitLayoutFor and the field-action icons' base rule, run without the game.

#include "hud/layout/viewport_placement.h"

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
void TestEffectPoint()
{
    std::puts("effect point: a UI effect's point maps through the band it is drawn beside; a full view leaves it");
    using lss::hud::EffectPointFor;
    using lss::hud::IntRect;
    using lss::hud::LayoutSize;
    const LayoutPoint head{1546.0f, 657.0f};  // the summon gauge's head glow (facts/hud.h)
    // A 1920x1080 view: the layout fills the image.
    EXPECT(EffectPointFor(head, LayoutSize{1920.0f, 1080.0f}, IntRect{0, 0, 1920, 1080}, 1920, 1080) == head);
    // A top/bottom half (a 3840x1080 layout drawn into the middle band): the bar's corner shift puts the head at
    // (3466, 657); the effect must be told the image position of that, in layout units: (2693, 598.5).
    const LayoutPoint moved{3466.0f, 657.0f};
    const auto        top = EffectPointFor(moved, LayoutSize{3840.0f, 1080.0f}, IntRect{0, 270, 1920, 810}, 1920, 1080);
    EXPECT(std::fabs(top.x - 2693.0f) < 0.01f && std::fabs(top.y - 598.5f) < 0.01f);
    // A side-by-side half (a 1920x2160 layout drawn into the centre band): the two maps cancel, the moved point stands.
    const LayoutPoint side{1546.0f, 1737.0f};
    EXPECT(EffectPointFor(side, LayoutSize{1920.0f, 2160.0f}, IntRect{480, 0, 1440, 1080}, 1920, 1080) == side);
    // Unknown sizes leave the point.
    EXPECT(EffectPointFor(moved, LayoutSize{}, IntRect{0, 270, 1920, 810}, 1920, 1080) == moved);
    EXPECT(EffectPointFor(moved, LayoutSize{3840.0f, 1080.0f}, IntRect{}, 1920, 1080) == moved);
    EXPECT(EffectPointFor(moved, LayoutSize{3840.0f, 1080.0f}, IntRect{0, 270, 1920, 810}, 0, 0) == moved);
}

void TestEffectSize()
{
    std::puts("placement: a UI effect's size factor is the band's drawing scale over single player's");
    using lss::hud::EffectSizeFactor;
    using lss::hud::IntRect;
    using lss::hud::LayoutSize;
    // A 1920x1080 view fills a 1920x1080 image: as single player.
    EXPECT(std::fabs(EffectSizeFactor(LayoutSize{1920.0f, 1080.0f}, IntRect{0, 0, 1920, 1080}, 1080) - 1.0f) < 1e-6f);
    // A 3840x2160 image under a single view: the band is twice the layout, and so is single player's: 1.
    EXPECT(std::fabs(EffectSizeFactor(LayoutSize{1920.0f, 1080.0f}, IntRect{0, 0, 3840, 2160}, 2160) - 1.0f) < 1e-6f);
    // A side-by-side half (a 1920x2160 layout into the centre band of a 1920x1080 image): the bar is drawn at half
    // its single-player scale, so the effect needs half its size.
    EXPECT(std::fabs(EffectSizeFactor(LayoutSize{1920.0f, 2160.0f}, IntRect{480, 0, 1440, 1080}, 1080) - 0.5f) < 1e-6f);
    // A top/bottom half (a 3840x1080 layout into the middle band): half as well.
    EXPECT(std::fabs(EffectSizeFactor(LayoutSize{3840.0f, 1080.0f}, IntRect{0, 270, 1920, 810}, 1080) - 0.5f) < 1e-6f);
    // Unknown sizes: 1.
    EXPECT(EffectSizeFactor(LayoutSize{}, IntRect{0, 270, 1920, 810}, 1080) == 1.0f);
    EXPECT(EffectSizeFactor(LayoutSize{3840.0f, 1080.0f}, IntRect{}, 1080) == 1.0f);
    EXPECT(EffectSizeFactor(LayoutSize{3840.0f, 1080.0f}, IntRect{0, 270, 1920, 810}, 0) == 1.0f);
}

void TestEffectViewport()
{
    using lss::hud::EffectBox;
    using lss::hud::EffectViewport;
    using lss::hud::IntRect;
    using lss::hud::LayoutSize;
    const LayoutPoint corner{1.0f, 1.0f};
    const auto        near = [](const std::optional<EffectBox>& b, float x, float y, float h) {
        return b && std::fabs(b->centreX - x) < 1e-4f && std::fabs(b->centreY - y) < 1e-4f && std::fabs(b->height - h) < 1e-4f;
    };
    // A 16:9 column while the tick's layout is 16:9 too: the whole image (the engine's own viewport).
    EXPECT(near(EffectViewport(LayoutSize{1920.0f, 1080.0f}, LayoutSize{1920.0f, 1080.0f}, IntRect{0, 0, 3840, 2160}, 3840,
                               2160, corner),
                0.5f, 0.5f, 1.0f));
    // The row's ultrawide (player 1's, the tick's own layout): its band's height; the centre moved right by the 661
    // units the corner-anchored gauge stands further right than in the frame, at the band's 1607/1080 px per unit.
    EXPECT(near(EffectViewport(LayoutSize{2581.0f, 1080.0f}, LayoutSize{2581.0f, 1080.0f}, IntRect{0, 276, 3840, 1883}, 3840,
                               2160, corner),
                (2581.0f / 2.0f + 661.0f) * (1607.0f / 1080.0f) / 3840.0f, (276.0f + 1883.0f) / 2.0f / 2160.0f,
                1607.0f / 2160.0f));
    // A 16:9 column beside it: the gauge at its design place, the tick's centre at 1290.5 units = 2581 px of 3840.
    EXPECT(near(EffectViewport(LayoutSize{2581.0f, 1080.0f}, LayoutSize{1920.0f, 1080.0f}, IntRect{0, 0, 3840, 2160}, 3840,
                               2160, corner),
                2581.0f / 3840.0f, 0.5f, 1.0f));
    // Two stacked halves of one shape (3840x1080 units each, one px per unit): the gauge 1920 units right of its design
    // place, so the centre at the image's right edge; the lower band's middle height, the band's height.
    EXPECT(near(EffectViewport(LayoutSize{3840.0f, 1080.0f}, LayoutSize{3840.0f, 1080.0f}, IntRect{0, 1080, 3840, 2160}, 3840,
                               2160, corner),
                1.0f, 0.75f, 0.5f));
    // Two side-by-side halves (1920x2160 units each, the tick's too): the gauge 1080 units below its design place, so
    // the right half's centre at its band's middle width and the image's bottom; the box as tall as the band.
    EXPECT(near(EffectViewport(LayoutSize{1920.0f, 2160.0f}, LayoutSize{1920.0f, 2160.0f}, IntRect{1920, 0, 3840, 2160}, 3840,
                               2160, corner),
                0.75f, 1.0f, 1.0f));
    // A tall column (1920x2160 units in the image's left half) while the tick's layout is 16:9: the game's half-height
    // is 540 units, the band's unit one px, so the box is 1080 px tall, centred 960 px in and 540 + 1080 px down.
    EXPECT(near(EffectViewport(LayoutSize{1920.0f, 1080.0f}, LayoutSize{1920.0f, 2160.0f}, IntRect{0, 0, 1920, 2160}, 3840,
                               2160, corner),
                0.25f, 0.75f, 0.5f));
    // A centre anchor and a left anchor move the centre by half the extra width and by none of it.
    EXPECT(near(EffectViewport(LayoutSize{1920.0f, 1080.0f}, LayoutSize{2581.0f, 1080.0f}, IntRect{0, 276, 3840, 1883}, 3840,
                               2160, LayoutPoint{0.5f, 0.0f}),
                (960.0f + 330.5f) * (1607.0f / 1080.0f) / 3840.0f, (276.0f + 540.0f * (1607.0f / 1080.0f)) / 2160.0f,
                1607.0f / 2160.0f));
    EXPECT(near(EffectViewport(LayoutSize{1920.0f, 1080.0f}, LayoutSize{2581.0f, 1080.0f}, IntRect{0, 276, 3840, 1883}, 3840,
                               2160, LayoutPoint{}),
                960.0f * (1607.0f / 1080.0f) / 3840.0f, (276.0f + 540.0f * (1607.0f / 1080.0f)) / 2160.0f, 1607.0f / 2160.0f));
    // Unknowns.
    EXPECT(!EffectViewport(LayoutSize{}, LayoutSize{1920.0f, 1080.0f}, IntRect{0, 0, 3840, 2160}, 3840, 2160, corner));
    EXPECT(!EffectViewport(LayoutSize{1920.0f, 1080.0f}, LayoutSize{}, IntRect{0, 0, 3840, 2160}, 3840, 2160, corner));
    EXPECT(!EffectViewport(LayoutSize{1920.0f, 1080.0f}, LayoutSize{1920.0f, 1080.0f}, IntRect{}, 3840, 2160, corner));
    EXPECT(!EffectViewport(LayoutSize{1920.0f, 1080.0f}, LayoutSize{1920.0f, 1080.0f}, IntRect{0, 0, 3840, 2160}, 0, 2160,
                           corner));
    EXPECT(!EffectViewport(LayoutSize{1920.0f, 1080.0f}, LayoutSize{1920.0f, 1080.0f}, IntRect{0, 0, 3840, 2160}, 3840, 2160,
                           LayoutPoint{std::numeric_limits<float>::quiet_NaN(), 1.0f}));
}

void TestReferenceColumn()
{
    std::puts("placement: a once-per-screen thing follows the layout shape most columns share, player 1's on a tie");
    using lss::hud::LayoutSize;
    using lss::hud::ReferenceColumn;
    // The user's row: an ultrawide column 0 (a 2581x1080 band layout) beside two 16:9 columns: the 16:9 shape wins.
    const LayoutSize row[3] = {{2581.0f, 1080.0f}, {1920.0f, 1080.0f}, {1920.0f, 1080.0f}};
    EXPECT(ReferenceColumn(row, 3) == 1);
    // Three players in one window: a wide top view (3840x1080) over two 16:9 quarters: the quarters win.
    const LayoutSize window[3] = {{3840.0f, 1080.0f}, {1920.0f, 1080.0f}, {1920.0f, 1080.0f}};
    EXPECT(ReferenceColumn(window, 3) == 1);
    // Two stacked halves: one shape, column 0.
    const LayoutSize stacked[2] = {{3840.0f, 1080.0f}, {3840.0f, 1080.0f}};
    EXPECT(ReferenceColumn(stacked, 2) == 0);
    // Two quarters whose bands differ by a pixel of rounding (two layout units) are still one shape.
    const LayoutSize jitter[3] = {{3840.0f, 1080.0f}, {1920.0f, 1080.0f}, {1922.0f, 1080.0f}};
    EXPECT(ReferenceColumn(jitter, 3) == 1);
    // Three different shapes: a tie at one each, player 1's.
    const LayoutSize mixed[3] = {{2581.0f, 1080.0f}, {3840.0f, 1080.0f}, {1920.0f, 1080.0f}};
    EXPECT(ReferenceColumn(mixed, 3) == 0);
    // Unknown columns count for nothing; none known: 0.
    const LayoutSize sparse[3] = {{}, {1920.0f, 1080.0f}, {1920.0f, 1080.0f}};
    EXPECT(ReferenceColumn(sparse, 3) == 1);
    const LayoutSize none[2] = {{}, {}};
    EXPECT(ReferenceColumn(none, 2) == 0);
    EXPECT(ReferenceColumn(nullptr, 0) == 0);
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
    TestEffectPoint();
    TestEffectSize();
    TestEffectViewport();
    TestReferenceColumn();
    TestRejected();

    return lss::test::Report();
}
