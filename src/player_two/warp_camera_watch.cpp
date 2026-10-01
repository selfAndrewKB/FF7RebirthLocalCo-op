#include "player_two/warp_camera_watch.h"

#include "platform/guarded.h"
#include "platform/log.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/object_fields.h"
#include "players/unreal_access.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <string>
#include <tuple>

namespace lss::player_two::warp_camera_watch {

namespace {

// RE-p2-party-warp.md §11.1 and §11.5 (this build). The camera manager's own fields are in engine_facts.h.
// The Field operator (FEndModularCameraOperator, camera manager +0x2CB0):
constexpr std::uint32_t kOpWeight          = 0x08;  // float
constexpr std::uint32_t kOpPose            = 0x40;  // the pose floats +0x40..+0x98
constexpr std::uint32_t kOpPoseFloats      = (0x98 - 0x40) / 4;
constexpr std::uint32_t kOpPivot           = 0x60;  // float[3]: subject location + op +0x760
constexpr std::uint32_t kOpSubjectLocation = 0x70;  // float[3]: the subject's root plus its camera offsets
constexpr std::uint32_t kOpActive          = 0xD0;  // u8
constexpr std::uint32_t kOpCurrent         = 0xD1;  // u8: the operator the camera actor is written from
constexpr std::uint32_t kOpSubject         = 0xF0;  // FWeakObjectPtr: whom the camera follows
constexpr std::uint32_t kOpModules         = 0x6E0;  // module table, 16 bytes a slot
constexpr std::uint32_t kLeaderChangeSlot  = 22;
constexpr std::uint32_t kModuleStride      = 16;
constexpr std::uint32_t kModuleActive      = 0x44;  // u8
constexpr std::uint32_t kModuleRunning     = 0x45;  // u8
constexpr std::uint32_t kLeaderChangeTarget = 0x3F0;  // FWeakObjectPtr: the hand-over's new subject
// The camera manager:
constexpr std::uint32_t kPcmPreviousOperator = 0x2CD1;  // u8, next to the current operator (+0x2CD0)
constexpr std::uint32_t kPcmLeader           = 0x2CC0;  // the leader its OnLeaderChanged recorded
constexpr std::uint32_t kPcmLeaderWeak       = 0x2CC8;
constexpr std::uint32_t kPcmPersistent[]     = {0x2CD8, 0x2CE4, 0x2F90, 0x2F94, 0x2F98, 0x2FA0};  // collision, occlusion

constexpr float     kWatchSeconds   = 4.0f;    // the warp, its 0.2 s fades and the commit take about 0.5 s
constexpr float     kAfterSeconds   = 5.0f;    // once left behind: a sample a second for this long
constexpr float     kNearSubjectCm  = 1000.0f;  // the subject location is its root plus small camera offsets
constexpr float     kNearCameraCm   = 3000.0f;  // the camera actor sits on the boom behind it
constexpr unsigned  kMaxChangeLines = 40;

struct Sample
{
    bool          pcm = false, op = false;
    std::uint8_t  active = 0xFF, current = 0xFF, pcmCurrent = 0xFF, pcmPrevious = 0xFF;
    float         weight = 0.0f;
    std::uint64_t subject = 0;
    bool          namesMember = false, resolves = false;
    bool          hasMember = false, hasCamera = false, hasSubjectLocation = false;
    float         member[3]{}, camera[3]{}, subjectLocation[3]{}, pivot[3]{};
    bool          poseFinite = false, persistentFinite = false;
    std::uint8_t  changeActive = 0xFF, changeRunning = 0xFF;
    bool          changeTargetsMember = false;
    bool          leaderIsMember = false;

    float SubjectToMember() const noexcept { return hasSubjectLocation && hasMember ? Distance(subjectLocation, member) : -1.0f; }
    float CameraToMember() const noexcept { return hasCamera && hasMember ? Distance(camera, member) : -1.0f; }
    bool  SubjectNear() const noexcept { const float d = SubjectToMember(); return d >= 0.0f && d < kNearSubjectCm; }
    bool  CameraNear() const noexcept { const float d = CameraToMember(); return d >= 0.0f && d < kNearCameraCm; }

    /// What a change line is written for (positions only through "near").
    auto Key() const noexcept
    {
        return std::make_tuple(pcm, op, active, current, pcmCurrent, namesMember, resolves, poseFinite, persistentFinite,
                               changeActive, changeRunning, changeTargetsMember, leaderIsMember, SubjectNear(), CameraNear());
    }

