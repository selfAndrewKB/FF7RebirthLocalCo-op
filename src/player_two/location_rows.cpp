#include "player_two/location_rows.h"

#include "facts/cameras.h"
#include "facts/controller.h"
#include "facts/world.h"
#include "platform/verified_code.h"
#include "player_two/world_rules.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/vtable_entry.h"
#include "player_two/camera_access.h"
#include "player_two/generation.h"
#include "player_two/location_volumes.h"
#include "player_two/object_fields.h"
#include "player_two/primary_controller.h"
#include "players/unreal_access.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::player_two::location_rows {

namespace {

// RE-p2-framing.md "Live finding" §1.1, §2.3, §3.3 (this build; slots and entries checked by disassembly).
// vtbl_OpLocationListener slot 2: OpLocationListener_OnCameraSetting_ForwardToOp(listener, location, row, enter):
// op = *(listener+8); enter -> op vt+0xE0 (set layer 5), else op vt+0xE8 (clear layer 5).
constexpr std::uint8_t kListenerForwardBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x5E, 0x94, 0x07,
                                                  0x01, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0x49, 0x08};
constexpr platform::SlotFact kListenerSlot{L"OpLocationListener's OnCameraSetting slot (2)", 0x1466B6F90,
                                           {L"OpLocationListener_OnCameraSetting_ForwardToOp", 0x141035AC8,
                                            kListenerForwardBytes}};
constexpr std::uint32_t  kListenerOperator = 0x08;
// vtbl_UEndPlayerExistNavAreaComponent TickComponent slot: UpdateNavModifiers from its controller's leader.
constexpr std::uint8_t kNavAreaTickBytes[] = {0x40, 0x53, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8, 0xD0, 0xCE,
                                              0xA4, 0x00, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0x41, 0x10};
constexpr platform::SlotFact kNavAreaTickSlot{L"UEndPlayerExistNavAreaComponent's TickComponent slot", 0x1467C8448,
                                              {L"EndPlayerExistNavAreaComponent_TickComponent", 0x141662054,
                                               kNavAreaTickBytes}};
using location_volumes::kNavAreaController;
using facts::kCameraManagerBattleOperator;

// Player 2's own location rows (§2.3, a documented recreation of the manager's enter/leave bookkeeping only): the
// game's volume list, containment test and the volume's row and priority (player_two/location_volumes.h).
using location_volumes::kFlagChangeCamera;
using location_volumes::kFlagDisabled;
using location_volumes::kVolumeCameraPriority;
using location_volumes::kVolumeCameraRow;
using location_volumes::kVolumeUntouchMargin;
constexpr std::uint32_t  kOpSetLocation   = 0xE0;  // op vtable: SetLocationSetting(op, location, row)
constexpr std::uint32_t  kOpClearLocation = 0xE8;  // op vtable: ClearLocationSetting(op, location, row)

constexpr ULONGLONG kPollMs   = 10000;

using ForwardFn = std::uintptr_t (*)(void* listener, std::uintptr_t location, std::uintptr_t row, std::uintptr_t enter);
using TickFn    = void (*)(void* component, float deltaTime, std::uintptr_t tickType, void* tickFunction);
using LocationFn = void (*)(void* op, std::uintptr_t location, std::uintptr_t row);

platform::VTableEntry g_listener;
platform::VTableEntry g_navTick;
bool                  g_volumes = false;  // the volume walk's checks passed (player_two/location_volumes.h)

/// Player 2's location as player 2's own camera holds it.
struct Location
{
    const void*   volume   = nullptr;
    std::uint64_t location = 0;
    std::uint64_t row      = 0;
};
Location g_playerTwo;  // game thread

// Bumped by the engine's callbacks (the listener broadcast, the nav-area tick) and read by Poll on the game thread:
// atomic, should a callback ever run on another thread.
enum Count
{
    kForwarded,
    kWithheld,
    kTicksOne,
    kTicksTwo,
    kTicksOther,
    kScoped,
    kRefused,
    kCountKinds
};
std::atomic<unsigned> g_counts[kCountKinds];
unsigned              g_logged[kCountKinds] = {};
ULONGLONG             g_nextPoll            = 0;

void Bump(Count count) noexcept
{
    g_counts[count].fetch_add(1, std::memory_order_relaxed);
}

void Line(const std::wstring& text) noexcept
{
    platform::Log(L"location rows: {}", text);
}

void* CameraManager(void* controller) noexcept
{
    return ReadPointer(controller, facts::kControllerCameraManager);
}

