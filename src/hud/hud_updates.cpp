#include "hud/hud_updates.h"

#include "facts/hud.h"
#include "hud/field/marker_timers.h"
#include "platform/frame_watch.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/verified_code.h"

#include "players/weak_object.h"

#include <array>
#include <cstdint>
#include <format>

namespace lss::hud::hud_updates {

namespace {

// Facts read from the database (research/RE-p2-hud.md). Each function's prologue up
// to its stack probe call is checked before it is called.

struct SubTick
{
    std::uint32_t          member;    // EndUISystem member holding the object (a pointer)
    platform::FunctionFact function;  // void (object)
};

constexpr std::uint8_t kTick98[]  = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55, 0x57, 0x41,
                                     0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xC9, 0xB8, 0xC0, 0x00, 0x00, 0x00};
constexpr std::uint8_t kTickB0[]  = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C,
                                     0x24, 0x20, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0xB8, 0x40, 0x01, 0x00, 0x00};
constexpr std::uint8_t kTick150[] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
                                     0x48, 0x8D, 0x6C, 0x24, 0xE1, 0xB8, 0xE8, 0x00, 0x00, 0x00};
constexpr std::uint8_t kTick188[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C,
                                     0x24, 0x20, 0x41, 0x56, 0xB8, 0xE0, 0x00, 0x00, 0x00};
constexpr std::uint8_t kTick170[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x48, 0x89, 0x78,
                                     0x18, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x68, 0xA1, 0xB8, 0xA0, 0x00, 0x00, 0x00};
constexpr std::uint8_t kTick180[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C,
                                     0x24, 0x20, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0xB8, 0xB0, 0x01, 0x00, 0x00};

// the off-screen enemy arrows among them (hud/field/marker_timers.h)
constexpr std::uint32_t kMarkerListMember = 0x150;

// In MasterHUD_Tick's order (research/RE-p2-hud.md).
constexpr std::array<SubTick, 6> kSubTicks{{
    {0x98, {L"the HUD's +0x98 object's tick", 0x140A677F0, kTick98}},
    {0xB0, {L"the HUD's +0xB0 object's tick", 0x140B8B374, kTickB0}},
    {0x150, {L"EndUI__MarkerList_Tick", 0x140C8D360, kTick150}},
    {0x188, {L"the HUD's +0x188 object's tick", 0x140F37874, kTick188}},
    {0x170, {L"the HUD's +0x170 object's tick", 0x140F37ACC, kTick170}},
    {0x180, {L"the HUD's +0x180 object's tick", 0x140A68620, kTick180}},
}};

