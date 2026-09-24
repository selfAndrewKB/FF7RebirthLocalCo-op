// Host-side tests for views::LooksLikeViewInfo and FillHalf, run without the game.
//
// The view info is a zeroed 0x6A0-byte block (FMinimalViewInfo's size in this build) given the
// shape the game's cameras produce: 16:9, bars on. The fill may clear only the constrain bit;
// every other bit and byte, the aspect ratio included, stays as it was. A block with implausible
// numbers is not recognised, so it is never written.

#include "views/minimal_view_info.h"
#include "views/view_shape.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

using lss::views::FillHalf;
using lss::views::LooksLikeViewInfo;
namespace view_info = lss::views::view_info;

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
    unsigned char Flags() const { return static_cast<unsigned char>(bytes[view_info::kFlags]); }
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

void TestFillClearsOnlyTheConstrainBit()
{
    std::puts("fill: clears only the constrain bit; the ratio, the LOD bit and every other byte are kept");
    ViewInfo view(kSixteenByNine, 0x03, 90.0f);
    auto expected = view.bytes;
    expected[view_info::kFlags] = static_cast<std::byte>(0x02);

    EXPECT(FillHalf(view.bytes.data()));
    EXPECT(view.Flags() == 0x02);
    EXPECT(view.bytes == expected);

    EXPECT(!FillHalf(view.bytes.data()));  // already filled: reports it, changes nothing
    EXPECT(view.bytes == expected);
    EXPECT(!FillHalf(nullptr));

    ViewInfo leftovers(kSixteenByNine, 0xA3, 60.0f);
    EXPECT(FillHalf(leftovers.bytes.data()));
    EXPECT(leftovers.Flags() == 0xA2);  // the unused bits are left as they were
}

}  // namespace

int main()
{
    TestGameCameraIsRecognised();
    TestUnknownShapesAreRefused();
    TestFillClearsOnlyTheConstrainBit();

    std::printf(g_failures == 0 ? "all tests passed\n" : "%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
