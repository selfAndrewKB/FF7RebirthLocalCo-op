#include "platform/deferred_sites.h"

#include "platform/build_value.h"
#include "platform/game_thread_sites.h"

namespace lss::platform {

bool DeferredSites::Install(const GameImage& image, PlaceFn place, bool deferred) noexcept
{
    image_.emplace(image);
    place_          = place;
    fromGameThread_ = deferred;
    if (!deferred)
    {
        return place(*image_);
    }
    pending_ = true;
    if (game_thread_sites::Queue(&RunQueued, this))
    {
        return true;
    }
    pending_ = false;
    return false;
}

void DeferredSites::RunQueued(void* self) noexcept
{
    auto& sites = *static_cast<DeferredSites*>(self);
    if (sites.pending_.exchange(false))
    {
        sites.place_(*sites.image_);
    }
}

bool DeferredSites::Deferred() noexcept
{
    return RunningBuild() == BuildIndex::V1005;
}

}  // namespace lss::platform
