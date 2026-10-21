// Host-side tests for views::ForwardOf, CoveringFov16x9, StreamFov and CameraOf, run without the game.

#include "views/stream_view_math.h"

#include "expect.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

using lss::views::CameraOf;
using lss::views::ForwardOf;

bool Near(float a, float b)
{
    return std::fabs(a - b) < 1e-4f;
}

void TestForward()
{
    std::puts("stream view: forward vectors follow FRotator::Vector");
    float f[3];
    ForwardOf(0.0f, 0.0f, f);
    EXPECT(Near(f[0], 1.0f) && Near(f[1], 0.0f) && Near(f[2], 0.0f));
    ForwardOf(0.0f, 90.0f, f);
    EXPECT(Near(f[0], 0.0f) && Near(f[1], 1.0f) && Near(f[2], 0.0f));
    ForwardOf(90.0f, 0.0f, f);
    EXPECT(Near(f[0], 0.0f) && Near(f[2], 1.0f));
    ForwardOf(-30.0f, 45.0f, f);
    EXPECT(Near(f[0] * f[0] + f[1] * f[1] + f[2] * f[2], 1.0f) && f[2] < 0.0f);
}

void TestCoveringFov()
{
    std::puts("stream view: a 16:9 frustum is widened only for views narrower than 16:9");
    using lss::views::CoveringFov16x9;
    EXPECT(Near(CoveringFov16x9(90.0f, 16.0f / 9.0f), 90.0f));
    EXPECT(Near(CoveringFov16x9(90.0f, 32.0f / 9.0f), 90.0f));  // top/bottom half: wider, unchanged
    // Side by side (8:9): the same vertical extent at 16:9 is twice the half-width tangent.
    const float expected = 2.0f * std::atan(2.0f * std::tan(3.14159265f / 4.0f)) * 180.0f / 3.14159265f;
    EXPECT(Near(CoveringFov16x9(90.0f, 8.0f / 9.0f), expected));
    EXPECT(Near(CoveringFov16x9(90.0f, 0.0f), 90.0f));
}

void TestStreamFov()
{
    std::puts("stream view: a wanted FOV replaces the covering one, capped; none keeps it");
    using lss::views::kMaxStreamFov;
    using lss::views::StreamFov;
    EXPECT(Near(StreamFov(78.5f, 0.0f), 78.5f));
    EXPECT(Near(StreamFov(78.5f, -5.0f), 78.5f));
    EXPECT(Near(StreamFov(78.5f, std::nanf("")), 78.5f));
    EXPECT(Near(StreamFov(78.5f, 150.0f), 150.0f));
    EXPECT(Near(StreamFov(78.5f, 60.0f), 60.0f));
    EXPECT(Near(StreamFov(78.5f, 179.0f), kMaxStreamFov));
}

void Put(unsigned char* view, std::size_t offset, float value)
{
    std::memcpy(view + offset, &value, sizeof(value));
}

void TestCameraOf()
{
    std::puts("stream view: a view info's camera, refused when a value is unusable");
    unsigned char view[0x40] = {};
    Put(view, 0x00, 100.0f);
    Put(view, 0x04, -200.0f);
    Put(view, 0x08, 300.0f);
    Put(view, 0x0C, 0.0f);   // pitch
    Put(view, 0x10, 90.0f);  // yaw
    Put(view, 0x18, 70.0f);  // FOV
    const auto camera = CameraOf(reinterpret_cast<const std::byte*>(view));
    EXPECT(camera && Near(camera->location[1], -200.0f) && Near(camera->direction[1], 1.0f) && Near(camera->fov, 70.0f));

    Put(view, 0x18, 0.0f);
    EXPECT(!CameraOf(reinterpret_cast<const std::byte*>(view)));
    Put(view, 0x18, 70.0f);
    Put(view, 0x04, std::nanf(""));
    EXPECT(!CameraOf(reinterpret_cast<const std::byte*>(view)));
    EXPECT(!CameraOf(nullptr));
}

}  // namespace

int main()
{
    TestForward();
    TestCoveringFov();
    TestStreamFov();
    TestCameraOf();
    return lss::test::Report();
}
