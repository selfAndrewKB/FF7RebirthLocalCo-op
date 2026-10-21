#include "player_two/location_rows.h"

#include "player_two/world_rules.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/vtable_entry.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"
#include "player_two/primary_controller.h"
#include "players/unreal_access.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
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
constexpr std::uintptr_t kListenerSlot    = 0x1466B6F90;
constexpr std::uintptr_t kListenerForward = 0x141035AC8;
constexpr std::uint8_t   kListenerForwardBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x5E, 0x94, 0x07,
                                                    0x01, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0x49, 0x08};
constexpr std::uint32_t  kListenerOperator = 0x08;
// vtbl_UEndPlayerExistNavAreaComponent TickComponent slot: UpdateNavModifiers from its controller's leader.
constexpr std::uintptr_t kNavAreaTickSlot = 0x1467C8448;
constexpr std::uintptr_t kNavAreaTick     = 0x141662054;
constexpr std::uint8_t   kNavAreaTickBytes[] = {0x40, 0x53, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8, 0xD0, 0xCE,
                                                0xA4, 0x00, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0x41, 0x10};
constexpr std::uint32_t  kNavAreaController = 0xB0;
constexpr std::uint32_t  kBattleOperator    = 0x2CA8;

// Player 2's own location rows (§2.3, a documented recreation of the manager's enter/leave bookkeeping only):
// g_LocationVolumeManager (a pointer), its volume array (+8 data, +0x10 count) and lock (+0x188, a
// CRITICAL_SECTION the game's update holds), each AEndLocationVolume's flags, margin, row and priority, and the
// game's own containment test LocationVolume_ContainsPoint(volume, &position, margin) (a pure read).
constexpr std::uintptr_t kLocationManager    = 0x14939BCA0;
constexpr std::uint32_t  kManagerVolumes     = 0x08;
constexpr std::uint32_t  kManagerVolumeCount = 0x10;
constexpr std::uint32_t  kManagerLock        = 0x188;
constexpr std::uint32_t  kVolumeLocationId   = 0x270;
constexpr std::uint32_t  kVolumeUntouchMargin = 0x278;  // float: the margin the game's test stays inside by
constexpr std::uint32_t  kVolumeFlags        = 0x280;   // bit 1 bDisabled, bit 8 bChangeCamera
constexpr std::uint32_t  kVolumeCameraRow    = 0x2E0;   // FName CameraSettingID
constexpr std::uint32_t  kVolumeCameraPriority = 0x2E8; // lower = higher priority
constexpr std::uint32_t  kFlagDisabled = 0x1, kFlagChangeCamera = 0x8;
constexpr std::int32_t   kMaxVolumes   = 4096;
constexpr std::uintptr_t kContainsPoint = 0x140A9F840;
constexpr std::uint8_t   kContainsPointBytes[] = {0x40, 0x53, 0xB8, 0x50, 0x00, 0x00, 0x00, 0xE8, 0xE4,
                                                  0xF6, 0x60, 0x01, 0x48, 0x2B, 0xE0, 0x33, 0xDB};
constexpr std::uint32_t  kOpSetLocation   = 0xE0;  // op vtable: SetLocationSetting(op, location, row)
constexpr std::uint32_t  kOpClearLocation = 0xE8;  // op vtable: ClearLocationSetting(op, location, row)

constexpr ULONGLONG kPollMs   = 10000;

using ForwardFn = std::uintptr_t (*)(void* listener, std::uintptr_t location, std::uintptr_t row, std::uintptr_t enter);
using TickFn    = void (*)(void* component, float deltaTime, std::uintptr_t tickType, void* tickFunction);
using ContainsFn = bool (*)(const void* volume, const float* position, float margin);
using LocationFn = void (*)(void* op, std::uintptr_t location, std::uintptr_t row);

platform::VTableEntry g_listener;
platform::VTableEntry g_navTick;
std::uintptr_t        g_manager  = 0;  // the address of the manager pointer
ContainsFn            g_contains = nullptr;

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
    playerTwo = false;
    void* const managers[2] = {CameraManager(generation::PlayerOneController()),
                               generation::Active() ? CameraManager(generation::PlayerTwoController()) : nullptr};
    for (int p = 0; p < 2; ++p)
    {
        if (managers[p] == nullptr)
        {
            continue;
        }
        const bool field  = ReadPointer(managers[p], facts::kCameraManagerFieldOperator) == op;
        const bool battle = ReadPointer(managers[p], kBattleOperator) == op;
        if (field || battle)
        {
            playerTwo = p == 1;
            return std::format(L"player {}'s {}", p + 1, field ? L"Field" : L"Battle");
        }
    }
    return L"another";
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

