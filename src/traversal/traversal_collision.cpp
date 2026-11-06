#include "traversal/traversal_collision.h"

#include "camera/camera_access.h"
#include "facts/cameras.h"
#include "facts/controller.h"
#include "platform/log.h"
#include "players/object_fields.h"
#include "traversal/traversal_shared.h"

#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string>

namespace lss::traversal::traversal_camera::collision {

namespace {

std::optional<platform::GameImage> g_image;
std::uintptr_t                     g_climbingModuleVt = 0;  // live

/// One traversal camera's time on a player's slot 15: frames, and frames the collision pulled the camera in, with the
/// climbing wall's hip framing off and on, then the frames after it left (the hand-over to the follow camera).
struct CollisionWatch
{
    const void*    module   = nullptr;
    std::uintptr_t vtable   = 0;  // research
    unsigned       frames[3]{}, pulled[3]{};
    std::wstring   firstHit[3];
    unsigned       after = 0;  // frames counted since the module left (0: none)
};
std::array<CollisionWatch, 2> g_collision;

const void* SlotFifteen(const void* cameraManager) noexcept
{
    const void* op = ReadPointer(cameraManager, facts::kCameraManagerFieldOperator);
    return camera::camera_access::ModuleAt(op, kSlotFifteen);
}

void LogCollision(int player, const CollisionWatch& w)
{
    static constexpr const wchar_t* kBucket[] = {L"hip framing off", L"hip framing on", L"after it ended"};
    std::wstring text;
    for (int b = 0; b < 3; ++b)
    {
        if (w.frames[b] == 0)
        {
            continue;
        }
        text += std::format(L"{}{}: pulled in {} of {} frame(s){}", text.empty() ? L"" : L"; ", kBucket[b], w.pulled[b],
                            w.frames[b], w.firstHit[b].empty() ? L"" : L", first hit " + w.firstHit[b]);
    }
    platform::LogDebug(L"traversal camera: player {}'s {} camera (vt 0x{:X}) collision: {}", player + 1,
                       w.vtable == kClimbingModuleVt ? L"climbing wall" : L"traversal", w.vtable,
                       text.empty() ? std::wstring(L"no frames") : text);
}

void WatchCollision(int player, const void* cameraManager)
{
    CollisionWatch& w      = g_collision[player];
    const void*     module = cameraManager != nullptr ? SlotFifteen(cameraManager) : nullptr;
    const auto      vt     = module != nullptr ? VTableOf(module) : 0;
    // Only the traversal modules this watch follows (a dev tool: player 1's and player 2's): the climbing wall's (the
    // holders unit installs it) and player 2's ladder's (any other slot-15 module, a ride framing or a cutscene, is left to its own watch).
    const bool traversal =
        module != nullptr &&
        (vt == g_climbingModuleVt || (player == 1 && module == g_ladderModule[players::kFirstFurtherSeat]));
    if (module != w.module && w.module != nullptr && w.after == 0)
    {
        w.after = 1;  // the traversal camera left: watch the hand-over
    }
    if (traversal && module != w.module)
    {
        if (w.module != nullptr)
        {
            LogCollision(player, w);
        }
        w        = CollisionWatch{};
        w.module = module;
        w.vtable = g_image ? g_image->ResearchAddress(vt) : vt;
    }
    if (w.module == nullptr || cameraManager == nullptr)
    {
        return;
    }
    int bucket = 2;
    if (w.after == 0)
    {
        bucket = vt == g_climbingModuleVt && ReadField<float>(module, kClimbHipBlend) > 0.5f ? 1 : 0;
    }
    ++w.frames[bucket];
    if (ReadField<std::uint8_t>(cameraManager, kPcmPulledIn) != 0)
    {
        ++w.pulled[bucket];
        if (w.firstHit[bucket].empty())
        {
            const void* hit = scopes::leader_scope::ObjectAt(static_cast<const std::byte*>(cameraManager) + kPcmHitActor);
            w.firstHit[bucket] = hit != nullptr ? players::NameOf(hit) : std::wstring(L"(no actor)");
        }
    }
    if (w.after != 0 && ++w.after > kAfterFrames)
    {
        LogCollision(player, w);
        w = CollisionWatch{};
    }
}

void ResetCollisionWatch() noexcept
{
    g_collision = {};
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    g_image.emplace(image);
    const auto vt      = image.Rebase(kClimbingModuleVt, 0x30);
    g_climbingModuleVt = vt ? *vt : 0;
    return g_climbingModuleVt != 0;
}

void Tick(bool active) noexcept
{
    if (!active)
    {
        ResetCollisionWatch();
        return;
    }
    try
    {
        void* const controllers[2] = {players::generation::PlayerOneController(), players::generation::PlayerTwoController()};
        for (int p = 0; p < 2; ++p)
        {
            WatchCollision(p, controllers[p] != nullptr ? ReadPointer(controllers[p], facts::kControllerCameraManager)
                                                        : nullptr);
        }
    }
    catch (...)
    {
    }
}

}  // namespace lss::traversal::traversal_camera::collision