/// "player N's Field/Battle", or "another" for an operator of neither camera manager.
std::wstring OperatorText(const void* op, bool& playerTwo) noexcept
{
    const int p = camera_access::PlayerOfOperator(op);
    playerTwo   = p == 1;
    if (p < 0)
    {
        return L"another";
    }
    void* const controller = p == 1 ? generation::PlayerTwoController() : generation::PlayerOneController();
    const bool  field      = ReadPointer(CameraManager(controller), facts::kCameraManagerFieldOperator) == op;
    return std::format(L"player {}'s {}", p + 1, field ? L"Field" : L"Battle");
}

/// The row arrives as an FName value (seen live: 0x90A03F, 0x101738), not a pointer to one.
std::wstring RowText(std::uintptr_t row)
{
    return players::unreal::NameText(row);
}

/// Replaces the listeners' slot 2.
std::uintptr_t Forward(void* listener, std::uintptr_t location, std::uintptr_t row, std::uintptr_t enter) noexcept
{
    bool        playerTwo = false;
    const void* op        = ReadPointer(listener, kListenerOperator);
    try
    {
        const std::wstring who = OperatorText(op, playerTwo);
        Line(std::format(L"{} row {} for {} operator: {}", (enter & 0xFF) != 0 ? L"enter" : L"leave", RowText(row), who,
                         playerTwo ? L"withheld (player 1's location, not player 2's)" : L"forwarded"));
    }
    catch (...)
    {
    }
    if (playerTwo)
    {
        Bump(kWithheld);
        return 0;
    }
    Bump(kForwarded);
    return reinterpret_cast<ForwardFn>(g_listener.Original())(listener, location, row, enter);
}

/// Replaces the nav-area component's tick: player 2's controller's runs as the primary player's.
void NavAreaTick(void* component, float deltaTime, std::uintptr_t tickType, void* tickFunction) noexcept
{
    const auto  original   = reinterpret_cast<TickFn>(g_navTick.Original());
    const void* controller = ReadPointer(component, kNavAreaController);
    if (generation::Active() && controller != nullptr && controller == generation::PlayerTwoController())
    {
        Bump(kTicksTwo);
        const primary_controller::Scope scope;
        Bump(scope.Held() ? kScoped : kRefused);  // refused: still the game's tick, with the game's primary player
        original(component, deltaTime, tickType, tickFunction);
        return;
    }
    Bump(controller != nullptr && controller == generation::PlayerOneController() ? kTicksOne : kTicksOther);
    original(component, deltaTime, tickType, tickFunction);
}

/// The camera-row pick over the walk's volumes.
struct Pick
{
    const float* position;
    Location     best;
    std::uint8_t bestPriority = 0xFF;
};

void Visit(void* context, const location_volumes::Volume& volume) noexcept
{
    auto*        pick     = static_cast<Pick*>(context);
    std::uint8_t priority = 0xFF;
    float        margin   = 0.0f;
    if (!world_rules::LocationCandidate(volume.flags, kFlagDisabled, kFlagChangeCamera) ||
        !Read(volume.volume, kVolumeCameraPriority, priority))
    {
        return;
    }
    Location here{volume.volume, volume.location};
    if (!Read(volume.volume, kVolumeCameraRow, here.row))
    {
        return;
    }
    // The volume player 2 is in, matched by its LocationId as well as its address: a streamed-in volume may reuse
    // a streamed-out one's address.
    const bool current = volume.volume == g_playerTwo.volume && here.location == g_playerTwo.location;
    if (!current || !Read(volume.volume, kVolumeUntouchMargin, margin))
    {
        margin = 0.0f;
    }
    bool inside = false;
    const float (&position)[3] = *reinterpret_cast<const float (*)[3]>(pick->position);
    if (!location_volumes::Contains(volume.volume, position, margin, inside) || !inside ||
        !world_rules::LowerPriority(priority, pick->bestPriority))
    {
        return;
    }
    pick->best         = here;
    pick->bestPriority = priority;
}

/// The volume player 2's character is in, by the game's own test and rules: a volume that changes the camera and
/// is not disabled; strictly inside to enter, within its UntouchMargin to stay in the current one; the lowest
/// CameraSettingPriority wins. Empty if the manager is busy or unreadable (then nothing changes this tick).
std::optional<Location> PlayerTwoLocation(const float (&position)[3]) noexcept
{
    Pick                    pick{&position[0]};
    location_volumes::Reads reads;
    const auto result = location_volumes::WalkVolumes(false, &Visit, &pick, reads);
    return result == location_volumes::WalkResult::Done ? std::optional<Location>(pick.best) : std::nullopt;
}

