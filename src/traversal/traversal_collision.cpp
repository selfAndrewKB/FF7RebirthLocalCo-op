#include "traversal/traversal_collision.h"

#include "camera/camera_access.h"
#include "facts/cameras.h"
#include "facts/controller.h"
#include "platform/log.h"
#include "players/actor_fields.h"
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
std::array<CollisionWatch, players::kMaxPlayers> g_collision;  // by player: 0 player 1's camera, then each seat's

const void* SlotFifteen(const void* cameraManager) noexcept
{
    const void* op = players::actor_fields::FieldOperator(cameraManager);
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
    platform::LogDebug(L"traversal camera: {}'s {} camera (vt 0x{:X}) collision: {}", players::SeatName(player),
                       w.vtable == kClimbingModuleVt ? L"climbing wall" : L"traversal", w.vtable,
                       text.empty() ? std::wstring(L"no frames") : text);
}

void WatchCollision(int player, const void* cameraManager)
{
    CollisionWatch& w      = g_collision[player];
    const void*     module = cameraManager != nullptr ? SlotFifteen(cameraManager) : nullptr;
    const auto      vt     = module != nullptr ? VTableOf(module) : 0;
    // Only the traversal modules this watch follows (a dev tool): the climbing wall's (the holders unit installs it)
    // and a seat's ladder's (any other slot-15 module, a ride framing or a cutscene, is left to its own watch).
    const bool traversal =
        module != nullptr && (vt == g_climbingModuleVt || (player >= 1 && module == g_ladderModule[player]));
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
        w.vtable = g_image ? g_image->SourceAddress(vt) : vt;
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
        // Player 1's camera, and the camera of each seat whose player is in (a seat that is out has none to watch).
        for (int p = 0; p < players::kMaxPlayers; ++p)
        {
            if (p != 0 && !players::generation::Active(p))
            {
                CollisionWatch& w = g_collision[static_cast<std::size_t>(p)];
                if (w.module != nullptr)
                {
                    LogCollision(p, w);  // the seat's player left mid-camera: its line, then a fresh record
                }
                w = CollisionWatch{};
                continue;
            }
            void* const controller = players::generation::ControllerOf(p);
            WatchCollision(p, controller != nullptr ? players::actor_fields::CameraManager(controller) : nullptr);
        }
    }
    catch (...)
    {
    }
}

}  // namespace lss::traversal::traversal_camera::collision