    static float Distance(const float (&a)[3], const float (&b)[3]) noexcept
    {
        const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
        return std::sqrt(x * x + y * y + z * z);
    }
};

// Game thread.
bool          g_armed    = false;
void*         g_member   = nullptr;
std::uint64_t g_weak     = 0;  // the member as a weak pointer value, to compare with the subject
float         g_elapsed  = 0.0f;
unsigned      g_frames   = 0;
unsigned      g_lines    = 0;
int           g_subjectAt  = -1;  // the first frame the subject location was with the member
float         g_loggedPivot[3]{};  // the pivot on the last line written
bool          g_hasLoggedPivot = false;
int           g_cameraAt = -1;
bool          g_verdict  = false;  // written; then only the after-samples of a camera left behind
float         g_nextAfter = 0.0f;
Sample        g_last;

template <class T>
bool Read(const void* object, std::uint32_t offset, T& out) noexcept
{
    return object != nullptr && platform::CopyGuarded(&out, static_cast<const std::byte*>(object) + offset, sizeof(T));
}

bool Finite(const float* values, std::size_t count) noexcept
{
    for (std::size_t i = 0; i < count; ++i)
    {
        if (!std::isfinite(values[i]))
        {
            return false;
        }
    }
    return true;
}

Sample Take() noexcept
{
    Sample s;
    void* pcm = ReadPointer(generation::PlayerTwoController(), facts::kControllerCameraManager);
    s.pcm     = pcm != nullptr;
    s.hasMember = players::unreal::ActorLocation(g_member, s.member);
    if (!s.pcm)
    {
        return s;
    }
    Read(pcm, facts::kCameraManagerCurrentOperator, s.pcmCurrent);
    Read(pcm, kPcmPreviousOperator, s.pcmPrevious);
    s.hasCamera      = players::unreal::ActorLocation(ReadPointer(pcm, facts::kCameraManagerEndCameraActor), s.camera);
    s.leaderIsMember = ReadPointer(pcm, kPcmLeader) == g_member;
    std::uint64_t leaderWeak = 0;
    s.leaderIsMember = s.leaderIsMember || (Read(pcm, kPcmLeaderWeak, leaderWeak) && leaderWeak != 0 && leaderWeak == g_weak);
    s.persistentFinite = true;
    for (const std::uint32_t offset : kPcmPersistent)
    {
        float value = 0.0f;
        s.persistentFinite = s.persistentFinite && Read(pcm, offset, value) && std::isfinite(value);
    }

    void* op = ReadPointer(pcm, facts::kCameraManagerFieldOperator);
    s.op     = op != nullptr;
    if (!s.op)
    {
        return s;
    }
    Read(op, kOpActive, s.active);
    Read(op, kOpCurrent, s.current);
    Read(op, kOpWeight, s.weight);
    Read(op, kOpSubject, s.subject);
    s.namesMember        = s.subject != 0 && s.subject == g_weak;
    s.resolves           = s.namesMember && leader_scope::StillNames(s.subject, g_member);
    s.hasSubjectLocation = Read(op, kOpSubjectLocation, s.subjectLocation);
    Read(op, kOpPivot, s.pivot);
    float pose[kOpPoseFloats]{};
    s.poseFinite = Read(op, kOpPose, pose) && Finite(pose, kOpPoseFloats);

    const void* modules = ReadPointer(op, kOpModules);
    const void* change  = modules == nullptr ? nullptr
                                             : ReadPointer(static_cast<const std::byte*>(modules) + kLeaderChangeSlot * kModuleStride, 0);
    Read(change, kModuleActive, s.changeActive);
    Read(change, kModuleRunning, s.changeRunning);
    std::uint64_t target = 0;
    s.changeTargetsMember = Read(change, kLeaderChangeTarget, target) && target != 0 && target == g_weak;
    return s;
}

const wchar_t* YesNo(bool value) noexcept
{
    return value ? L"yes" : L"no";
}

std::wstring Metres(float cm)
{
    return cm < 0.0f ? std::wstring(L"?") : std::format(L"{:.1f} m", cm / 100.0f);
}

/// `pivotMoved`: how far the pivot moved since the last line (-1 for the first); a pivot that does not move while
/// the member did is the frozen pivot of cause A.
std::wstring Describe(const Sample& s, float pivotMoved)
{
    if (!s.pcm)
    {
        return L"player 2 has no camera manager";
    }
    if (!s.op)
    {
        return std::format(L"no Field operator; camera manager op {} (prev {}); camera→member {}", s.pcmCurrent,
                           s.pcmPrevious, Metres(s.CameraToMember()));
    }
    return std::format(
        L"[A] subject names the member {} (resolves {}; subject {:#x}, member {:#x}) | [B] Field op active {} current {} "
        L"weight {:.2f}, camera manager op {} (prev {}) | [C] pose finite {}, collision/occlusion finite {} | subject "
        L"location→member {}, pivot ({:.0f}, {:.0f}, {:.0f}) moved {} since the last line, camera→member {}; member ({:.0f}, "
        L"{:.0f}, {:.0f}), camera ({:.0f}, {:.0f}, {:.0f}) | leader-change module active {} running {} targets the member {}; "
        L"camera manager's leader is the member {}",
        YesNo(s.namesMember), YesNo(s.resolves), s.subject, g_weak, s.active, s.current, s.weight, s.pcmCurrent,
        s.pcmPrevious, YesNo(s.poseFinite), YesNo(s.persistentFinite), Metres(s.SubjectToMember()), s.pivot[0], s.pivot[1],
        s.pivot[2], Metres(pivotMoved), Metres(s.CameraToMember()), s.member[0], s.member[1], s.member[2], s.camera[0],
        s.camera[1], s.camera[2], s.changeActive, s.changeRunning, YesNo(s.changeTargetsMember), YesNo(s.leaderIsMember));
}

void Write(const wchar_t* what, const Sample& s) noexcept
{
    try
    {
        const float moved = g_hasLoggedPivot ? Sample::Distance(s.pivot, g_loggedPivot) : -1.0f;
        platform::Log(L"warp camera: {} (frame {}, {:.2f} s): {}", what, g_frames, g_elapsed, Describe(s, moved));
        std::copy(std::begin(s.pivot), std::end(s.pivot), std::begin(g_loggedPivot));
        g_hasLoggedPivot = true;
    }
    catch (...)
    {
    }
}

void Disarm() noexcept
{
    g_armed  = false;
    g_member = nullptr;
    g_weak   = 0;
}

/// Which of the three causes the sample shows, for the verdict line.
const wchar_t* Suspect(const Sample& s) noexcept
{
    return !s.pcm || !s.op                     ? L"no camera manager or Field operator"
           : !s.namesMember || !s.resolves     ? L"A: the Field operator's subject is not the member"
           : s.current == 0                    ? L"B: the Field operator is not current (the camera actor is not written)"
           : !s.poseFinite || !s.persistentFinite ? L"C: a non-finite value in the pose or the collision/occlusion state"
           : !s.SubjectNear()                    ? L"none of A-C; the subject location is not on the member"
                                               : L"none of A-C; the subject location follows, the camera actor does not";
}

}  // namespace

void Arm(void* member) noexcept
{
    if (member == nullptr || !generation::OnGameThread())
    {
        return;
    }
    g_armed     = true;
    g_member    = member;
    g_weak      = leader_scope::WeakOf(member);
    g_elapsed   = 0.0f;
    g_frames    = 0;
    g_lines     = 0;
    g_subjectAt   = -1;
    g_cameraAt  = -1;
    g_verdict   = false;
    g_nextAfter = 0.0f;
    g_hasLoggedPivot = false;
    g_last      = Take();
    Write(L"armed at the warp request", g_last);
}

void Tick(void* member, float seconds) noexcept
{
    if (!g_armed || !generation::OnGameThread())
    {
        return;
    }
    if (member != g_member)
    {
        platform::Log(L"warp camera: player 2's member changed or left after {:.2f} s; the watch stops", g_elapsed);
        Disarm();
        return;
    }
    ++g_frames;
    g_elapsed += seconds;
    const Sample now = Take();
    if (!g_verdict)
    {
        if (g_subjectAt < 0 && now.SubjectNear() && !g_last.SubjectNear())
        {
            g_subjectAt = static_cast<int>(g_frames);
        }
        if (g_cameraAt < 0 && now.CameraNear() && !g_last.CameraNear())
        {
            g_cameraAt = static_cast<int>(g_frames);
        }
        if (now.Key() != g_last.Key() && g_lines < kMaxChangeLines)
        {
            ++g_lines;
            Write(L"changed", now);
        }
        g_last = now;
        if (g_elapsed < kWatchSeconds)
        {
            return;
        }
        g_verdict = true;
        if (now.SubjectNear() && now.CameraNear())
        {
            platform::Log(L"warp camera: followed (the subject location reached the member at frame {}, the camera actor at frame {}, "
                          L"-1 = never seen apart; {} change line(s))",
                          g_subjectAt, g_cameraAt, g_lines);
            Disarm();
            return;
        }
        platform::Log(L"warp camera: LEFT BEHIND after {:.2f} s; suspect {}", g_elapsed, Suspect(now));
        Write(L"left behind", now);
        g_nextAfter = g_elapsed + 1.0f;
        return;
    }
    if (g_elapsed >= kWatchSeconds + kAfterSeconds)
    {
        Write(now.SubjectNear() && now.CameraNear() ? L"recovered by the end of the watch" : L"still left behind; the watch stops",
              now);
        Disarm();
        return;
    }
    if (g_elapsed >= g_nextAfter)
    {
        g_nextAfter += 1.0f;
        Write(now.SubjectNear() && now.CameraNear() ? L"recovered" : L"still left behind", now);
    }
}

}  // namespace lss::player_two::warp_camera_watch