/// Calls one of player 2's operators' own location functions (vtable slot).
bool CallOperator(void* op, std::uint32_t slot, std::uint64_t location, std::uint64_t row) noexcept
{
    void* vtable = ReadPointer(op, 0);
    void* fn     = ReadPointer(vtable, slot);
    return fn != nullptr && platform::CallGuarded(reinterpret_cast<LocationFn>(fn), op, location, row);
}

}  // namespace

void Tick() noexcept
{
    if (!g_volumes || !generation::OnGameThread())
    {
        return;
    }
    void* pcm = generation::Active() ? CameraManager(generation::PlayerTwoController()) : nullptr;
    float position[3]{};
    if (pcm == nullptr || !players::unreal::ActorLocation(generation::Member(), position))
    {
        g_playerTwo = {};  // player 2 out (its operators go with its camera manager) or no member yet
        return;
    }
    const auto now = PlayerTwoLocation(position);
    if (!now ||
        (now->volume == g_playerTwo.volume && now->location == g_playerTwo.location && now->row == g_playerTwo.row))
    {
        return;
    }
    void* const ops[2] = {ReadPointer(pcm, facts::kCameraManagerFieldOperator),
                          ReadPointer(pcm, kCameraManagerBattleOperator)};
    bool        ok     = true;
    for (void* op : ops)
    {
        ok = op != nullptr && (now->volume != nullptr ? CallOperator(op, kOpSetLocation, now->location, now->row)
                                                      : CallOperator(op, kOpClearLocation, g_playerTwo.location, g_playerTwo.row)) &&
             ok;
    }
    try
    {
        Line(now->volume != nullptr
                 ? std::format(L"player 2's character is in a camera location volume: row {} to player 2's camera{}",
                               RowText(now->row), ok ? L"" : L" (FAILED for an operator)")
                 : std::format(L"player 2's character left its camera location volume: row {} cleared from player 2's "
                               L"camera{}",
                               RowText(g_playerTwo.row), ok ? L"" : L" (FAILED for an operator)"));
    }
    catch (...)
    {
    }
    g_playerTwo = *now;
}

bool Install(const platform::GameImage& image) noexcept
{
    const bool listener = platform::ReplaceSlot(image, kListenerSlot, reinterpret_cast<std::uintptr_t>(&Forward),
                                                g_listener);
    const bool nav      = primary_controller::Installed() &&
                          platform::ReplaceSlot(image, kNavAreaTickSlot, reinterpret_cast<std::uintptr_t>(&NavAreaTick),
                                                g_navTick);
    // Player 2's own rows check the volume walk's two facts only (the gate above is a separate lever).
    g_volumes = location_volumes::Install(image);
    platform::Log(L"location rows: player 2's camera takes location rows from player 2's own character {}",
                  g_volumes ? L"yes" : L"NO (a check failed)");
    platform::Log(L"location rows: player 1's location rows withheld from player 2's camera {}; player 2's indoor "
                  L"areas pushed to player 2's camera {}",
                  listener ? L"yes" : L"NO (a check failed)", nav ? L"yes" : L"NO (a check failed)");
    return listener || nav;
}

void Remove() noexcept
{
    for (auto* entry : {&g_listener, &g_navTick})
    {
        if (entry->Original() != 0 && !entry->Restore())
        {
            platform::Log(L"location rows: a slot was changed after us; left in place (it passes through)");
        }
    }
}

void Poll() noexcept
{
    const ULONGLONG now = GetTickCount64();
    if (now < g_nextPoll)
    {
        return;
    }
    unsigned counts[kCountKinds];
    for (int i = 0; i < kCountKinds; ++i)
    {
        counts[i] = g_counts[i].load(std::memory_order_relaxed);
    }
    // The nav-area ticks run every frame: a line for a forwarded, withheld or refused call, or when player 2's
    // component starts ticking, not for the frame counts alone.
    const bool changed = counts[kForwarded] != g_logged[kForwarded] || counts[kWithheld] != g_logged[kWithheld] ||
                         counts[kRefused] != g_logged[kRefused] || ((counts[kTicksTwo] != 0) != (g_logged[kTicksTwo] != 0));
    if (!changed)
    {
        return;
    }
    g_nextPoll = now + kPollMs;
    std::copy(std::begin(counts), std::end(counts), std::begin(g_logged));
    try
    {
        platform::Log(L"location rows: so far {} forwarded, {} withheld from player 2; nav-area ticks: player 1's {}, "
                      L"player 2's {} ({} as the primary player, {} refused), other {}",
                      counts[kForwarded], counts[kWithheld], counts[kTicksOne], counts[kTicksTwo], counts[kScoped],
                      counts[kRefused], counts[kTicksOther]);
    }
    catch (...)
    {
    }
}

}  // namespace lss::player_two::location_rows
