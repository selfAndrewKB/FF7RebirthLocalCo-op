#include "world/catch_up_move.h"

#include "facts/world.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "players/unreal_access.h"

#include <cmath>
#include <cstdint>

namespace lss::world::catch_up_move {

namespace {

/// A TArray<float, TInlineAllocator<4>> as the game builds one on its stack.
struct FloatArray
{
    float        inline_[4] = {};
    float*       heap       = nullptr;
    std::int32_t num        = 0;
    std::int32_t max        = 4;
};
static_assert(sizeof(FloatArray) == 32);

/// An FTransform: rotation (x y z w), translation, scale; each 16 bytes.
struct alignas(16) Transform
{
    float rotation[4]    = {0.0f, 0.0f, 0.0f, 1.0f};
    float translation[4] = {};
    float scale[4]       = {1.0f, 1.0f, 1.0f, 0.0f};
};
static_assert(sizeof(Transform) == 48);

using AroundFn = bool (*)(void* unused, void* querier, const Transform* origin, const void* agent, const FloatArray* distances,
                          const FloatArray* angles, std::int32_t unusedInt, float* out);
using AgentFn  = const void* (*)(void* agentInterface, std::int32_t);
using WarpFn   = void (*)(void* character, const float* look, const float* rotation, const float* dest, bool snapToGround,
                        bool resetControlHolder);

// Set at install, then only read.
AroundFn g_around = nullptr;
WarpFn   g_warp   = nullptr;

/// The querier's nav agent, by its own interface's getter (the game's battle gather makes the same call).
const void* AgentOf(void* querier) noexcept
{
    void* const agentInterface = static_cast<std::uint8_t*>(querier) + facts::kCharacterNavAgentInterface;
    const auto  getter = reinterpret_cast<AgentFn>(ReadField<std::uintptr_t>(ReadPointer(agentInterface, 0), facts::kNavAgentGetterSlot));
    const void* agent  = nullptr;
    return getter != nullptr && platform::CallGuarded(getter, agent, agentInterface, std::int32_t{0}) ? agent : nullptr;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto around = platform::FindFunction(image, facts::kNavPointAround);
    const auto warp   = platform::FindFunction(image, facts::kBattleTeleportFadeIn);
    if (!around || !warp)
    {
        platform::Log(L"catch-up move: an engine check failed; no character is brought beside another");
        return false;
    }
    g_around = reinterpret_cast<AroundFn>(*around);
    g_warp   = reinterpret_cast<WarpFn>(*warp);  // last: Installed tests it
    return true;
}

bool Installed() noexcept
{
    return g_warp != nullptr;
}

bool PointAround(void* querier, const float origin[3], const float yawRadians, const float distanceCm, float out[3]) noexcept
{
    if (!Installed() || querier == nullptr || !std::isfinite(yawRadians))
    {
        return false;
    }
    const void* agent = AgentOf(querier);
    if (agent == nullptr)
    {
        return false;
    }
    Transform from;
    from.rotation[2]    = std::sin(yawRadians * 0.5f);
    from.rotation[3]    = std::cos(yawRadians * 0.5f);
    from.translation[0] = origin[0];
    from.translation[1] = origin[1];
    from.translation[2] = origin[2];
    FloatArray distances;
    distances.inline_[0] = distanceCm;
    distances.num        = 1;
    FloatArray angles;
    angles.num = 1;  // straight along the yaw
    alignas(16) float point[4] = {};
    bool              found    = false;
    if (!platform::CallGuarded(g_around, found, static_cast<void*>(nullptr), querier, static_cast<const Transform*>(&from), agent,
                               static_cast<const FloatArray*>(&distances), static_cast<const FloatArray*>(&angles),
                               std::int32_t{0}, static_cast<float*>(point)) ||
        !found || !std::isfinite(point[0]) || !std::isfinite(point[1]) || !std::isfinite(point[2]))
    {
        return false;
    }
    out[0] = point[0];
    out[1] = point[1];
    out[2] = point[2];
    return true;
}

bool Move(void* character, const float point[3], const float yawDegrees, const float* look, const bool resetControlHolder) noexcept
{
    if (!Installed() || character == nullptr)
    {
        return false;
    }
    const float rotation[3] = {0.0f, yawDegrees, 0.0f};  // an FRotator: pitch, yaw, roll in degrees
    return platform::CallGuarded(g_warp, character, look != nullptr ? look : point, static_cast<const float*>(rotation), point,
                                 true, resetControlHolder);
}

bool SpotTaken(const float at[3], const void* except) noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const void* other = players::generation::CameraMemberOf(seat);
        float       there[3] = {};
        if (other != nullptr && other != except && players::unreal::ActorLocation(other, there) &&
            std::hypot(at[0] - there[0], at[1] - there[1]) < 100.0f)
        {
            return true;
        }
    }
    return false;
}

}  // namespace lss::world::catch_up_move
