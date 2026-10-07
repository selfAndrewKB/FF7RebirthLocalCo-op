#include "player_two/ride_framing_watch.h"

#include "platform/entry_redirect.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "player_two/chocobo_ride.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/object_fields.h"
#include "player_two/primary_controller.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <tuple>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::player_two::ride_framing_watch {

namespace {

// RE-p2-chocobo-warp-camera.md §3, §6 (1.004; 1.005 through replaced-chocobo-cam.json).
// FEndModularCameraOperator::SetModuleSlot15_SetOwner50(op, TSharedPtr<FEndCameraModule>* byValue): the one function
// both slot-15 installs call (PrimaryPCM_FieldOp_InstallModuleSlot15 and its _WithActivateParam variant). Its first
// instruction `mov [rsp+10h], rbx` lies inside one 16-byte block in both builds (+0x4, 1.005 +0x0).
constexpr std::uintptr_t kSetSlotFifteen        = 0x1416E86F4;
constexpr std::uint8_t   kSetSlotFifteenBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                                   0x24, 0x18, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr std::size_t    kFirstInstruction      = 5;
using SetSlotFn = std::uintptr_t (*)(void* op, void* module);

// FEndChocoboCameraModule::Deactivate (vtable +0x18): (module, blend time).
constexpr std::uint8_t kDeactivateBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xEC,
                                             0xA5, 0xCA, 0xFE, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xD9};
constexpr platform::SlotFact kDeactivateSlot{
    L"the chocobo camera module's Deactivate slot (+0x18)", 0x145CE2C68,
    {L"FEndChocoboCameraModule::Deactivate", 0x143404938, kDeactivateBytes}};
using DeactivateFn = std::uintptr_t (*)(void* module, double blendTime);

constexpr std::uintptr_t kChocoboModuleVt = 0x145CE2C50;  // FEndChocoboCameraModule
constexpr std::uint32_t  kOpModules       = 0x6E0;        // the module table, 16 bytes a slot (module pointer first)
constexpr std::uint32_t  kSlotFifteenOffset     = 15 * 16;
constexpr std::uint32_t  kModuleWeight    = 0x40;   // float
constexpr std::uint32_t  kModuleActive    = 0x44;   // u8
constexpr std::uint32_t  kModuleRunning   = 0x45;   // u8: 0 once it has blended out (the operator then drops it)
constexpr std::uint32_t  kModuleOwner     = 0x50;   // the operator SetModuleSlot15 wrote
constexpr std::uint32_t  kChocoboModuleBody = 0x540;  // FWeakObjectPtr: the ride body it follows
constexpr std::uint32_t  kOpPose          = 0x40;   // the pose floats +0x40..+0x98; pitch, yaw, roll at +0x50
constexpr std::uint32_t  kOpPoseFloats    = (0x98 - 0x40) / 4;
constexpr std::uint32_t  kOpPitch         = (0x50 - 0x40) / 4;
constexpr std::uint32_t  kPcmRideSubject  = 0x2CC8;  // FWeakObjectPtr

constexpr unsigned kStackFrames = 8;
constexpr unsigned kInfoLines   = 400;  // then Debug: a slot fought over every frame would otherwise flood the log

struct Slot
{
    bool           op = false;
    const void*    module = nullptr;
    std::uintptr_t vtable = 0;  // live
    std::uint8_t   active = 0, running = 0;
    float          weight = 0.0f;
    bool           owned = false;          // the module names this operator as its owner
    const void*    follows = nullptr;      // a chocobo module's ride body
    const void*    subject = nullptr;      // the camera manager's ride subject
    bool           poseFinite = true;
    float          rotation[3]{};          // pitch, yaw, roll

    auto Key() const noexcept { return std::make_tuple(op, module, vtable, active, running, owned, follows, subject, poseFinite); }
};

// Set once at install.
platform::EntryRedirect             g_setSlot;
platform::VTableEntry               g_deactivate;
std::optional<platform::GameImage>  g_image;
std::uintptr_t                      g_chocoboModuleVt = 0;

std::atomic<unsigned>               g_lines{0};  // the hooks' lines and the tick's

// Game thread.
bool g_seen = false;
Slot g_last[2];

void* FieldOperatorOf(const void* cameraManager) noexcept
{
    return ReadPointer(cameraManager, facts::kCameraManagerFieldOperator);
}

void* CameraManagerOf(const void* controller) noexcept
{
    return ReadPointer(controller, facts::kControllerCameraManager);
}

const void* SlotModule(const void* op) noexcept
{
    const void* modules = ReadPointer(op, kOpModules);
    return modules != nullptr ? ReadPointer(modules, kSlotFifteenOffset) : nullptr;
}

