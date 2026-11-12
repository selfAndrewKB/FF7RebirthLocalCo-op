#include "camera/location_rows.h"

#include "camera/camera_access.h"
#include "camera/location_volumes.h"
#include "facts/cameras.h"
#include "facts/controller.h"
#include "facts/world.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/unreal_access.h"
#include "scopes/primary_controller.h"
#include "world/world_rules.h"

#include <algorithm>
#include <array>
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

namespace lss::camera::location_rows {

namespace {

// research/RE-p2-framing.md (this build; slots and entries checked by disassembly).
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

// A seat's own location rows (a documented recreation of the manager's enter/leave bookkeeping only): the
// game's volume list, containment test and the volume's row and priority (camera/location_volumes.h).
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
bool                  g_volumes = false;  // the volume walk's checks passed (camera/location_volumes.h)

/// A seat's location as its own camera holds it.
struct Location
{
    const void*   volume   = nullptr;
    std::uint64_t location = 0;
    std::uint64_t row      = 0;
};
std::array<Location, players::kMaxPlayers> g_seatLocation{};  // game thread, by seat; index 0 unused

// Bumped by the engine's callbacks (the listener broadcast, the nav-area tick) and read by Poll on the game thread:
// atomic, should a callback ever run on another thread. The counts a seat owns are kept per seat (index 0: player 1's
// nav-area ticks); the others in index 0.
enum Count
{
    kForwarded,   // [0]
    kWithheld,    // per seat
    kTicks,       // per seat, [0] player 1's
    kTicksOther,  // [0]
    kScoped,      // per seat
    kRefused,     // per seat
    kCountKinds
};
constexpr int kCountSlots = kCountKinds * players::kMaxPlayers;
std::atomic<unsigned> g_counts[kCountSlots];
unsigned              g_logged[kCountSlots] = {};
ULONGLONG             g_nextPoll            = 0;

constexpr int Slot(Count count, int seat = 0) noexcept
{
    return count * players::kMaxPlayers + seat;
}

void Bump(Count count, int seat = 0) noexcept
{
    g_counts[Slot(count, seat)].fetch_add(1, std::memory_order_relaxed);
}

void Line(const std::wstring& text) noexcept
{
    platform::Log(L"location rows: {}", text);
}

void* CameraManager(void* controller) noexcept
{
    return ReadPointer(controller, facts::kControllerCameraManager);
}

/// "player N's Field/Battle", or "another" for an operator of no player's camera manager.
std::wstring OperatorText(const void* op, int seat)
{
    if (seat < 0)
    {
        return L"another";
    }
    const bool field = ReadPointer(CameraManager(players::generation::ControllerOf(seat)), facts::kCameraManagerFieldOperator) == op;
    return std::format(L"{}'s {}", players::SeatName(seat), field ? L"Field" : L"Battle");
}

/// The row arrives as an FName value (seen live: 0x90A03F, 0x101738), not a pointer to one.
std::wstring RowText(std::uintptr_t row)
{
    return players::unreal::NameText(row);
}

/// Replaces the listeners' slot 2.
std::uintptr_t Forward(void* listener, std::uintptr_t location, std::uintptr_t row, std::uintptr_t enter) noexcept
{
    const void* op     = ReadPointer(listener, kListenerOperator);
    const int   seat   = camera_access::SeatOfOperator(op);
    const bool  aSeats = players::SeatBuilt(seat);
    try
    {
        const std::wstring who = OperatorText(op, seat);
        Line(std::format(L"{} row {} for {} operator: {}", (enter & 0xFF) != 0 ? L"enter" : L"leave", RowText(row), who,
                         aSeats ? std::format(L"withheld (player 1's location, not {}'s)", players::SeatName(seat))
                                : std::wstring(L"forwarded")));
    }
    catch (...)
    {
    }
    if (aSeats)
    {
        Bump(kWithheld, seat);
        return 0;
    }
    Bump(kForwarded);
    return reinterpret_cast<ForwardFn>(g_listener.Original())(listener, location, row, enter);
}

/// Replaces the nav-area component's tick: a seat's controller's runs as the primary player's.
void NavAreaTick(void* component, float deltaTime, std::uintptr_t tickType, void* tickFunction) noexcept
{
    const auto  original   = reinterpret_cast<TickFn>(g_navTick.Original());
    const void* controller = ReadPointer(component, kNavAreaController);
    const int   seat       = players::generation::SeatOfController(controller);
    if (players::SeatBuilt(seat) && players::generation::Active(seat))
    {
        Bump(kTicks, seat);
        const scopes::primary_controller::Scope scope(seat);
        Bump(scope.Held() ? kScoped : kRefused, seat);  // refused: still the game's tick, with the game's primary player
        original(component, deltaTime, tickType, tickFunction);
        return;
    }
    if (controller != nullptr && controller == players::generation::ControllerOf(0))
    {
        Bump(kTicks, 0);
    }
    else
    {
        Bump(kTicksOther);
    }
    original(component, deltaTime, tickType, tickFunction);
}

/// The camera-row pick over the walk's volumes.
struct Pick
{
    const float*    position;
    const Location* current;  // the seat's location as its camera holds it
    Location        best;
    std::uint8_t    bestPriority = 0xFF;
};

void Visit(void* context, const location_volumes::Volume& volume) noexcept
{
    auto*        pick     = static_cast<Pick*>(context);
    std::uint8_t priority = 0xFF;
    float        margin   = 0.0f;
    if (!world::world_rules::LocationCandidate(volume.flags, kFlagDisabled, kFlagChangeCamera) ||
        !Read(volume.volume, kVolumeCameraPriority, priority))
    {
        return;
    }
    Location here{volume.volume, volume.location};
    if (!Read(volume.volume, kVolumeCameraRow, here.row))
    {
        return;
    }
    // The volume the seat is in, matched by its LocationId as well as its address: a streamed-in volume may reuse
    // a streamed-out one's address.
    const bool current = volume.volume == pick->current->volume && here.location == pick->current->location;
    if (!current || !Read(volume.volume, kVolumeUntouchMargin, margin))
    {
        margin = 0.0f;
    }
    bool inside = false;
    const float (&position)[3] = *reinterpret_cast<const float (*)[3]>(pick->position);
    if (!location_volumes::Contains(volume.volume, position, margin, inside) || !inside ||
        !world::world_rules::LowerPriority(priority, pick->bestPriority))
    {
        return;
    }
    pick->best         = here;
    pick->bestPriority = priority;
}

/// The volume a seat's character is in, by the game's own test and rules: a volume that changes the camera and
/// is not disabled; strictly inside to enter, within its UntouchMargin to stay in `current`; the lowest
/// CameraSettingPriority wins. Empty if the manager is busy or unreadable (then nothing changes this tick).
std::optional<Location> SeatLocation(const float (&position)[3], const Location& current) noexcept
{
    Pick                    pick{&position[0], &current};
    location_volumes::Reads reads;
    const auto result = location_volumes::WalkVolumes(false, &Visit, &pick, reads);
    return result == location_volumes::WalkResult::Done ? std::optional<Location>(pick.best) : std::nullopt;
}

/// Calls one of a seat's operators' own location functions (vtable slot).
bool CallOperator(void* op, std::uint32_t slot, std::uint64_t location, std::uint64_t row) noexcept
{
    void* vtable = ReadPointer(op, 0);
    void* fn     = ReadPointer(vtable, slot);
    return fn != nullptr && platform::CallGuarded(reinterpret_cast<LocationFn>(fn), op, location, row);
}

/// One seat's camera follows the location volume its character is in.
void TickSeat(int seat) noexcept
{
    Location& held = g_seatLocation[static_cast<std::size_t>(seat)];
    void*     pcm  = players::generation::Active(seat) ? CameraManager(players::generation::ControllerOf(seat)) : nullptr;
    float     position[3]{};
    if (pcm == nullptr || !players::unreal::ActorLocation(players::generation::MemberOf(seat), position))
    {
        held = {};  // the seat's player out (its operators go with its camera manager) or no member yet
        return;
    }
    const auto now = SeatLocation(position, held);
    if (!now || (now->volume == held.volume && now->location == held.location && now->row == held.row))
    {
        return;
    }
    void* const ops[2] = {ReadPointer(pcm, facts::kCameraManagerFieldOperator),
                          ReadPointer(pcm, kCameraManagerBattleOperator)};
    bool        ok     = true;
    for (void* op : ops)
    {
        ok = op != nullptr && (now->volume != nullptr ? CallOperator(op, kOpSetLocation, now->location, now->row)
                                                      : CallOperator(op, kOpClearLocation, held.location, held.row)) &&
             ok;
    }
    try
    {
        const wchar_t* name = players::SeatName(seat);
        Line(now->volume != nullptr
                 ? std::format(L"{}'s character is in a camera location volume: row {} to {}'s camera{}", name,
                               RowText(now->row), name, ok ? L"" : L" (FAILED for an operator)")
                 : std::format(L"{}'s character left its camera location volume: row {} cleared from {}'s "
                               L"camera{}",
                               name, RowText(held.row), name, ok ? L"" : L" (FAILED for an operator)"));
    }
    catch (...)
    {
    }
    held = *now;
}

}  // namespace

void Tick() noexcept
{
    if (!g_volumes || !players::generation::OnGameThread())
    {
        return;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        TickSeat(seat);
    }
}

bool Install(const platform::GameImage& image) noexcept
{
    const bool listener = platform::ReplaceSlot(image, kListenerSlot, reinterpret_cast<std::uintptr_t>(&Forward),
                                                g_listener);
    const bool nav      = scopes::primary_controller::Installed() &&
                          platform::ReplaceSlot(image, kNavAreaTickSlot, reinterpret_cast<std::uintptr_t>(&NavAreaTick),
                                                g_navTick);
    // The seats' own rows check the volume walk's two facts only (the gate above is a separate lever).
    g_volumes = location_volumes::Install(image);
    platform::Log(L"location rows: player 2's or 3's camera takes location rows from that player's own character {}",
                  g_volumes ? L"yes" : L"NO (a check failed)");
    platform::Log(L"location rows: player 1's location rows withheld from players 2 and 3's cameras {}; each one's indoor "
                  L"areas pushed to its own camera {}",
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
    unsigned counts[kCountSlots];
    for (int i = 0; i < kCountSlots; ++i)
    {
        counts[i] = g_counts[i].load(std::memory_order_relaxed);
    }
    // The nav-area ticks run every frame: a line for a forwarded, withheld or refused call, or when a seat's
    // component starts ticking, not for the frame counts alone.
    bool changed = counts[Slot(kForwarded)] != g_logged[Slot(kForwarded)];
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        changed = changed || counts[Slot(kWithheld, seat)] != g_logged[Slot(kWithheld, seat)] ||
                  counts[Slot(kRefused, seat)] != g_logged[Slot(kRefused, seat)] ||
                  ((counts[Slot(kTicks, seat)] != 0) != (g_logged[Slot(kTicks, seat)] != 0));
    }
    if (!changed)
    {
        return;
    }
    g_nextPoll = now + kPollMs;
    std::copy(std::begin(counts), std::end(counts), std::begin(g_logged));
    try
    {
        std::wstring withheld, ticks;  // per built seat, as the line named player 2 alone
        for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
        {
            const wchar_t* name = players::SeatName(seat);
            withheld += std::format(L"{}{} withheld from {}", seat > 1 ? L", " : L"", counts[Slot(kWithheld, seat)], name);
            ticks += std::format(L"{}'s {} ({} as the primary player, {} refused), ", name, counts[Slot(kTicks, seat)],
                                 counts[Slot(kScoped, seat)], counts[Slot(kRefused, seat)]);
        }
        platform::Log(L"location rows: so far {} forwarded, {}; nav-area ticks: player 1's {}, {}other {}",
                      counts[Slot(kForwarded)], withheld, counts[Slot(kTicks, 0)], ticks, counts[Slot(kTicksOther)]);
    }
    catch (...)
    {
    }
}

}  // namespace lss::camera::location_rows
