#pragma once

// The game's location volumes, walked for each seat (player_two/camera/location_rows.h: the camera row a seat's
// character stands in; player_two/input/walk_limits.h: the move-type volumes it stands in). Nothing here is kept per
// seat: the walk reads the game's one list for whichever seat's visitor asks. One global location manager
// (g_LocationVolumeManager, a pointer) holds the volume array (+8 data, +0x10 count), its move-type stack, a pause byte
// and the lock (+0x188, a CRITICAL_SECTION the game's update holds); each AEndLocationVolume carries its LocationId,
// UntouchMargin, flags and the fields each rule reads; the game's own containment test
// LocationVolume_ContainsPoint(volume, &position, margin) is a pure read (RE-p2-framing.md §2.3, RE-p2-walk-mode.md
// §4.3.1). The facts, the checked install and the locked walk are here once; each rule (the camera-row pick, the
// move-type enter/leave with its refcounted stack) stays in its module, as a visitor of the walk. Game thread.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::location_volumes {

// The manager's fields.
inline constexpr std::uint32_t kManagerVolumes     = 0x08;
inline constexpr std::uint32_t kManagerVolumeCount = 0x10;
inline constexpr std::uint32_t kManagerStack       = 0xF8;   // {int32 refcount; u8 move; u8 dash; pad} entries
inline constexpr std::uint32_t kManagerStackCount  = 0x100;
inline constexpr std::uint32_t kManagerPaused      = 0x128;  // byte: the game's update does nothing while set
inline constexpr std::uint32_t kManagerLock        = 0x188;
inline constexpr std::int32_t  kMaxVolumes         = 4096;

// Each volume's fields.
inline constexpr std::uint32_t kVolumeLocationId     = 0x270;
inline constexpr std::uint32_t kVolumeUntouchMargin  = 0x278;  // float: the margin the game's test stays inside by
inline constexpr std::uint32_t kVolumeFlags          = 0x280;
inline constexpr std::uint32_t kVolumeCameraRow      = 0x2E0;  // FName CameraSettingID
inline constexpr std::uint32_t kVolumeCameraPriority = 0x2E8;  // lower = higher priority
inline constexpr std::uint32_t kVolumeMoveType       = 0x2E9;  // EEndSpecificMoveType
inline constexpr std::uint32_t kVolumeDashType       = 0x2EA;  // EEndDashType
inline constexpr std::uint32_t kVolumeForcedOut      = 0x308;  // bool: never entered by position; a forced one leaves
inline constexpr std::uint32_t kFlagDisabled           = 0x1;
inline constexpr std::uint32_t kFlagForce              = 0x2;
inline constexpr std::uint32_t kFlagChangeCamera       = 0x8;
inline constexpr std::uint32_t kFlagChangeMovementType = 0x8000;

/// UEndPlayerExistNavAreaComponent's controller field (both modules read it).
inline constexpr std::uint32_t kNavAreaController = 0xB0;

/// Finds the manager pointer and the containment test; false if either check fails (the callers say so). Once.
bool Install(const platform::GameImage& image) noexcept;
bool Installed() noexcept;

/// One volume as the walk reads it: its address, flags and LocationId (a streamed-in volume may reuse a streamed-out
/// one's address, so a rule matches on both).
struct Volume
{
    const void*   volume   = nullptr;
    std::uint32_t flags    = 0;
    std::uint64_t location = 0;
};

/// The game's containment test for `volume` at `position`, within `margin` (0: strictly inside). False if the test
/// could not run; `inside` is its answer.
bool Contains(const void* volume, const float (&position)[3], float margin, bool& inside) noexcept;

/// What a walk read of the manager besides the volumes.
struct Reads
{
    bool         paused  = false;  // the pause byte was set
    bool         hasTop  = false;  // the move-type stack has an entry
    std::uint8_t topMove = 0;      // ... its {move, dash}
    std::uint8_t topDash = 0;
};

enum class WalkResult
{
    Busy,        // no manager, or the game's update holds its lock: nothing changes this tick
    Unreadable,  // the volume list could not be read, or the lock not left: nothing is safe to conclude
    Paused,      // `skipWhilePaused`, and the pause byte is set
    Done,
};

using Visitor = void (*)(void* context, const Volume& volume);

/// Walks the manager's volume list under its lock: `visit(context, volume)` for every volume whose pointer, flags
/// and LocationId could be read, in the list's order (the visitor may call Contains). `reads` is filled either way.
WalkResult WalkVolumes(bool skipWhilePaused, Visitor visit, void* context, Reads& reads) noexcept;

}  // namespace lss::player_two::location_volumes
