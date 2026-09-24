#include "views/view_turn.h"

#include <cmath>
#include <cstring>

namespace lss::views {

float NormalizeDegrees(float degrees) noexcept
{
    float wrapped = std::fmod(degrees, 360.0f);  // (-360, 360)
    if (wrapped > 180.0f)
    {
        wrapped -= 360.0f;
    }
    else if (wrapped <= -180.0f)
    {
        wrapped += 360.0f;
    }
    return wrapped;
}

bool TurnIfPlayer(const void* self, const void* turnedPlayer, float degrees, std::byte* viewInfo) noexcept
{
    if (self == nullptr || self != turnedPlayer || viewInfo == nullptr)
    {
        return false;
    }
    float yaw = 0.0f;
    std::memcpy(&yaw, viewInfo + view_info::kYaw, sizeof(yaw));
    yaw = NormalizeDegrees(yaw + degrees);
    std::memcpy(viewInfo + view_info::kYaw, &yaw, sizeof(yaw));
    return true;
}

}  // namespace lss::views
