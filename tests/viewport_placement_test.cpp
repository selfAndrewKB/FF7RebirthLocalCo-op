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

void TestEffectViewport()
{
    std::puts("effect viewport: a column's UI particles get a box of the tick layout's height at its band's scale");
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
    // With the widget drawn at a HUD size about its corner: the box that many times as tall, its centre moved with the
    // widget's points (the corner stays put). The side-by-side right half at 1.5: the corner at (3840, 2160), the tick's
    // centre 960 units left of it in the design, so 1440 px left at 1.5; the box 3240 px tall.
    EXPECT(near(EffectViewport(LayoutSize{1920.0f, 2160.0f}, LayoutSize{1920.0f, 2160.0f}, IntRect{1920, 0, 3840, 2160}, 3840,
                               2160, corner, 1.5f),
                2400.0f / 3840.0f, 1.0f, 1.5f));
    // The lower stacked half at 0.75: the centre 540 units above the corner in the design, so 405 px above the image's
    // bottom; the box 810 px tall.
    EXPECT(near(EffectViewport(LayoutSize{3840.0f, 1080.0f}, LayoutSize{3840.0f, 1080.0f}, IntRect{0, 1080, 3840, 2160}, 3840,
                               2160, corner, 0.75f),
                1.0f, 1755.0f / 2160.0f, 0.375f));
    // Size 1 is the game's own box.
    EXPECT(EffectViewport(LayoutSize{2581.0f, 1080.0f}, LayoutSize{1920.0f, 1080.0f}, IntRect{0, 0, 3840, 2160}, 3840, 2160,
                          corner, 1.0f) ==
           EffectViewport(LayoutSize{2581.0f, 1080.0f}, LayoutSize{1920.0f, 1080.0f}, IntRect{0, 0, 3840, 2160}, 3840, 2160,
                          corner));
    // Three players with player 1 tall on the left: a 16:9 view's box is two images tall at size 1 and 2.5 at 1.25 (the
    // largest box any column gets: hud/passes/ui_effect_pass accepts up to the largest size's). Two px per unit; the
    // tick's centre 960 units left of the corner in the design, so 1920 px left at 1, 2400 px at 1.25.
    EXPECT(near(EffectViewport(LayoutSize{1920.0f, 2160.0f}, LayoutSize{1920.0f, 1080.0f}, IntRect{0, 0, 3840, 2160}, 3840,
                               2160, corner),
                0.5f, 1.0f, 2.0f));
    EXPECT(near(EffectViewport(LayoutSize{1920.0f, 2160.0f}, LayoutSize{1920.0f, 1080.0f}, IntRect{0, 0, 3840, 2160}, 3840,
                               2160, corner, 1.25f),
                1440.0f / 3840.0f, 1.0f, 2.5f));
    // A scale centre at the corner is the corner's box.
    EXPECT(EffectViewport(LayoutSize{1920.0f, 2160.0f}, LayoutSize{1920.0f, 1080.0f}, IntRect{0, 0, 3840, 2160}, 3840, 2160,
                          corner, 1.25f, corner) ==
           EffectViewport(LayoutSize{1920.0f, 2160.0f}, LayoutSize{1920.0f, 1080.0f}, IntRect{0, 0, 3840, 2160}, 3840, 2160,
                          corner, 1.25f));
    // Converted against the 1920x1080 frame (the mod's answer to the conversion), a gauge point d lands where the column's
    // HUD draws it, and one half-height of the box is one half-height of the frame at the band's scale: the sprites at
    // the HUD's size. The game's point is (d - (960, 540)) / 540; the box draws it at centre + point * height / 2.
    const LayoutSize  frame{1920.0f, 1080.0f};
    const LayoutPoint gauge{1508.0f, 620.0f};
    const auto        drawnAt = [&](const std::optional<EffectBox>& b, std::int32_t imageW, std::int32_t imageH) {
        const float half = b->height * static_cast<float>(imageH) / 2.0f;
        return LayoutPoint{b->centreX * static_cast<float>(imageW) + (gauge.x - 960.0f) / 540.0f * half,
                           b->centreY * static_cast<float>(imageH) + (gauge.y - 540.0f) / 540.0f * half};
    };
    const auto at = [](const LayoutPoint& a, float x, float y) { return std::fabs(a.x - x) < 0.05f && std::fabs(a.y - y) < 0.05f; };
    // A tall column (the right half, one px per unit) at size 1: the gauge 1080 units below its design place, so at
    // (1920 + 1508, 620 + 1080); the box half the image tall (540 px a half-height, not the tall layout's 1080).
    const auto tall = EffectViewport(frame, LayoutSize{1920.0f, 2160.0f}, IntRect{1920, 0, 3840, 2160}, 3840, 2160, corner);
    EXPECT(near(tall, 0.75f, 0.75f, 0.5f) && at(drawnAt(tall, 3840, 2160), 3428.0f, 1700.0f));
    // The summon gauges' scale height (795 of 1080) at x1.50 in a 16:9 column (two px per unit): the point drawn at
    // C + 1.5 (d - C) with C = (1920, 795), so at (1302, 532.5) units, (2604, 1065) px; the box 1.5 images tall.
    const LayoutPoint summon{1.0f, 795.0f / 1080.0f};
    const auto        sized = EffectViewport(frame, frame, IntRect{0, 0, 3840, 2160}, 3840, 2160, corner, 1.5f, summon);
    EXPECT(near(sized, 0.25f, 825.0f / 2160.0f, 1.5f) && at(drawnAt(sized, 3840, 2160), 2604.0f, 1065.0f));
    // The same in the side-by-side right half (one px per unit, the area's corner at the image's bottom right): the scale
    // height 285 units above the bottom, the point at (1920 + 1302, 1080 + 532.5).
    const auto sizedTall = EffectViewport(frame, LayoutSize{1920.0f, 2160.0f}, IntRect{1920, 0, 3840, 2160}, 3840, 2160,
                                          corner, 1.5f, summon);
    EXPECT(sizedTall && at(drawnAt(sizedTall, 3840, 2160), 3222.0f, 1612.5f));
    // Unknowns.
    EXPECT(!EffectViewport(frame, frame, IntRect{0, 0, 3840, 2160}, 3840, 2160, corner, 1.0f,
                           LayoutPoint{1.0f, std::numeric_limits<float>::quiet_NaN()}));
    EXPECT(!EffectViewport(LayoutSize{1920.0f, 1080.0f}, LayoutSize{1920.0f, 1080.0f}, IntRect{0, 0, 3840, 2160}, 3840, 2160,
                           corner, 0.0f));
    EXPECT(!EffectViewport(LayoutSize{1920.0f, 1080.0f}, LayoutSize{1920.0f, 1080.0f}, IntRect{0, 0, 3840, 2160}, 3840, 2160,
                           corner, std::numeric_limits<float>::quiet_NaN()));
    EXPECT(!EffectViewport(LayoutSize{}, LayoutSize{1920.0f, 1080.0f}, IntRect{0, 0, 3840, 2160}, 3840, 2160, corner));
    EXPECT(!EffectViewport(LayoutSize{1920.0f, 1080.0f}, LayoutSize{}, IntRect{0, 0, 3840, 2160}, 3840, 2160, corner));
    EXPECT(!EffectViewport(LayoutSize{1920.0f, 1080.0f}, LayoutSize{1920.0f, 1080.0f}, IntRect{}, 3840, 2160, corner));
    EXPECT(!EffectViewport(LayoutSize{1920.0f, 1080.0f}, LayoutSize{1920.0f, 1080.0f}, IntRect{0, 0, 3840, 2160}, 0, 2160,
                           corner));
    EXPECT(!EffectViewport(LayoutSize{1920.0f, 1080.0f}, LayoutSize{1920.0f, 1080.0f}, IntRect{0, 0, 3840, 2160}, 3840, 2160,
                           LayoutPoint{std::numeric_limits<float>::quiet_NaN(), 1.0f}));
}