// The leader-target icon: m = *(g+0x140); if *(int32*)(m+0x20) != 0 and
// w = FWeakObjectPtr::Get(m+0x6C) is set, the game's placer is called with w.
constexpr std::uint32_t  kLeaderTargetMember = 0x140;
constexpr std::uint32_t  kLeaderTargetCount  = 0x20;
constexpr std::uint32_t  kLeaderTargetWeak   = 0x6C;
constexpr std::uint8_t   kLeaderTargetPrologue[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48,
                                                    0x89, 0x78, 0x20, 0x55, 0x48, 0x8D, 0x68, 0xA1, 0xB8, 0xA0, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kLeaderTargetTick{L"the leader-target icon's tick", 0x1417267F0,
                                                   kLeaderTargetPrologue};

// The compass (pass 12 item 5): CompassHolder__FrameUpdate(holder, DeltaTime), holder =
// *(*(g+0x108)+0x348). Its heading and scale come from the primary camera manager (L3). Run with a
// DeltaTime of 0 for each player [pass 12: looks safe to re-run, not fully traced].
constexpr std::uint32_t  kCompassHolder     = 0x348;
constexpr std::uint8_t   kCompassFrameUpdatePrologue[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18,
                                                          0x48, 0x89, 0x78, 0x20, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
                                                          0x41, 0x57, 0x48, 0x8D, 0xA8, 0xC8, 0xFD, 0xFF, 0xFF, 0xB8, 0x10,
                                                          0x03, 0x00, 0x00};
constexpr platform::FunctionFact kCompassFrameUpdate{L"the compass holder's FrameUpdate", 0x14122FBA0,
                                                     kCompassFrameUpdatePrologue};

using ObjectFn  = void (*)(void* object);
using CompassFn = void (*)(void* holder, float deltaTime);

constexpr std::size_t kLeaderSlot  = kSubTicks.size();
constexpr std::size_t kCompassSlot = kSubTicks.size() + 1;
constexpr const wchar_t* kNames[]  = {L"+98", L"+B0", L"+150", L"+188", L"+170", L"+180", L"leader", L"compass"};

// Set once at install.
std::uintptr_t          g_endUISystem  = 0;
std::array<ObjectFn, 6> g_subTicks{};
ObjectFn                g_leaderTarget = nullptr;
CompassFn               g_compass      = nullptr;

// Game thread: seconds per update since the last TakeTimings.
std::array<double, kSubTicks.size() + 2> g_seconds{};
using platform::frame_watch::Now;

bool CallGuarded(ObjectFn function, void* object) noexcept
{
    __try
    {
        function(object);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

bool CompassGuarded(void* holder) noexcept
{
    __try
    {
        g_compass(holder, 0.0f);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

void* Member(std::uint32_t member) noexcept
{
    void* object = nullptr;
    platform::CopyGuarded(&object, reinterpret_cast<const void*>(g_endUISystem + member), sizeof(object));
    return object;
}

/// The marker list with a column's own arrow timers and the real time step (hud/field/marker_timers.h).
bool RunMarkerListOwnTimers(ObjectFn tick, void* list, const OwnArrowTimers& arrows) noexcept
{
    const float still = 0.0f;
    marker_timers::SwapIn(arrows.seat, arrows.column);
    platform::WriteGuarded(arrows.timeStep, &arrows.realStep, sizeof(float));
    const bool ran = CallGuarded(tick, list);
    platform::WriteGuarded(arrows.timeStep, &still, sizeof(float));
    marker_timers::SwapOut(arrows.seat, arrows.column);
    return ran;
}

bool RunLists(const OwnArrowTimers* arrows) noexcept
{
    for (std::size_t i = 0; i < kSubTicks.size(); ++i)
    {
        const double start  = Now();
        void*        object = Member(kSubTicks[i].member);
        const bool   own    = arrows != nullptr && arrows->timeStep != nullptr && kSubTicks[i].member == kMarkerListMember;
        if (object != nullptr &&
            !(own ? RunMarkerListOwnTimers(g_subTicks[i], object, *arrows) : CallGuarded(g_subTicks[i], object)))
        {
            return false;
        }
        g_seconds[i] += Now() - start;
    }
    return true;
}

bool RunLeaderTarget() noexcept
{
    const double start  = Now();
    auto*        leader = static_cast<std::uint8_t*>(Member(kLeaderTargetMember));
    std::int32_t count  = 0;
    if (leader != nullptr && platform::CopyGuarded(&count, leader + kLeaderTargetCount, sizeof(count)) && count != 0)
    {
        if (void* target = players::weak_object::ObjectAt(leader + kLeaderTargetWeak);
            target != nullptr && !CallGuarded(g_leaderTarget, target))
        {
            return false;
        }
    }
    g_seconds[kLeaderSlot] += Now() - start;
    return true;
}

bool RunCompass() noexcept
{
    const double start   = Now();
    auto*        manager = static_cast<std::uint8_t*>(Member(facts::kUIAreaMap));
    void*        holder  = nullptr;
    if (manager != nullptr && platform::CopyGuarded(&holder, manager + kCompassHolder, sizeof(holder)) &&
        holder != nullptr && !CompassGuarded(holder))
    {
        return false;
    }
    g_seconds[kCompassSlot] += Now() - start;
    return true;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    bool holds = players::weak_object::Known();
    for (std::size_t i = 0; holds && i < kSubTicks.size(); ++i)
    {
        const auto live = platform::FindFunction(image, kSubTicks[i].function);
        holds           = live.has_value();
        g_subTicks[i]   = live ? reinterpret_cast<ObjectFn>(*live) : nullptr;
    }
    const auto system  = image.Rebase(facts::kEndUISystem, kSubTicks.back().member + sizeof(void*));
    const auto leader  = platform::FindFunction(image, kLeaderTargetTick);
    const auto compass = platform::FindFunction(image, kCompassFrameUpdate);
    if (!holds || !system || !leader || !compass)
    {
        return false;
    }
    g_endUISystem  = *system;
    g_leaderTarget = reinterpret_cast<ObjectFn>(*leader);
    g_compass      = reinterpret_cast<CompassFn>(*compass);
    return true;
}

bool Run(bool lists, bool compass, const OwnArrowTimers* arrows) noexcept
{
    return (!lists || (RunLists(arrows) && RunLeaderTarget())) && (!compass || RunCompass());
}

std::wstring TakeTimings(unsigned frames)
{
    const double perFrame = frames == 0 ? 0.0 : 1000.0 / frames;
    std::wstring text;
    for (std::size_t i = 0; i < g_seconds.size(); ++i)
    {
        text += std::format(L" {} {:.3f}", kNames[i], g_seconds[i] * perFrame);
    }
    g_seconds.fill(0.0);
    return text;
}

}  // namespace lss::hud::hud_updates
