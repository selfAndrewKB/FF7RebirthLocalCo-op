#pragma once

// The CharaPop manager, its entries and a camera manager's pose, as the game's character LOD reads them
// (RE-p2-charapop-per-view.md §1; IDA addresses and offsets of this build).

#include "platform/angles.h"
#include "player_two/engine_facts.h"
#include "player_two/object_fields.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace lss::platform {
bool CopyGuarded(void* out, const void* from, std::size_t length) noexcept;  // platform/guarded.h
}

namespace lss::player_two::charapop {

// The manager M = [[game root]+0x180] and its pop object P = [M+0x10] (§1.1).
inline constexpr std::uintptr_t kGameRoot      = 0x1490FEDE8;
inline constexpr std::uint32_t  kRootCharaPop  = 0x180;
inline constexpr std::uint32_t  kManagerPop    = 0x10;
inline constexpr std::uint32_t  kPopListInline = 0xC8;
inline constexpr std::uint32_t  kPopListHeap   = 0x20C8;
inline constexpr std::uint32_t  kPopCount      = 0x20D0;
inline constexpr std::int32_t   kMaxEntries    = 4096;

// The LOD parameters in M (RE-p2-lod-draw-distance.md §1; floats unless noted).
inline constexpr std::uint32_t kShouldTickFov        = 0x1148;  // degrees off the camera's forward
inline constexpr std::uint32_t kLightEnableDraw      = 0x114C;
inline constexpr std::uint32_t kLightInCamera        = 0x1150;
inline constexpr std::uint32_t kLightOutCamera       = 0x1154;
inline constexpr std::uint32_t kLightShouldTick      = 0x1158;
inline constexpr std::uint32_t kMiddleEnableDraw     = 0x115C;
inline constexpr std::uint32_t kMiddleInCamera       = 0x1160;
inline constexpr std::uint32_t kMiddleOutCamera      = 0x1164;
inline constexpr std::uint32_t kMiddleShouldTick     = 0x1168;
inline constexpr std::uint32_t kEnemyInCameraAdded   = 0x1170;
inline constexpr std::uint32_t kOverrideEnableDraw   = 0x1178;  // with its enable byte at +4
inline constexpr std::uint32_t kOverrideInCamera     = 0x1180;  // with its enable byte at +4
inline constexpr std::uint32_t kOverrideOutCamera    = 0x1188;  // with its enable byte at +4

// One entry (§1.3).
inline constexpr std::uint32_t kEntryKey      = 0x08;   // 8 bytes, FName layout: the setter's key
inline constexpr std::uint32_t kEntryId       = 0x10;   // i32
inline constexpr std::uint32_t kEntryCategory = 0x24;   // u8: 1 has the draw-distance hide, 4 enemy
inline constexpr std::uint32_t kEntryLevel    = 0x1A0;  // u8: 1 Light, 2 Middle, 3 Heavy
inline constexpr std::uint32_t kEntryMinLevel = 0x1A1;  // u8: the per-entry minimum (the setter's byte)
inline constexpr std::uint32_t kEntryActor    = 0x1A4;  // FWeakObjectPtr
inline constexpr std::uint32_t kEntryState    = 0x1AC;  // i32: 5 visible, 6 fading out, 7 hidden, 8 fading in

inline constexpr std::uint8_t kLevelHeavy = 3;

// The camera manager's pose the FOV test reads: a pointer at +0x2CB8 (`mov rcx,[rax+2CB8h]` at its entry);
// the pose's location at +0, its rotator at +0x10 (degrees).
inline constexpr std::uint32_t kCameraPose = 0x2CB8;

/// The manager and pop object, or nulls when this map has none (or not the shape the research read).
struct Objects
{
    const std::byte* manager = nullptr;
    const std::byte* pop     = nullptr;
};

inline Objects ObjectsOf(std::uintptr_t gameRootGlobal) noexcept
{
    const auto* root    = static_cast<const std::byte*>(ReadPointer(reinterpret_cast<const void*>(gameRootGlobal), 0));
    const auto* manager = static_cast<const std::byte*>(ReadPointer(root, kRootCharaPop));
    const auto* pop     = static_cast<const std::byte*>(ReadPointer(manager, kManagerPop));
    if (pop == nullptr || ReadPointer(pop, 0) != manager)
    {
        return {};
    }
    return {manager, pop};
}

/// The pop object's round-robin list (heap or inline) and its count, clamped.
inline const std::byte* EntryList(const std::byte* pop, std::int32_t& count) noexcept
{
    const auto* heap = static_cast<const std::byte*>(ReadPointer(pop, kPopListHeap));
    const auto  read = ReadField<std::int32_t>(pop, kPopCount);
    count            = read < 0 ? 0 : read > kMaxEntries ? kMaxEntries : read;
    return heap != nullptr ? heap : pop + kPopListInline;
}

struct Camera
{
    bool  read = false;
    float location[3]{};
    float forward[3]{};
};

/// A controller's camera pose as the FOV test reads it (forward as FRotator::Vector makes it).
inline Camera CameraOf(const void* controller) noexcept
{
    Camera      camera;
    const void* manager = ReadPointer(controller, facts::kControllerCameraManager);
    const void* posed   = ReadPointer(manager, kCameraPose);
    float       pose[7]{};  // location [0..2], 4 bytes, rotator [4..6] (pitch, yaw, roll)
    if (posed == nullptr || !platform::CopyGuarded(pose, posed, sizeof(pose)))
    {
        return camera;
    }
    const float pitch = pose[4] * platform::kRadiansPerDegree;
    const float yaw   = pose[5] * platform::kRadiansPerDegree;
    camera.read       = true;
    for (int i = 0; i < 3; ++i)
    {
        camera.location[i] = pose[i];
    }
    camera.forward[0] = std::cos(pitch) * std::cos(yaw);
    camera.forward[1] = std::cos(pitch) * std::sin(yaw);
    camera.forward[2] = std::sin(pitch);
    return camera;
}

inline float Distance(const float (&a)[3], const float (&b)[3]) noexcept
{
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

/// The actor's location (its reflected RootComponent's world location, the offset found once). False if
/// unreadable. Game thread.
bool ActorLocation(void* actor, float (&out)[3]) noexcept;

/// The actor's object name, or "?" (for the log).
std::wstring NameOf(void* actor);

/// Degrees between the camera's forward and the direction to `at` (the FOV test's angle).
inline float AngleOff(const Camera& camera, const float (&at)[3]) noexcept
{
    const float to[3]  = {at[0] - camera.location[0], at[1] - camera.location[1], at[2] - camera.location[2]};
    const float length = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
    if (length <= 0.0f)
    {
        return 0.0f;
    }
    float dot = (to[0] * camera.forward[0] + to[1] * camera.forward[1] + to[2] * camera.forward[2]) / length;
    dot       = dot < -1.0f ? -1.0f : dot > 1.0f ? 1.0f : dot;
    return std::acos(dot) * platform::kDegreesPerRadian;
}

}  // namespace lss::player_two::charapop
