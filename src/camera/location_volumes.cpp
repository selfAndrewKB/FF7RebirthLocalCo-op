#include "camera/location_volumes.h"

#include "facts/world.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/verified_code.h"
#include "players/object_fields.h"

#include <cstddef>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::camera::location_volumes {

namespace {

using ContainsFn = bool (*)(const void* volume, const float* position, float margin);

std::uintptr_t g_manager  = 0;  // the address of the manager pointer
ContainsFn     g_contains = nullptr;

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    if (Installed())
    {
        return true;
    }
    const auto manager  = image.Rebase(facts::kLocationVolumeManager, sizeof(void*));
    const auto contains = platform::FindFunction(image, facts::kLocationVolumeContainsPoint);
    if (!manager || !contains)
    {
        return false;
    }
    g_manager  = *manager;
    g_contains = reinterpret_cast<ContainsFn>(*contains);
    return true;
}

bool Installed() noexcept
{
    return g_manager != 0 && g_contains != nullptr;
}

bool Contains(const void* volume, const float (&position)[3], float margin, bool& inside) noexcept
{
    inside = false;
    return g_contains != nullptr && platform::CallGuarded(g_contains, inside, volume, &position[0], margin);
}

WalkResult WalkVolumes(bool skipWhilePaused, Visitor visit, void* context, Reads& reads) noexcept
{
    reads = {};
    void* manager = ReadPointerAt(g_manager);
    if (manager == nullptr)
    {
        return WalkResult::Busy;
    }
    // The lock lives in engine memory like any field, so its calls are guarded too (a manager torn down under us).
    auto* lock    = reinterpret_cast<CRITICAL_SECTION*>(static_cast<std::byte*>(manager) + kManagerLock);
    BOOL  entered = FALSE;
    if (!platform::CallGuarded(&TryEnterCriticalSection, entered, lock) || !entered)
    {
        return WalkResult::Busy;
    }
    std::uint8_t paused  = 0;
    void* const* volumes = nullptr;
    std::int32_t count   = 0;
    bool         readable = Read(manager, kManagerPaused, paused) && Read(manager, kManagerVolumes, volumes) &&
                    Read(manager, kManagerVolumeCount, count) && count >= 0 && count <= kMaxVolumes &&
                    (count == 0 || volumes != nullptr);
    reads.paused = paused != 0;
    const bool walk = readable && !(skipWhilePaused && reads.paused);
    for (std::int32_t i = 0; walk && i < count; ++i)
    {
        Volume volume;
        if (!platform::CopyGuarded(&volume.volume, volumes + i, sizeof(volume.volume)) || volume.volume == nullptr ||
            !Read(volume.volume, kVolumeFlags, volume.flags) ||
            !Read(volume.volume, kVolumeLocationId, volume.location))
        {
            continue;
        }
        visit(context, volume);
    }
    std::int32_t     stackCount = 0;
    const std::byte* stack      = nullptr;
    if (readable && Read(manager, kManagerStackCount, stackCount) && stackCount > 0 &&
        Read(manager, kManagerStack, stack))
    {
        reads.hasTop = Read(stack, 4, reads.topMove) && Read(stack, 5, reads.topDash);
    }
    if (!platform::CallGuarded(&LeaveCriticalSection, lock))
    {
        readable = false;  // nothing more is safe to conclude from this pass
    }
    if (!readable)
    {
        return WalkResult::Unreadable;
    }
    return walk ? WalkResult::Done : WalkResult::Paused;
}

}  // namespace lss::camera::location_volumes
