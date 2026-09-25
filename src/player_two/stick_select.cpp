#include "player_two/stick_select.h"

#include <cstddef>

namespace lss::player_two {

namespace {

constexpr std::int32_t kFloatsPerRecord = 6;

float Magnitude(float x, float y) noexcept
{
    return x * x + y * y;
}

}  // namespace

Sticks SelectPadSticks(std::span<const StickBank> banks, std::int32_t controllerId, const Sticks& merged) noexcept
{
    Sticks chosen{};
    float  bestLeft  = 0.0f;
    float  bestRight = 0.0f;
    for (const StickBank& bank : banks)
    {
        if (bank.records == nullptr || controllerId < 0 || controllerId >= bank.count)
        {
            continue;
        }
        const float* record = bank.records + static_cast<std::ptrdiff_t>(controllerId) * kFloatsPerRecord;
        if (const float left = Magnitude(record[0], record[1]); left > bestLeft)
        {
            bestLeft  = left;
            chosen[0] = record[0];
            chosen[1] = record[1];
        }
        if (const float right = Magnitude(record[2], record[3]); right > bestRight)
        {
            bestRight = right;
            chosen[2] = record[2];
            chosen[3] = record[3];
        }
    }
    if (merged[0] == 0.0f && merged[1] == 0.0f)
    {
        chosen[0] = chosen[1] = 0.0f;
    }
    if (merged[2] == 0.0f && merged[3] == 0.0f)
    {
        chosen[2] = chosen[3] = 0.0f;
    }
    return chosen;
}

}  // namespace lss::player_two
