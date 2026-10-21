// Host-side tests for platform/angles, run without the game.
//
// The two unit constants are each other's inverse; the quaternion yaw reads a rotation about Z as its angle, in
// degrees, with the sign and the range of atan2.

#include "platform/angles.h"

#include "expect.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace {

using lss::platform::kDegreesPerRadian;
using lss::platform::kRadiansPerDegree;
using lss::platform::QuaternionYawDegrees;

bool Near(float value, float expected)
{
    return std::fabs(value - expected) < 0.001f;
}

/// A rotation of `degrees` about Z, as FQuat {x, y, z, w}.
struct AboutZ
{
    float x = 0.0f, y = 0.0f, z, w;
    explicit AboutZ(float degrees)
        : z(std::sin(degrees * kRadiansPerDegree / 2.0f)), w(std::cos(degrees * kRadiansPerDegree / 2.0f))
    {
    }
};

void TestUnits()
{
    std::puts("angles: degrees and radians are each other's inverse");
    EXPECT(Near(180.0f * kRadiansPerDegree, 3.14159265f));
    EXPECT(Near(3.14159265f * kDegreesPerRadian, 180.0f));
    EXPECT(Near(kRadiansPerDegree * kDegreesPerRadian, 1.0f));
}

void TestYaw()
{
    std::puts("angles: the quaternion yaw is the rotation about Z, in degrees");
    EXPECT(Near(QuaternionYawDegrees(0.0f, 0.0f, 0.0f, 1.0f), 0.0f));  // identity
    for (const float degrees : {90.0f, -90.0f, 45.0f, -135.0f, 179.0f})
    {
        const AboutZ q(degrees);
        EXPECT(Near(QuaternionYawDegrees(q.x, q.y, q.z, q.w), degrees));
    }
    const AboutZ half(180.0f);
    EXPECT(Near(std::fabs(QuaternionYawDegrees(half.x, half.y, half.z, half.w)), 180.0f));
}

}  // namespace

int main()
{
    TestUnits();
    TestYaw();

    return lss::test::Report();
}