/// The volume player 2's character is in, by the game's own test and rules: a volume that changes the camera and
/// is not disabled; strictly inside to enter, within its UntouchMargin to stay in the current one; the lowest
/// CameraSettingPriority wins. Empty if the manager is busy or unreadable (then nothing changes this tick).
std::optional<Location> PlayerTwoLocation(const float (&position)[3]) noexcept
{
    void* manager = nullptr;
    if (!platform::CopyGuarded(&manager, reinterpret_cast<const void*>(g_manager), sizeof(manager)) || manager == nullptr)
    {
        return std::nullopt;
    }
    // The lock lives in engine memory like any field, so its calls are guarded too (a manager torn down under us).
    auto* lock    = reinterpret_cast<CRITICAL_SECTION*>(static_cast<std::byte*>(manager) + kManagerLock);
    BOOL  entered = FALSE;
    if (!platform::CallGuarded(&TryEnterCriticalSection, entered, lock) || !entered)
    {
        return std::nullopt;
    }
    Location     best;
    std::uint8_t bestPriority = 0xFF;
    bool         readable     = true;
    void* const* volumes = nullptr;
    std::int32_t count   = 0;
    if (!Read(manager, kManagerVolumes, volumes) || !Read(manager, kManagerVolumeCount, count) || count < 0 ||
        count > kMaxVolumes || (count > 0 && volumes == nullptr))
    {
        readable = false;
        count    = 0;
    }
    for (std::int32_t i = 0; i < count; ++i)
    {
        const void*   volume   = nullptr;
        std::uint32_t flags    = 0;
        std::uint8_t  priority = 0xFF;
        float         margin   = 0.0f;
        if (!platform::CopyGuarded(&volume, volumes + i, sizeof(volume)) || volume == nullptr ||
            !Read(volume, kVolumeFlags, flags) || !world_rules::LocationCandidate(flags, kFlagDisabled, kFlagChangeCamera) ||
            !Read(volume, kVolumeCameraPriority, priority))
        {
            continue;
        }
        Location here{volume};
        if (!Read(volume, kVolumeLocationId, here.location) || !Read(volume, kVolumeCameraRow, here.row))
        {
            continue;
        }
        // The volume player 2 is in, matched by its LocationId as well as its address: a streamed-in volume may reuse
        // a streamed-out one's address.
        const bool current = volume == g_playerTwo.volume && here.location == g_playerTwo.location;
        if (!current || !Read(volume, kVolumeUntouchMargin, margin))
        {
            margin = 0.0f;
        }
        bool inside = false;
        if (!platform::CallGuarded(g_contains, inside, volume, &position[0], margin) || !inside ||
            !world_rules::LowerPriority(priority, bestPriority))
        {
            continue;
        }
        best         = here;
        bestPriority = priority;
    }
    if (!platform::CallGuarded(&LeaveCriticalSection, lock))
    {
        readable = false;  // nothing more is safe to conclude from this pass
    }
    return readable ? std::optional<Location>(best) : std::nullopt;
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
    if (g_contains == nullptr || g_manager == 0 || !generation::OnGameThread())
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
    void* const ops[2] = {ReadPointer(pcm, facts::kCameraManagerFieldOperator), ReadPointer(pcm, kBattleOperator)};
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
    const auto listenerSlot = image.Rebase(kListenerSlot, sizeof(void*));
    const auto forward      = image.Rebase(kListenerForward);
    const auto navSlot      = image.Rebase(kNavAreaTickSlot, sizeof(void*));
    const auto navTick      = image.Rebase(kNavAreaTick);
    const bool listener = listenerSlot && forward && image.BytesMatch(kListenerForward, kListenerForwardBytes) &&
                          g_listener.Replace(*listenerSlot, *forward, reinterpret_cast<std::uintptr_t>(&Forward));
    const bool nav = navSlot && navTick && primary_controller::Installed() &&
                     image.BytesMatch(kNavAreaTick, kNavAreaTickBytes) &&
                     g_navTick.Replace(*navSlot, *navTick, reinterpret_cast<std::uintptr_t>(&NavAreaTick));
    const auto manager  = image.Rebase(kLocationManager, sizeof(void*));
    const auto contains = image.Rebase(kContainsPoint);
    // Player 2's own rows check their own two facts only (the gate above is a separate lever).
    if (manager && contains && image.BytesMatch(kContainsPoint, kContainsPointBytes))
    {
        g_manager  = *manager;
        g_contains = reinterpret_cast<ContainsFn>(*contains);
    }
    platform::Log(L"location rows: player 2's camera takes location rows from player 2's own character {}",
                  g_contains != nullptr ? L"yes" : L"NO (a check failed)");
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
