#include "player_two/world/strand_watch.h"

#include <algorithm>

namespace lss::player_two {

StrandEvent StrandWatch::Step(bool refused, float seconds) noexcept
{
    seconds   = std::max(seconds, 0.0f);
    cooldown_ = std::max(cooldown_ - seconds, 0.0f);
    if (!stranded_)
    {
        if (!refused)
        {
            return StrandEvent::None;
        }
        stranded_ = true;
        asked_    = false;
        held_     = 0.0f;
        gap_      = 0.0f;
        return StrandEvent::Began;
    }

    held_ += seconds;
    if (!refused)
    {
        gap_ += seconds;
        if (gap_ > rules_.graceSeconds)
        {
            stranded_ = false;
            return StrandEvent::Ended;
        }
        return StrandEvent::None;
    }
    gap_ = 0.0f;
    if (!asked_ && held_ >= rules_.holdSeconds && cooldown_ <= 0.0f)
    {
        asked_    = true;
        cooldown_ = rules_.cooldownSeconds;
        return StrandEvent::Warp;
    }
    return StrandEvent::None;
}

}  // namespace lss::player_two