Slot Take(const void* cameraManager) noexcept
{
    Slot s;
    const void* op = FieldOperatorOf(cameraManager);
    s.op           = op != nullptr;
    if (!s.op)
    {
        return s;
    }
    s.subject = leader_scope::ObjectAt(static_cast<const std::byte*>(cameraManager) + kPcmRideSubject);
    float pose[kOpPoseFloats]{};
    if (platform::CopyGuarded(pose, static_cast<const std::byte*>(op) + kOpPose, sizeof(pose)))
    {
        for (std::uint32_t i = 0; i < kOpPoseFloats; ++i)
        {
            s.poseFinite = s.poseFinite && std::isfinite(pose[i]);
        }
        for (std::uint32_t i = 0; i < 3; ++i)
        {
            s.rotation[i] = pose[kOpPitch + i];
        }
    }
    s.module = SlotModule(op);
    if (s.module == nullptr)
    {
        return s;
    }
    s.vtable  = VTableOf(s.module);
    s.active  = ReadField<std::uint8_t>(s.module, kModuleActive);
    s.running = ReadField<std::uint8_t>(s.module, kModuleRunning);
    s.weight  = ReadField<float>(s.module, kModuleWeight);
    s.owned   = ReadPointer(s.module, kModuleOwner) == op;
    if (s.vtable == g_chocoboModuleVt)
    {
        s.follows = leader_scope::ObjectAt(static_cast<const std::byte*>(s.module) + kChocoboModuleBody);
    }
    return s;
}

std::uintptr_t Research(std::uintptr_t live) noexcept
{
    return g_image ? g_image->ResearchAddress(live) : live;
}

/// Whom an actor is, for the log. Game thread.
std::wstring Whom(const void* actor)
{
    if (actor == nullptr)
    {
        return L"none";
    }
    const void* one = generation::PlayerOneController();
    const void* two = generation::PlayerTwoController();
    if (actor == chocobo_ride::RiddenChocobo() || actor == chocobo_ride::DrivenChocobo())
    {
        return L"player 2's chocobo";
    }
    if (actor == generation::Member())
    {
        return L"player 2's member";
    }
    if (one != nullptr && actor == ReadPointer(one, facts::kControllerPawn))
    {
        return L"player 1's pawn";
    }
    if (two != nullptr && actor == ReadPointer(two, facts::kControllerPawn))
    {
        return L"player 2's pawn";
    }
    return std::format(L"another actor {}", actor);
}

std::wstring ModuleText(const void* module, std::uintptr_t vtable)
{
    if (module == nullptr)
    {
        return L"empty";
    }
    return vtable == g_chocoboModuleVt ? std::wstring(L"chocobo module")
                                       : std::format(L"module vt 0x{:X}", Research(vtable));
}

std::wstring Text(const Slot& s)
{
    if (!s.op)
    {
        return L"no Field operator";
    }
    std::wstring slot = ModuleText(s.module, s.vtable);
    if (s.module != nullptr)
    {
        slot += std::format(L" {} (active {} running {} weight {:.2f}, owner {})", s.module, s.active, s.running, s.weight,
                            s.owned ? L"this operator" : L"OTHER");
        if (s.vtable == g_chocoboModuleVt)
        {
            slot += std::format(L", follows {}", Whom(s.follows));
        }
    }
    return std::format(L"slot 15 {}; ride subject {}; pose finite {} (pitch {:.1f} yaw {:.1f} roll {:.1f})", slot,
                       Whom(s.subject), s.poseFinite ? L"yes" : L"NO", s.rotation[0], s.rotation[1], s.rotation[2]);
}

/// Which camera an operator belongs to.
const wchar_t* CameraOf(const void* op) noexcept
{
    if (op == nullptr)
    {
        return L"no operator";
    }
    if (op == FieldOperatorOf(CameraManagerOf(generation::PlayerOneController())))
    {
        return L"player 1's camera";
    }
    if (op == FieldOperatorOf(CameraManagerOf(generation::PlayerTwoController())))
    {
        return L"player 2's camera";
    }
    return generation::Active() ? L"another camera operator" : L"the camera (no player 2)";
}

const wchar_t* ScopeText() noexcept
{
    if (!generation::OnGameThread())
    {
        return L"off the game thread";
    }
    if (!generation::Active())
    {
        return L"no player 2";
    }
    return primary_controller::Is(generation::PlayerTwoController()) ? L"in player 2's primary scope"
                                                                      : L"outside player 2's scope";
}

/// The calls that led here, as research addresses (a mod frame shows as its live address). `frames` from
/// RtlCaptureStackBackTrace, the first being the return into the hooked function's caller.
std::wstring StackText(void* const* frames, unsigned count)
{
    std::wstring text;
    for (unsigned i = 0; i < count; ++i)
    {
        text += std::format(L"{}0x{:X}", i == 0 ? L"" : L" <- ", Research(reinterpret_cast<std::uintptr_t>(frames[i])));
    }
    return text.empty() ? std::wstring(L"?") : text;
}

