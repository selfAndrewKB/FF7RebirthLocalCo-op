#include "player_two/camera/camera_repair.h"

#include "facts/cameras.h"
#include "facts/controller.h"
#include "platform/guarded.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/camera/camera_access.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"
#include "player_two/rides/chocobo_ride.h"
#include "player_two/scopes/leader_scope.h"
#include "player_two/scopes/primary_controller.h"
#include "player_two/world/world_rules.h"
#include "players/unreal_access.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>

namespace lss::player_two::camera_repair {

namespace {

using Clock = std::chrono::steady_clock;

// PrimaryPCM_ResetCameraToField(u8 bForce) (1.005 0x1414335C4): GetEndPlayerCameraManager(), then
// AEndPlayerCameraManager::ResetCameraToField_Cut (0x1408A55DC): the camera-cut bit, the collision and occlusion state
// cleared, the operators reset, and the Reset module re-armed on the primary camera's current operator. 31 game
// callers (player warps, fast travel). RE-p2-chocobo-warp-camera.md §5.1.
constexpr std::uint8_t kResetBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xD8,
                                        0xAE, 0x80, 0x00, 0x48, 0x2B, 0xE0, 0x8A, 0xD9};
constexpr platform::FunctionFact kReset{L"PrimaryPCM_ResetCameraToField", 0x1418A404C, kResetBytes};
using ResetFn = void (*)(std::uint8_t force);

// The Field operator (camera manager +0x2CB0) and its modules (§5.1, §6).
constexpr std::uint32_t kOpPose        = 0x40;  // the pose floats +0x40..+0x98; pitch, yaw, roll at +0x50;
                                                // three of them are stack residue (world_rules::kPoseLanes)
constexpr std::uint32_t kOpPoseFloats  = (0x98 - 0x40) / 4;
constexpr std::uint32_t kOpPitch       = (0x50 - 0x40) / 4;
constexpr std::uint32_t kOpCurrent     = 0xD1;   // u8: the operator the camera actor is written from
constexpr std::uint32_t kResetSlot     = 10;     // FEndResetCameraModule
constexpr std::uint32_t kSlotTwenty    = 20;     // its weight above 0 makes the Reset module stop without writing
constexpr std::uint32_t kSlotFifteen   = 15;
constexpr std::uint32_t kModuleWeight  = 0x40;   // float
constexpr std::uint32_t kModuleActive  = 0x44;   // u8
constexpr std::uint32_t kModuleRunning = 0x45;   // u8
constexpr std::uint32_t kResetPitch    = 0x408;  // float: the pitch the Reset module writes
constexpr std::uint32_t kResetYaw      = 0x40C;  // float
constexpr std::uint32_t kPcmFlags      = 0x2C10;  // bit 0x400: the stock bGameCameraCutThisFrame
constexpr std::uint32_t kCameraCutBit  = 0x400;

// The ride framing (RE-p2-chocobo-warp-camera.md §3, §6): while a player rides, slot 15 holds the chocobo camera
// module the boarding installed; the repair lines describe it (the former ride_framing_watch's describer; its
// redirects answered their question, 2026-10-07).
using facts::kChocoboCameraModuleVTable;
constexpr std::uint32_t  kModuleOwner       = 0x50;         // the operator SetModuleSlot15 wrote
constexpr std::uint32_t  kChocoboModuleBody = 0x540;        // FWeakObjectPtr: the ride body it follows
constexpr std::uint32_t  kPcmRideSubject    = 0x2CC8;       // FWeakObjectPtr

constexpr auto     kRetryGap     = std::chrono::milliseconds(500);
constexpr auto     kHealthyFor   = std::chrono::seconds(2);  // finite this long: the next NaN starts a new count
constexpr unsigned kMaxInARow    = 5;
constexpr unsigned kSettleFrames = 10;

struct Pose
{
    bool          read = false, finite = true;  // finite: where it counts (world_rules::PoseFinite)
    float         rotation[3]{};          // pitch, yaw, roll
    float         values[kOpPoseFloats]{};  // the whole block, for naming a non-finite float
    std::uint32_t nonFinite = 0;          // bit i: values[i] is not finite (the lanes included)
};

/// What the float at pose offset `4 * i` is (RE-p2-framing.md §1.3: the operator's 0x58-byte camera command).
const wchar_t* PoseFloatName(std::uint32_t i) noexcept
{
    constexpr const wchar_t* kNames[kOpPoseFloats] = {
        L"location x",   L"location y",   L"location z",   L"location's lane 4", L"pitch",    L"yaw",
        L"roll",         L"FOV",          L"pivot x",      L"pivot y",           L"pivot z",  L"pivot's lane 4",
        L"base x",       L"base y",       L"base z",       L"base's lane 4",     L"distance", L"aim offset x",
        L"aim offset y", L"aim offset z", L"+0x90",        L"height"};
    return i < kOpPoseFloats ? kNames[i] : L"?";
}

// Set once at install.
ResetFn        g_reset           = nullptr;
std::uintptr_t g_chocoboModuleVt = 0;  // live

// Game thread, one per seat (index 0, player 1's, unused).
struct SeatRepair
{
    unsigned                  inARow        = 0;  // repairs since the pose was last healthy
    bool                      gaveUp        = false;
    bool                      refusalLogged = false;
    Clock::time_point         nextTry{};
    world_rules::HealthyTimer healthy;            // the pose finite on every tick since: a new count after a while
    bool                      following     = false;  // the last repair's follow-up lines are due
    unsigned                  followUp      = 0;      // frames since the last repair
    const void*               slotBefore    = nullptr;
    unsigned                  repairs       = 0;      // this process
};
std::array<SeatRepair, players::kMaxPlayers> g_seats{};

Pose ReadPose(const void* op) noexcept
{
    Pose pose;
    pose.read = op != nullptr &&
                platform::CopyGuarded(pose.values, static_cast<const std::byte*>(op) + kOpPose, sizeof(pose.values));
    if (!pose.read)
    {
        return pose;
    }
    for (std::uint32_t i = 0; i < kOpPoseFloats; ++i)
    {
        if (!std::isfinite(pose.values[i]))
        {
            pose.nonFinite |= 1u << i;
        }
    }
    pose.finite = world_rules::PoseFinite(pose.nonFinite);
    for (std::uint32_t i = 0; i < 3; ++i)
    {
        pose.rotation[i] = pose.values[kOpPitch + i];
    }
    return pose;
}

using camera_access::ModuleAt;

std::wstring Metres(float cm)
{
    return cm < 0.0f ? std::wstring(L"?") : std::format(L"{:.1f} m", cm / 100.0f);
}

/// The camera actor's distance from `seat`'s member, in cm (-1 if either is not read).
float CameraToMember(int seat, const void* cameraManager) noexcept
{
    float camera[3]{}, member[3]{};
    if (!players::unreal::ActorLocation(ReadPointer(cameraManager, facts::kCameraManagerEndCameraActor), camera) ||
        !players::unreal::ActorLocation(generation::MemberOf(seat), member))
    {
        return -1.0f;
    }
    const float x = camera[0] - member[0], y = camera[1] - member[1], z = camera[2] - member[2];
    return std::sqrt(x * x + y * y + z * z);
}

/// The Reset module, slot 20's weight and the cut bit: what decides whether the reset writes a pose.
std::wstring ResetInputs(const void* cameraManager, const void* op)
{
    const void* reset = ModuleAt(op, kResetSlot);
    const void* twenty = ModuleAt(op, kSlotTwenty);
    return std::format(L"Reset module (slot 10) {} active {} running {} pitch {:.1f} yaw {:.1f}; slot 20 weight {:.2f}; "
                       L"camera cut {}",
                       reset != nullptr ? L"present" : L"MISSING", ReadField<std::uint8_t>(reset, kModuleActive),
                       ReadField<std::uint8_t>(reset, kModuleRunning), ReadField<float>(reset, kResetPitch),
                       ReadField<float>(reset, kResetYaw), twenty != nullptr ? ReadField<float>(twenty, kModuleWeight) : 0.0f,
                       (ReadField<std::uint32_t>(cameraManager, kPcmFlags) & kCameraCutBit) != 0 ? L"set" : L"clear");
}

/// "pose finite yes (pitch … yaw … roll …)", and which floats of the block are not finite and what they hold: the
/// rotation printed finite while the pose was called non-finite (the 2026-10-21 play check: the base point's lane,
/// stack residue, which is named but does not count).
std::wstring PoseText(const Pose& pose)
{
    std::wstring text = std::format(L"pose finite {} (pitch {:.1f} yaw {:.1f} roll {:.1f}",
                                    pose.finite ? L"yes" : L"NO", pose.rotation[0], pose.rotation[1], pose.rotation[2]);
    for (std::uint32_t i = 0; i < kOpPoseFloats; ++i)
    {
        if ((pose.nonFinite & (1u << i)) != 0)
        {
            const bool lane = (world_rules::kPoseLanes & (1u << i)) != 0;
            text += std::format(L"; {} at +{:#x} = {}{}", PoseFloatName(i), kOpPose + 4 * i, pose.values[i],
                                lane ? L" (stack residue, not counted)" : L"");
        }
    }
    return text + L")";
}

/// Whom an actor is, for the log. Game thread.
std::wstring Whom(const void* actor)
{
    if (actor == nullptr)
    {
        return L"none";
    }
    const void* one = generation::ControllerOf(0);
    if (const int rider = chocobo_ride::SeatDriving(actor); rider >= 1)  // seated or getting off
    {
        return std::format(L"{}'s chocobo", players::SeatName(rider));
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (actor == generation::MemberOf(seat))
        {
            return std::format(L"{}'s member", players::SeatName(seat));
        }
    }
    if (one != nullptr && actor == ReadPointer(one, facts::kControllerPawn))
    {
        return L"player 1's pawn";
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const void* controller = generation::ControllerOf(seat);
        if (controller != nullptr && actor == ReadPointer(controller, facts::kControllerPawn))
        {
            return std::format(L"{}'s pawn", players::SeatName(seat));
        }
    }
    return std::format(L"another actor {}", actor);
}

/// The camera's ride framing in words: slot 15 of its Field operator and the ride subject (the pose is PoseText's).
std::wstring Describe(const void* cameraManager)
{
    const void* op = ReadPointer(cameraManager, facts::kCameraManagerFieldOperator);
    if (op == nullptr)
    {
        return L"no Field operator";
    }
    const void*  module = ModuleAt(op, kSlotFifteen);
    std::wstring slot   = L"empty";
    if (module != nullptr)
    {
        const std::uintptr_t vtable = VTableOf(module);
        slot = vtable == g_chocoboModuleVt ? std::wstring(L"chocobo module") : std::format(L"module vt {:#x} (live)", vtable);
        slot += std::format(L" {} (active {} running {} weight {:.2f}, owner {})", module,
                            ReadField<std::uint8_t>(module, kModuleActive), ReadField<std::uint8_t>(module, kModuleRunning),
                            ReadField<float>(module, kModuleWeight),
                            ReadPointer(module, kModuleOwner) == op ? L"this operator" : L"OTHER");
        if (vtable == g_chocoboModuleVt)
        {
            slot += std::format(L", follows {}",
                                Whom(leader_scope::ObjectAt(static_cast<const std::byte*>(module) + kChocoboModuleBody)));
        }
    }
    return std::format(L"slot 15 {}; ride subject {}", slot,
                       Whom(leader_scope::ObjectAt(static_cast<const std::byte*>(cameraManager) + kPcmRideSubject)));
}

/// The repair's follow-up lines: after one frame (did the reset write the pose) and after ten (did the camera reach
/// the member, and is the ride framing still there).
void FollowUp(int seat, const void* cameraManager, const void* op, const Pose& pose) noexcept
{
    SeatRepair& s = g_seats[static_cast<std::size_t>(seat)];
    ++s.followUp;
    try
    {
        if (s.followUp == 1)
        {
            platform::Log(L"camera repair: after 1 frame: {}; {}", PoseText(pose), ResetInputs(cameraManager, op));
        }
        else if (s.followUp >= kSettleFrames)
        {
            const void* slot = ModuleAt(op, kSlotFifteen);
            platform::Log(L"camera repair: after {} frames: {}: {}, camera→member {}; slot 15 {}; {}", s.followUp,
                          pose.finite ? L"REPAIRED" : L"NOT repaired", PoseText(pose),
                          Metres(CameraToMember(seat, cameraManager)), slot == s.slotBefore ? L"unchanged" : L"CHANGED",
                          Describe(cameraManager));
            s.following = false;
        }
    }
    catch (...)
    {
        s.following = false;
    }
}

void Repair(int seat, void* cameraManager, const void* op, const Pose& pose) noexcept
{
    SeatRepair&    s    = g_seats[static_cast<std::size_t>(seat)];
    const wchar_t* name = players::SeatName(seat);
    s.nextTry           = Clock::now() + kRetryGap;
    // The reset resolves the primary camera manager and its operator.
    const primary_controller::Scope scope(seat);
    if (!scope.Held())
    {
        if (!s.refusalLogged)
        {
            s.refusalLogged = true;
            platform::Log(L"camera repair: {}'s camera pose is not finite, but {}'s primary scope was refused "
                          L"({}); retried every 0.5 s",
                          name, name, primary_controller::Describe(scope.Why()));
        }
        return;
    }
    if (ReadPointer(primary_controller::Current(), facts::kControllerCameraManager) != cameraManager)
    {
        if (!s.refusalLogged)  // the game would resolve another camera manager: never reset player 1's
        {
            s.refusalLogged = true;
            platform::Log(L"camera repair: {}'s camera pose is not finite, but in {}'s scope the primary "
                          L"controller's camera manager is not {}'s; not reset (retried every 0.5 s)",
                          name, name, name);
        }
        return;
    }
    ++s.inARow;
    ++s.repairs;
    s.slotBefore = ModuleAt(op, kSlotFifteen);
    try
    {
        platform::Log(L"camera repair: {}'s camera pose is not finite ({}); the game's player-warp camera reset "
                      L"(PrimaryPCM_ResetCameraToField, cut) on {}'s camera in {}'s primary scope, try {} of {} "
                      L"({} this session). Before: {}; {}; camera→member {}",
                      name, PoseText(pose), name, name, s.inARow, kMaxInARow, s.repairs, Describe(cameraManager),
                      ResetInputs(cameraManager, op), Metres(CameraToMember(seat, cameraManager)));
    }
    catch (...)
    {
    }
    if (!platform::CallGuarded(g_reset, std::uint8_t{1}))
    {
        s.inARow = kMaxInARow;  // never call it again while the pose stays broken
        platform::Log(L"camera repair: the reset raised an exception ({}); no more tries until the pose is finite",
                      platform::DescribeFault(platform::LastFault()).text);
        return;
    }
    s.following = true;
    s.followUp  = 0;
}

/// One seat's tick: its pose read, the last repair's follow-up, and a repair or the give-up.
void TickSeat(int seat) noexcept
{
    SeatRepair& s = g_seats[static_cast<std::size_t>(seat)];
    if (!generation::Active(seat) || generation::MemberOf(seat) == nullptr)
    {
        s.following = false;
        return;
    }
    void*       cameraManager = ReadPointer(generation::ControllerOf(seat), facts::kControllerCameraManager);
    const void* op            = ReadPointer(cameraManager, facts::kCameraManagerFieldOperator);
    const Pose  pose          = ReadPose(op);
    if (!pose.read)
    {
        s.following = false;
        return;
    }
    if (s.following)
    {
        FollowUp(seat, cameraManager, op, pose);
    }
    const auto now = Clock::now();
    if (s.healthy.Observe(pose.finite, now, kHealthyFor))
    {
        s.inARow        = 0;  // a NaN that comes back at once counts toward the same 5 tries
        s.gaveUp        = false;
        s.refusalLogged = false;
    }
    // Not before the last repair's follow-up is written (below ~20 fps the 0.5 s gap is shorter than 10 frames).
    const auto verdict = world_rules::RepairVerdict(pose.finite, s.following, ReadField<std::uint8_t>(op, kOpCurrent) != 0,
                                                    now >= s.nextTry, s.inARow, kMaxInARow, s.gaveUp);
    if (verdict == world_rules::Repair::Repair)
    {
        Repair(seat, cameraManager, op, pose);
    }
    else if (verdict == world_rules::Repair::GiveUp)
    {
        s.gaveUp = true;
        try
        {
            platform::Log(L"camera repair: gave up after {} tries; the pose stays non-finite ({}); {}; {}", kMaxInARow,
                          PoseText(pose), ResetInputs(cameraManager, op), Describe(cameraManager));
        }
        catch (...)
        {
        }
    }
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const auto reset = platform::FindFunction(image, kReset);
    g_reset          = reset ? reinterpret_cast<ResetFn>(*reset) : nullptr;
    const auto vt    = image.Rebase(kChocoboCameraModuleVTable);
    g_chocoboModuleVt = vt ? *vt : 0;
    platform::Log(L"camera repair: {}", g_reset != nullptr
                                            ? L"installed (a non-finite pose on player 2's camera gets the game's camera reset)"
                                            : L"UNAVAILABLE (the reset's bytes differ; a lost camera needs Ctrl+F8, Ctrl+F7)");
}

void Tick() noexcept
{
    if (g_reset == nullptr || !generation::OnGameThread())
    {
        for (SeatRepair& s : g_seats)
        {
            s.following = false;
        }
        return;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        TickSeat(seat);
    }
}

}  // namespace lss::player_two::camera_repair