void TestCornerPivot()
{
    std::puts("corner pivot: a widget scaled about its area's corner with its render translation scaled too");
    using lss::hud::CornerPivot;
    // Slate's draw of a widget point p: pivot + scale * (p - pivot) + translation (pivot in the widget's units).
    const auto drawn = [](const LayoutPoint& pivot, float scale, const LayoutPoint& translation, const LayoutPoint& p) {
        const LayoutPoint at{pivot.x * 1920.0f, pivot.y * 1080.0f};
        return LayoutPoint{at.x + scale * (p.x - at.x) + translation.x, at.y + scale * (p.y - at.y) + translation.y};
    };
    const auto close = [](const LayoutPoint& a, const LayoutPoint& b) {
        return std::fabs(a.x - b.x) < 0.01f && std::fabs(a.y - b.y) < 0.01f;
    };
    // No translation: the corner itself.
    EXPECT((CornerPivot({1.0f, 1.0f}, {}, 1920.0f, 1080.0f) == LayoutPoint{1.0f, 1.0f}));
    EXPECT((CornerPivot({0.5f, 0.0f}, {}, 1920.0f, 1080.0f) == LayoutPoint{0.5f, 0.0f}));
    // The summon gauges (their own translation 1508,620): drawn where the game puts them, scaled about the corner, for two
    // points and two sizes; the corner-only pivot misses by (scale - 1) times the translation.
    const LayoutPoint translation{1508.0f, 620.0f};
    const LayoutPoint corner{1920.0f, 1080.0f};
    const auto        pivot = CornerPivot({1.0f, 1.0f}, translation, 1920.0f, 1080.0f);
    EXPECT(pivot.has_value());
    for (const float scale : {1.5f, 0.75f})
    {
        for (const LayoutPoint p : {LayoutPoint{300.0f, -180.0f}, LayoutPoint{400.0f, 0.0f}})
        {
            const LayoutPoint placed{p.x + translation.x, p.y + translation.y};  // where the game draws it at 1
            const LayoutPoint wanted{corner.x + scale * (placed.x - corner.x), corner.y + scale * (placed.y - corner.y)};
            EXPECT(pivot && close(drawn(*pivot, scale, translation, p), wanted));
            EXPECT(!close(drawn({1.0f, 1.0f}, scale, translation, p), wanted));
        }
    }
    // About any point of the area, not only a corner: the summon gauges' scale height (795 of 1080 at the right edge).
    const LayoutPoint centre{1920.0f, 795.0f};
    const auto        midPivot = CornerPivot({1.0f, 795.0f / 1080.0f}, translation, 1920.0f, 1080.0f);
    for (const LayoutPoint p : {LayoutPoint{0.0f, 0.0f}, LayoutPoint{332.0f, 70.0f}})
    {
        const LayoutPoint placed{p.x + translation.x, p.y + translation.y};
        const LayoutPoint wanted{centre.x + 1.5f * (placed.x - centre.x), centre.y + 1.5f * (placed.y - centre.y)};
        EXPECT(midPivot && close(drawn(*midPivot, 1.5f, translation, p), wanted));
    }
    // An empty area or an unreadable translation: none.
    EXPECT(!CornerPivot({1.0f, 1.0f}, translation, 0.0f, 1080.0f));
    EXPECT(!CornerPivot({1.0f, 1.0f}, LayoutPoint{std::numeric_limits<float>::quiet_NaN(), 0.0f}, 1920.0f, 1080.0f));
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
    TestEffectViewport();
    TestCornerPivot();
    TestRejected();

    return lss::test::Report();
}