template <class... Args>
void Line(std::wformat_string<Args...> fmt, Args&&... args) noexcept
{
    const bool info = g_lines.fetch_add(1, std::memory_order_relaxed) < kInfoLines;
    platform::LogAt(info ? platform::Level::Info : platform::Level::Debug, fmt, std::forward<Args>(args)...);
}

std::uintptr_t SetSlotFifteen(void* op, void* module) noexcept
{
    void* frames[kStackFrames]{};
    const unsigned count  = RtlCaptureStackBackTrace(1, kStackFrames, frames, nullptr);
    const void*    before = SlotModule(op);
    const auto     beforeVt = VTableOf(before);
    const void*    after  = ReadPointer(module, 0);  // the TSharedPtr's object; the call releases the TSharedPtr
    const auto     afterVt = VTableOf(after);
    const auto     result = reinterpret_cast<SetSlotFn>(g_setSlot.Original())(op, module);
    try
    {
        Line(L"ride framing: {}'s slot 15: {} -> {} (installed {}); calls: {}", CameraOf(op), ModuleText(before, beforeVt),
             ModuleText(after, afterVt), ScopeText(), StackText(frames, count));
    }
    catch (...)
    {
    }
    return result;
}

std::uintptr_t DeactivateChocoboModule(void* module, double blendTime) noexcept
{
    void* frames[kStackFrames]{};
    const unsigned count   = RtlCaptureStackBackTrace(1, kStackFrames, frames, nullptr);
    const void*    op      = ReadPointer(module, kModuleOwner);
    const bool     onGame  = generation::OnGameThread();  // the weak-pointer resolve is game-thread code
    const void*    follows = onGame ? leader_scope::ObjectAt(static_cast<const std::byte*>(module) + kChocoboModuleBody)
                                    : nullptr;  // read before the call, which clears it
    const auto     result  = reinterpret_cast<DeactivateFn>(g_deactivate.Original())(module, blendTime);
    try
    {
        Line(L"ride framing: {}'s chocobo module {} deactivated (it followed {}; {}); calls: {}", CameraOf(op), module,
             onGame ? Whom(follows) : std::wstring(L"?"), ScopeText(), StackText(frames, count));
    }
    catch (...)
    {
    }
    return result;
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    g_image = image;
    const auto vt     = image.Rebase(kChocoboModuleVt);
    g_chocoboModuleVt = vt ? *vt : 0;
    const std::span<const std::uint8_t> bytes(kSetSlotFifteenBytes);
    const auto at  = image.Rebase(kSetSlotFifteen);
    const bool set = at && image.BytesMatch(kSetSlotFifteen, bytes) &&
                     g_setSlot.Install(*at, image.EntryInstruction(kSetSlotFifteen, bytes.first(kFirstInstruction)),
                                       reinterpret_cast<std::uintptr_t>(&SetSlotFifteen));
    const bool deactivate =
        platform::ReplaceSlot(image, kDeactivateSlot, reinterpret_cast<std::uintptr_t>(&DeactivateChocoboModule), g_deactivate);
    platform::Log(L"ride framing: watch installed (read-only): slot-15 installs {}, chocobo module deactivations {}, "
                  L"both cameras' slot 15 {}",
                  set ? L"logged" : L"NOT logged (the function's bytes differ)",
                  deactivate ? L"logged" : L"NOT logged (the vtable entry differs)",
                  g_chocoboModuleVt != 0 ? L"logged on change" : L"NOT named (the module vtable is not found)");
}

void Remove() noexcept
{
    if (!g_setSlot.Restore())
    {
        platform::Log(L"ride framing: the slot-15 setter was changed after us; left in place (it passes through)");
    }
    if (!g_deactivate.Restore())
    {
        platform::Log(L"ride framing: the chocobo module's Deactivate entry could not be restored (it passes through)");
    }
}

void Tick() noexcept
{
    if (!generation::OnGameThread() || !generation::Active())
    {
        g_seen = false;
        return;
    }
    void* const controllers[2] = {generation::PlayerOneController(), generation::PlayerTwoController()};
    for (int p = 0; p < 2; ++p)
    {
        const Slot now = Take(CameraManagerOf(controllers[p]));
        if (g_seen && now.Key() == g_last[p].Key())
        {
            continue;
        }
        try
        {
            Line(L"ride framing: player {}'s camera {}: {}", p + 1, g_seen ? L"changed" : L"now", Text(now));
        }
        catch (...)
        {
        }
        g_last[p] = now;
    }
    g_seen = true;
}

std::wstring Describe(const void* cameraManager)
{
    return Text(Take(cameraManager));
}

}  // namespace lss::player_two::ride_framing_watch
