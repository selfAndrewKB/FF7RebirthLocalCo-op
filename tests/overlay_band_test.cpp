// Host-side tests for hud::VisibleBand and MarkerHostScale, run without the game.
//
// The expected values are the ones measured or derived on 2026-09-25 (RE-p2-hud.md): a 3840x2160 UI
// image, a 2560x1440 window, halves of 2560x720 (top/bottom), 1280x1440 (side by side) and 1280x720
// (16:9 with bars). Solo must reproduce the engine's own host scale, 1.333.

#include "hud/overlay_band.h"

#include <cmath>
#include <cstdio>

namespace {

using lss::hud::IntRect;
using lss::hud::MarkerHostScale;
using lss::hud::VisibleBand;

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

bool Near(float value, float expected, float tolerance = 0.001f)
{
    return std::fabs(value - expected) <= tolerance;
}

void TestBands()
{
    std::puts("band: the part of a 16:9 image each view shape shows under the cover fit");
    EXPECT((VisibleBand(3840, 2160, 2560.0f, 1440.0f) == IntRect{0, 0, 3840, 2160}));   // solo
    EXPECT((VisibleBand(3840, 2160, 1280.0f, 720.0f) == IntRect{0, 0, 3840, 2160}));    // bars
    EXPECT((VisibleBand(3840, 2160, 2560.0f, 720.0f) == IntRect{0, 540, 3840, 1620}));  // top/bottom
    EXPECT((VisibleBand(3840, 2160, 1280.0f, 1440.0f) == IntRect{960, 0, 2880, 2160})); // side by side
    EXPECT((VisibleBand(1920, 1080, 2560.0f, 720.0f) == IntRect{0, 270, 1920, 810}));   // the smaller image
}

void TestHostScales()
{
    std::puts("host scale: solo is the engine's own; each split shape gets the value that keeps markers on objects");
    const auto solo = VisibleBand(3840, 2160, 2560.0f, 1440.0f);
    EXPECT(Near(MarkerHostScale(solo, 3840, 2160, 2560.0f, 1440.0f), 1440.0f / 1080.0f));
    const auto bars = VisibleBand(3840, 2160, 1280.0f, 720.0f);
    EXPECT(Near(MarkerHostScale(bars, 3840, 2160, 1280.0f, 720.0f), 720.0f / 1080.0f));
    const auto topBottom = VisibleBand(3840, 2160, 2560.0f, 720.0f);
    EXPECT(Near(MarkerHostScale(topBottom, 3840, 2160, 2560.0f, 720.0f), 2.0f / 3.0f));
    const auto sideBySide = VisibleBand(3840, 2160, 1280.0f, 1440.0f);
    EXPECT(Near(MarkerHostScale(sideBySide, 3840, 2160, 1280.0f, 1440.0f), 2.0f / 3.0f));
    // The image size does not change the answer for the same view: 1920x1080 gives the same scale.
    const auto small = VisibleBand(1920, 1080, 2560.0f, 720.0f);
    EXPECT(Near(MarkerHostScale(small, 1920, 1080, 2560.0f, 720.0f), 2.0f / 3.0f));
}

void TestMarkerFactorIsOne()
{
    std::puts("host scale: k * fit = 1 in every shape, i.e. a view pixel moves a marker by one view pixel");
    const float views[][2] = {{2560, 1440}, {1280, 720}, {2560, 720}, {1280, 1440}, {3440, 1440}};
    for (const auto& view : views)
    {
        const auto  band = VisibleBand(3840, 2160, view[0], view[1]);
        const float host = MarkerHostScale(band, 3840, 2160, view[0], view[1]);
        const float k    = lss::hud::ContainScaleFor1920x1080(static_cast<float>(band.Width()),
                                                              static_cast<float>(band.Height())) / host;
        const float fit  = std::fmax(view[0] / 3840.0f, view[1] / 2160.0f);
        EXPECT(Near(k * fit, 1.0f));
    }
}

void TestPlacement()
{
    std::puts("placement: the whole image gives the game's own matrix terms; a band lands unstretched at its Min");
    using lss::hud::PlaceInBand;
    // The game's matrix for a rect of W x H: (2/W, -2/H) and a constant (-1, 1) translation.
    const auto whole = PlaceInBand(IntRect{0, 0, 3840, 2160}, 3840, 2160);
    EXPECT(Near(whole.scaleX, 2.0f / 3840.0f, 1e-9f) && Near(whole.scaleY, -2.0f / 2160.0f, 1e-9f));
    EXPECT(Near(whole.translateX, -1.0f) && Near(whole.translateY, 1.0f));

    // Top/bottom: rows 540..1620. Layout row 0 lands on NDC y 0.5 (target row 540), row 1080 on -0.5.
    const auto band = PlaceInBand(IntRect{0, 540, 3840, 1620}, 3840, 2160);
    EXPECT(Near(band.scaleX, 2.0f / 3840.0f, 1e-9f) && Near(band.scaleY, -2.0f / 2160.0f, 1e-9f));
    EXPECT(Near(band.translateX, -1.0f) && Near(band.translateY, 0.5f));
    EXPECT(Near(1080.0f * band.scaleY + band.translateY, -0.5f));

    // Side by side: columns 960..2880. Layout column 0 lands on NDC x -0.5, column 1920 on 0.5.
    const auto columns = PlaceInBand(IntRect{960, 0, 2880, 2160}, 3840, 2160);
    EXPECT(Near(columns.translateX, -0.5f) && Near(columns.translateY, 1.0f));
    EXPECT(Near(1920.0f * columns.scaleX + columns.translateX, 0.5f));

    const auto none = PlaceInBand(IntRect{}, 3840, 2160);
    EXPECT(none.scaleX == 0.0f && none.scaleY == 0.0f);
}

void TestUnusable()
{
    std::puts("band and host scale: unusable sizes give empty results");
    EXPECT((VisibleBand(0, 2160, 2560.0f, 720.0f) == IntRect{}));
    EXPECT((VisibleBand(3840, 2160, 0.0f, 720.0f) == IntRect{}));
    EXPECT((VisibleBand(3840, 2160, std::nanf(""), 720.0f) == IntRect{}));
    EXPECT(MarkerHostScale(IntRect{}, 3840, 2160, 2560.0f, 720.0f) == 0.0f);
}

}  // namespace

int main()
{
    TestBands();
    TestHostScales();
    TestMarkerFactorIsOne();
    TestPlacement();
    TestUnusable();

    std::printf(g_failures == 0 ? "all tests passed\n" : "%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
