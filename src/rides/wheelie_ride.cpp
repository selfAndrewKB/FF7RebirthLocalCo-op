#include "rides/wheelie_ride.h"

#include "facts/cameras.h"
#include "facts/controller.h"
#include "facts/engine.h"
#include "facts/in_battle.h"
#include "facts/move_state.h"
#include "facts/party.h"
#include "facts/vehicles.h"
#include "input/member_movement.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/every.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/actor_fields.h"
#include "players/describe.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "players/unreal_access.h"
#include "players/weak_object.h"
#include "rides/own_ride.h"
#include "rides/ride_rules.h"
#include "scopes/leader_scope.h"
#include "scopes/primary_controller.h"
#include "session/party_member.h"
#include "world/charapop_facts.h"

#include <intrin.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>

namespace lss::rides::wheelie_ride {

namespace {

using Clock = std::chrono::steady_clock;

// The game's functions (research/RE-p2-wheelie-ride.md). All Square Enix End code.
constexpr std::uint8_t kShouldRideBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x18, 0x48,
                                             0x89, 0x78, 0x20, 0x55, 0x48, 0x8D, 0x68, 0xA1, 0xB8, 0xD0, 0x00, 0x00};
constexpr platform::FunctionFact kShouldRide{L"SBScooterCoordinator_ShouldRideAuto_Vt2F8", 0x140E88050, kShouldRideBytes};

constexpr std::uint8_t kIsRidingBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xCC, 0x79, 0xA0, 0x01,
                                           0x48, 0x2B, 0xE0, 0xE8, 0xA8, 0x01, 0x00, 0x00, 0x48, 0x8B, 0xD8, 0x48};
constexpr platform::FunctionFact kIsRiding{L"PrimaryPC_IsActiveVehicleRiding", 0x1407641A8, kIsRidingBytes};

constexpr std::uint8_t kInputBasisBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x2C, 0x19, 0x1D, 0x01,
                                             0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xD9, 0xE8, 0x89, 0xA1, 0x7C, 0xFF, 0x48};
constexpr platform::FunctionFact kInputBasis{L"PrimaryPCM_GetInputBasisRotation_2FA8OrVt718", 0x140F9A248, kInputBasisBytes};

constexpr std::uint8_t kLookupBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89,
                                         0x74, 0x24, 0x18, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xC6, 0x72};
constexpr platform::FunctionFact kLookup{L"EndPC_LookupMap5C0", 0x1407648A0, kLookupBytes};

using facts::kGetOrCreateVehicleCoordinator;

constexpr std::uint8_t kDispatchBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8,
                                           0xF0, 0x80, 0x70, 0xFE, 0x48, 0x2B, 0xE0, 0x8A, 0x01, 0x48, 0x8B, 0xD9};
constexpr platform::FunctionFact kDispatch{L"Vehicle_BeginByType_Dispatch", 0x143A63A80, kDispatchBytes};

constexpr std::uint8_t kBeginBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57,
                                        0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8};
constexpr platform::FunctionFact kBegin{L"SBScooterCoordinator_Begin_SetActive", 0x143B1C234, kBeginBytes};

constexpr std::uint8_t kFlushBytes[] = {0xB8, 0x38, 0x00, 0x00, 0x00, 0xE8, 0xAE, 0xAF, 0x9A, 0xFF, 0x48, 0x2B,
                                        0xE0, 0x48, 0x8D, 0x41, 0x48, 0x48, 0x83, 0xC1, 0x08, 0x48, 0x89, 0x44};
constexpr platform::FunctionFact kFlush{L"SBScooterCoordinator_SM_FlushPending", 0x1427C0BC8, kFlushBytes};

constexpr std::uint8_t kRequestGetOffBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x68, 0x41, 0x65, 0xFE,
                                                0x48, 0x2B, 0xE0, 0x4C, 0x8D, 0x99, 0x18, 0x02, 0x00, 0x00, 0x48, 0x8B};
constexpr platform::FunctionFact kRequestGetOff{L"SBScooterCoordinator_RequestGetOff", 0x143B17A0C, kRequestGetOffBytes};

constexpr std::uint8_t kGetOffPrimaryBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8,
                                                0xCC, 0x45, 0xA8, 0x00, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xD9, 0xE8, 0x31};
constexpr platform::FunctionFact kGetOffPrimary{L"Vehicle_RequestGetOff_PrimaryActive", 0x1416E75A4, kGetOffPrimaryBytes};

constexpr std::uint8_t kCoordTickBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8,
                                            0x60, 0x2A, 0x2E, 0x01, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xD9, 0x48, 0x81};
constexpr platform::FunctionFact kCoordTick{L"SBScooterCoordinator_Tick_Vt280", 0x140E89110, kCoordTickBytes};

constexpr std::uint8_t kSwapBackBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8,
                                           0x50, 0x6A, 0x65, 0xFE, 0x48, 0x2B, 0xE0, 0x65, 0x48, 0x8B, 0x04, 0x25};
constexpr platform::SlotFact kSwapBackSlot{
    L"the scooter coordinator's getoff swap-back slot (vt+0x2F0)", 0x145BD6E48,
    {L"SBScooterCoordinator_Vt2F0_FinishGetOff_SwapBack", 0x143B15120, kSwapBackBytes}};

// The single-player animated getoff's swap back: the getoff motion's "End DetachFromSBScooter" notify
// on the rider's mesh. It detaches the rider, then calls `[PM+0x140]+0x5B8`'s vt+0x2F0 only when the rider is the party
// data's field leader. Its function is shared with the base detach class and the chocobo's; only the scooter class's
// slot is replaced.
constexpr std::uint8_t kDetachNotifyBytes[] = {0x48, 0x85, 0xD2, 0x0F, 0x84, 0x9E, 0x03, 0x00, 0x00, 0x48, 0x89, 0x5C,
                                               0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55, 0x57, 0x41, 0x54, 0x41};
constexpr platform::SlotFact kDetachNotifySlot{
    L"the End DetachFromSBScooter notify's Notify slot", 0x14574D9B8,
    {L"AnimNotify_DetachFromRideObject_Notify_FieldLeaderSwapBack", 0x14302A2A4, kDetachNotifyBytes}};
constexpr std::uintptr_t kDetachLeaderSite = 0x14302A610;  // its field-leader test: call ResolveFieldLeaderCharOrLeader

using facts::kSwitchAiControlAsync;
using facts::kScooterUpdateTarget;

// The key-4 "should ride" request FieldSummonBody builds, zeroed: type, character, world stick, FName out.
struct alignas(16) RideAsk
{
    std::uint8_t  type = 0;
    std::uint8_t  pad0[7]{};
    void*         character = nullptr;
    float         stick[4]{};
    std::uint64_t out = 0;
    std::uint8_t  pad1[0x18]{};
};
static_assert(offsetof(RideAsk, character) == 0x08 && offsetof(RideAsk, stick) == 0x10 && offsetof(RideAsk, out) == 0x20);

// The dispatch request, zeroed: type +0, FName +4 (unaligned), +0xD = 1 as FieldSummonBody passes.
struct alignas(16) BeginRequest
{
    std::uint8_t bytes[0x70]{};
};
constexpr std::uint32_t kBeginName    = 0x04;
constexpr std::uint32_t kBeginFlagD   = 0x0D;

// The 16-byte getoff parameters: type filter +0 (4 = scooter), instant +2.
struct alignas(8) GetOffParams
{
    std::uint8_t bytes[16]{};
};
constexpr std::uint32_t kGetOffType    = 0;
constexpr std::uint32_t kGetOffInstant = 2;

using ShouldRideFn    = std::uint8_t (*)(void* coordinator, RideAsk* ask);
using IsRidingFn      = std::uint8_t (*)();
using InputBasisFn    = float* (*)(float* rotator);
using LookupFn        = void* (*)(void* controller, std::uint8_t key);
using DispatchFn      = void (*)(BeginRequest* request);
using BeginFn         = void (*)(void* coordinator, void* scooter, void* rider, bool instant, bool applyNow);
using FlushFn         = void (*)(void* stateMachine, void* coordinator);
using RequestGetOffFn = void (*)(void* coordinator, const GetOffParams* params);
using GetOffPrimaryFn = std::uint8_t (*)(const GetOffParams* params);
using CoordTickFn     = void (*)(void* coordinator, float deltaTime);
using SwapBackFn      = std::uintptr_t (*)(void* coordinator, void* a2, void* a3, void* a4);  // rcx; the rest passed on
using SwitchAiFn      = void (*)(void* character, std::uint8_t toPlayer, void* controller);
using NotifyFn        = void (*)(void* notify, void* mesh, void* a3, void* a4);  // rcx, rdx; the rest passed on
using ResolveLeaderFn = void* (*)(std::uint8_t selector);
using UpdateTargetFn  = void (*)(void* shared);
using TopUIFn         = void (*)(void* coordinator);

// Objects and fields (this build).
constexpr std::uint8_t   kScooterKey        = 4;
using facts::kScooterCoordinatorVTable;
using facts::kScooterCharacterVTable;
constexpr std::uintptr_t kScooterMoveVt     = 0x146547858;  // SBScooterMoveState
using facts::kControllerActiveVehicle;
using facts::kCoordinatorBody;
using facts::kScooterCoordShared;
using facts::kScooterSharedTarget;
using facts::kScooterSharedList;
using facts::kScooterSharedListCount;
using facts::kScooterCoordMarkerState;
using facts::kScooterCoordMarkerTarget;
using facts::kScooterCoordMarkerWidget;
using facts::kScooterListCap;
constexpr std::uint32_t  kCoordInstant      = 0x11C;        // u8, from the request
constexpr std::uint32_t  kCoordApplyNow     = 0x11E;        // u8, from the request
constexpr std::uint32_t  kCoordScripted     = 0x1F8;        // u8: a ride latched for TryBeginRide
constexpr std::uint32_t  kCoordDisabled     = 0x2A;         // u8
constexpr std::uint32_t  kCoordStateMachine = 0x218;
constexpr std::uint32_t  kCoordStatePtr     = 0x230;        // -> u32 state id
constexpr std::uint32_t  kCoordPending      = 0x238;        // pending row (null when none)
constexpr std::int32_t   kStateIdle = 0, kStateBoarding = 2, kStateRiding = 3, kStateGettingOff = 4;
// 1.005 moved these scooter fields by +0x20 (research/RE-1005-offsets-C.md): SetRider writes +0x2338 (1.004 +0x2318);
// FindNearestFreeScooter reads +0x233A (1.004 +0x231A); ShouldRideAuto_Vt2F8 loads MovementSetting from +0x1890
// (1.004 +0x1870).
using facts::kScooterRidden;
using facts::kScooterExcluded;
constexpr platform::PerBuild<std::uint32_t> kScooterSetting  = {0x1890, 0x1870};  // MovementSetting*: +0x394 distance, +0x398 max yaw, +0x39C min power
constexpr std::uint32_t  kSettingDistance   = 0x394;
constexpr std::uint32_t  kSettingMaxYaw     = 0x398;
constexpr std::uint32_t  kSettingMinPower   = 0x39C;
using facts::kCharacterControlHolder;
constexpr std::uint32_t  kHolderMoveRequest = 0x18;         // FVector: SetMoveInput's dir * scale
constexpr std::uint32_t  kStateStickX       = 0x10;         // the on-foot state's stick pair (slot 11 TickInput)
constexpr std::uint32_t  kStateStickY       = 0x14;
constexpr float          kMoveEpsilon       = 1e-4f;        // FieldSummonBody's test
constexpr float          kWarpDistance      = 5000.0f;      // the rental-return warp: > 50 m from the leader
constexpr float          kNearTarget        = 300.0f;       // the ask's Debug line: the seat's member within 3 m of the mark
constexpr std::uint32_t  kMeshOwner         = 0xA0;         // a component's owner (the notify's mesh -> the rider)
constexpr std::uint32_t  kActorRoot         = 0x138;        // an actor's root component
using facts::kSceneComponentAttachParent;

constexpr auto     kSummaryEvery = std::chrono::seconds(10);
constexpr int      kIdleTicks    = 4;          // the coordinator's own ticks run to bring it to idle on a forced end
constexpr float    kIdleTickTime = 1.0f / 30.0f;

// The game's forced getoffs: each caller of Vehicle_RequestGetOff_PrimaryActive that is not the driver's own
// button and can reach the scooter. The call is followed by the same call for each riding seat's coordinator.
struct Site
{
    std::uintptr_t  address;
    const wchar_t*  what;
};
constexpr std::array<Site, 14> kForcedSites = {{
    {0x1410DA1BB, L"the field step"},
    {0x140956BFF, L"a fast travel"},
    {0x1415DC2EC, L"an interaction (A)"},
    {0x1415DC50B, L"an interaction (D)"},
    {0x1433443CC, L"an interaction (B)"},
    {0x1433445C8, L"an interaction (C)"},
    {0x14274A8AB, L"an event"},
    {0x14320CFB6, L"an event node (1)"},
    {0x14320D042, L"an event node (2)"},
    {0x14320D106, L"an event node (3)"},
    {0x14320D227, L"an event node (4)"},
    {0x14320D2EB, L"an event node (5)"},
    {0x143283B58, L"the Wheelie level command"},
    {0x143283C0D, L"a level command"},
}};
constexpr std::uintptr_t kOwnGetOffSite = 0x143170A3F;  // the driver's getoff button, scooter slot 16

struct Functions
{
    ShouldRideFn    shouldRide    = nullptr;
    IsRidingFn      isRiding      = nullptr;
    InputBasisFn    inputBasis    = nullptr;
    LookupFn        lookup        = nullptr;
    LookupFn        getOrCreate   = nullptr;
    DispatchFn      dispatch      = nullptr;
    BeginFn         begin         = nullptr;
    FlushFn         flush         = nullptr;
    RequestGetOffFn requestGetOff = nullptr;
    GetOffPrimaryFn getOffPrimary = nullptr;
    CoordTickFn     coordTick     = nullptr;
    SwitchAiFn      switchAi      = nullptr;
    UpdateTargetFn  updateTarget  = nullptr;  // the coordinator's target search (null: the seats mark nothing of their own)
    TopUIFn         requestTopUI  = nullptr;  // the coordinator's distance counter: its asset request and its close
    TopUIFn         closeTopUI    = nullptr;
};

Functions             g_fn;
std::uintptr_t        g_coordVt = 0, g_scooterVt = 0, g_moveVt = 0;
SwapBackFn            g_swapBackOriginal = nullptr;
platform::VTableEntry g_swapBackSlot;
NotifyFn              g_detachOriginal = nullptr;
platform::VTableEntry g_detachSlot;
ResolveLeaderFn       g_resolveLeader = nullptr;
bool                  g_installed = false;
void*                 g_detachFor  = nullptr;  // a seat's member, while her detach notify runs in the seat's scope
int                   g_detachSeat = -1;       // ... that seat (one notify at a time: a nested one passes through)

// Installed from the game thread on the first tick (some of the calls cross a 16-byte block).
std::array<platform::CallRedirect, kForcedSites.size()> g_forcedCalls;
std::array<std::uintptr_t, kForcedSites.size()>        g_forcedSites{};
platform::CallRedirect                                 g_ownCall;
std::uintptr_t                                         g_ownSite   = 0;
platform::CallRedirect                                 g_detachLeaderCall;  // the detach notify's field-leader test
std::uintptr_t                                         g_detachLeaderSite = 0;
platform::CallRedirect                                 g_updateTargetCall;  // the coordinator tick's target search
std::uintptr_t                                         g_updateTargetSite = 0;
platform::CallRedirect                                 g_markerLeaderCall;  // the idle tick's field-leader resolve
std::uintptr_t                                         g_markerLeaderSite = 0;
platform::CallRedirect                                 g_topCloseCall;  // the tick's close of the distance counter
std::uintptr_t                                         g_topCloseSite = 0;
bool                                                   g_sitesTried = false;
bool                                                   g_ownMark    = false;  // both placed: a seat's coordinator marks its own scooter
bool                                                   g_ownTop     = false;  // placed: a seat's coordinator keeps its own distance counter

// Game thread. Per seat, in the seat's record below.
struct Asked
{
    bool          valid   = false;
    void*         member  = nullptr;
    void*         scooter = nullptr;
    std::uint64_t weak    = 0;
    std::uint64_t name    = 0;  // the scooter's Tags[0], as the game's request carries it
};
Asked g_asked;

struct AskLog  // the ask's last Debug line: logged when this changes
{
    const void* target = nullptr;
    bool        close  = false;
    bool        yes    = false;
    bool operator==(const AskLog&) const = default;
};
const void* g_settingsLogged = nullptr;  // the scooter whose direct-ride settings were last logged (any seat's ask)

struct MarkLog  // a seat's coordinator's mark and marker as last logged: logged when this changes
{
    const void*  pick   = nullptr;  // the target the mod picked for it
    const void*  marked = nullptr;  // the scooter its marker is on
    std::uint8_t state  = 0;        // the marker's state
    bool operator==(const MarkLog&) const = default;
};

// The lease (the seat's controller, member, the scooter as `vehicle`, the seat's own scooter coordinator) is
// rides/own_ride's; the rest is this ride's.
struct Ride : own_ride::Lease
{
    std::int32_t   state       = -1;       // the coordinator's state as last logged
    std::uint32_t  swapBacks = 0, reissued = 0;
    std::uint32_t  detaches = 0, leaderAnswers = 0;  // her getoff motion's detach notify; its field-leader test
};

/// One seat's walk-in answer, ride and what it remembers between rides. Game thread.
struct SeatRide
{
    Asked           asked;
    AskLog          askLog;
    MarkLog         markLog;
    bool            markLogged         = false;
    const void*     coordinatorLogged  = nullptr;  // its own coordinator as last logged (made, or found again)
    bool            topLogged          = false;    // its distance counter's widget was seen and said
    bool            markFaulted        = false;    // its target could not be written; logged once
    bool            listCapLogged      = false;    // player 1's list is longer than the cap; logged once
    std::uint32_t   markerAnswers      = 0;        // its coordinator's marker computed for the seat's member
    const void*     heldLogged = nullptr;  // the scooter last refused as still on a player's controller
    bool            active     = false;
    Ride            ride{};
    bool            stopped    = false;  // a ride failed: the seat's player does not board again this session
    platform::Every summary{kSummaryEvery};
};
SeatRide g_seats[players::kMaxPlayers];  // by seat (players/seats.h); index 0, player 1's, unused

/// The seat's record, or null for a number without one.
SeatRide* RideOf(int seat) noexcept
{
    return players::SeatIndex(seat) ? &g_seats[seat] : nullptr;
}

/// The built seat whose active ride `pick` accepts, or -1.
template <class Pick>
int ActiveSeatWhere(Pick&& pick) noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (g_seats[seat].active && pick(g_seats[seat].ride))
        {
            return seat;
        }
    }
    return -1;
}

using players::BySeat;  // a text kept by pointer (an end's reason) naming the seat's player

std::int32_t CoordState(const void* coordinator) noexcept
{
    return own_ride::CoordState(coordinator, g_coordVt, kCoordStatePtr);
}

const wchar_t* StateName(std::int32_t state) noexcept
{
    switch (state)
    {
        case kStateIdle: return L"idle";
        case 1: return L"resume";
        case kStateBoarding: return L"boarding";
        case kStateRiding: return L"riding";
        case kStateGettingOff: return L"getting off";
        default: return L"unread";
    }
}

void* Coordinator(void* controller) noexcept
{
    void* found = nullptr;
    return controller != nullptr && platform::CallGuarded(g_fn.lookup, found, controller, kScooterKey) &&
                   VTableOf(found) == g_coordVt
               ? found
               : nullptr;
}

/// The object the weak at `object` + `offset` names, or null (for no object).
void* WeakAt(const void* object, std::uint32_t offset) noexcept
{
    return object != nullptr ? scopes::leader_scope::ObjectAt(static_cast<const std::byte*>(object) + offset) : nullptr;
}

void* TargetOf(const void* coordinator) noexcept
{
    void* target = WeakAt(ReadPointer(coordinator, kScooterCoordShared), kScooterSharedTarget);
    return VTableOf(target) == g_scooterVt ? target : nullptr;
}

/// The entries of a coordinator's scooter list (player 1's is the one the game fills).
std::int32_t ListCount(const void* coordinator) noexcept
{
    const void* shared = ReadPointer(coordinator, kScooterCoordShared);
    return shared != nullptr ? ReadField<std::int32_t>(shared, kScooterSharedListCount) : 0;
}

/// The scooter the coordinator's marker is on, or null.
void* MarkedOf(const void* coordinator) noexcept
{
    return WeakAt(coordinator, kScooterCoordMarkerTarget);
}

/// The coordinator's marker widget, or null.
void* MarkerWidgetOf(const void* coordinator) noexcept
{
    return WeakAt(coordinator, kScooterCoordMarkerWidget);
}

bool Free(const void* scooter) noexcept
{
    return ReadField<std::uint8_t>(scooter, kScooterRidden) == 0 && ReadField<std::uint8_t>(scooter, kScooterExcluded) == 0;
}

/// Centimetres between two actors, or -1.
float Between(void* a, void* b) noexcept
{
    float at[3]{}, to[3]{};
    return a != nullptr && b != nullptr && players::unreal::ActorLocation(a, at) && players::unreal::ActorLocation(b, to)
               ? world::charapop::Distance(at, to)
               : -1.0f;
}

void* PlayerOneBody() noexcept
{
    return players::actor_fields::Pawn(players::generation::PlayerOneController());
}

/// The game's rental-return warp would take the scooter home during an animated getoff (player 1's leader more than
/// 50 m from it).
bool WarpWouldFire(void* scooter) noexcept
{
    return Between(PlayerOneBody(), scooter) > kWarpDistance;
}

bool CameraModuleOn(const void* controller) noexcept
{
    return own_ride::CameraModule15(controller) != nullptr;
}

/// The actor `actor`'s root is attached to (root +0x138 -> attach parent +0xC0 -> owner +0xA0), or null.
const void* AttachedTo(const void* actor) noexcept
{
    return ReadPointer(ReadPointer(ReadPointer(actor, kActorRoot), kSceneComponentAttachParent), kMeshOwner);
}

const wchar_t* AttachText(const void* actor, const void* scooter) noexcept
{
    const void* parent = AttachedTo(actor);
    return parent == nullptr ? L"nothing" : parent == scooter ? L"the scooter" : L"another actor";
}

std::wstring LeaseText(const Ride& ride)
{
    const float metres = Between(PlayerOneBody(), ride.vehicle);
    return std::format(L"coordinator state {} ({}); {}; the member attached to {}; the scooter {}; {}'s camera "
                       L"slot-15 module {}; {} from player 1",
                       CoordState(ride.coordinator), StateName(CoordState(ride.coordinator)),
                       own_ride::PossessionText(ride, L"scooter"), AttachText(ride.member, ride.vehicle),
                       players::NameOf(ride.vehicle), players::SeatName(ride.seat),
                       CameraModuleOn(ride.controller) ? L"present" : L"empty",
                       metres >= 0.0f ? std::format(L"{:.0f} m", metres / 100.0f) : std::wstring{L"(unread)"});
}

void LogLine(const wchar_t* prefix, const Ride& ride) noexcept
{
    try
    {
        platform::Log(L"wheelie ride: {}: {}", prefix, LeaseText(ride));
    }
    catch (...)
    {
    }
}

using own_ride::InputComponentOf;
using own_ride::ControllerHolds;

// ---- The question (Ask) ----

/// FieldSummonBody's world stick for this state: the stick pair turned by the primary camera's yaw, when the
/// holder's move request is not zero. False when it is zero (the game's request then carries no stick: no ride).
bool WorldStick(const void* state, const void* member, float (&stick)[4]) noexcept
{
    const void* holder = ReadPointer(member, kCharacterControlHolder);
    if (holder == nullptr)
    {
        return false;
    }
    float request[3]{};
    for (int i = 0; i < 3; ++i)
    {
        request[i] = ReadField<float>(holder, kHolderMoveRequest + 4u * static_cast<std::uint32_t>(i));
    }
    if (!ride_rules::RequestMoves(request, kMoveEpsilon))
    {
        return false;
    }
    float  rotator[4]{};  // pitch, yaw, roll
    float* answer = nullptr;
    if (!platform::CallGuarded(g_fn.inputBasis, answer, &rotator[0]) || answer == nullptr)
    {
        return false;
    }
    const auto world = ride_rules::WorldStickOf(ReadField<float>(state, kStateStickX), ReadField<float>(state, kStateStickY),
                                                ReadField<float>(answer, 4));
    stick[0]         = world[0];
    stick[1]         = world[1];
    stick[2]         = 0.0f;
    stick[3]         = 0.0f;
    return true;
}

void LogSettings(void* scooter) noexcept
{
    if (scooter == g_settingsLogged)
    {
        return;
    }
    g_settingsLogged      = scooter;
    const void* setting   = ReadPointer(scooter, kScooterSetting);
    try
    {
        platform::Log(L"wheelie ride: the marked scooter {}; its walk-in settings: within {:.0f} cm, at most {:.1f}° "
                      L"off, stick above {:.2f}",
                      players::NameOf(scooter), ReadField<float>(setting, kSettingDistance), ReadField<float>(setting, kSettingMaxYaw),
                      ReadField<float>(setting, kSettingMinPower));
    }
    catch (...)
    {
    }
}

/// Whose coordinator the walk-in question goes to, for the log.
const wchar_t* AskedOf() noexcept
{
    return g_ownMark ? L"their own coordinator's mark" : L"player 1's coordinator's mark";
}

void LogAsk(SeatRide& s, int seat, const void* target, float distance, bool yes) noexcept
{
    const AskLog now{target, distance >= 0.0f && distance < kNearTarget, yes};
    if (now == s.askLog || !platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    s.askLog = now;
    try
    {
        platform::LogDebug(L"wheelie ride: the walk-in question for {} ({} {}): {}{}", players::SeatName(seat), AskedOf(),
                           target != nullptr ? players::NameOf(target) : std::wstring{L"none"}, yes ? L"yes" : L"no",
                           now.close ? std::format(L" ({}'s member within 3 m of the mark)", players::SeatName(seat))
                                     : std::wstring{});
    }
    catch (...)
    {
    }
}

// ---- The seat's own mark ----

/// The active seat whose own coordinator `pick` accepts, or -1.
template <class Pick>
int SeatWhoseCoordinator(Pick&& pick) noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const void* coordinator = players::generation::Active(seat) ? Coordinator(players::generation::ControllerOf(seat)) : nullptr;
        if (coordinator != nullptr && pick(coordinator))
        {
            return seat;
        }
    }
    return -1;
}

/// The seat whose own coordinator's shared block `shared` is, or -1.
int SeatOfShared(const void* shared) noexcept
{
    return SeatWhoseCoordinator([shared](const void* c) { return ReadPointer(c, kScooterCoordShared) == shared; });
}

/// The active seat whose own coordinator `coordinator` is, or -1.
int SeatOwning(const void* coordinator) noexcept
{
    return coordinator != nullptr ? SeatWhoseCoordinator([coordinator](const void* c) { return c == coordinator; }) : -1;
}

/// Another seat's ride (boarding, riding or getting off) is on `scooter`.
bool AnotherSeatRides(const void* scooter, int seat) noexcept
{
    const int other = ActiveSeatWhere([scooter](const Ride& ride) { return ride.vehicle == scooter; });
    return other >= 1 && other != seat;
}

/// The free scooter of player 1's coordinator's list nearest `member`, as the game's own search picks the one nearest
/// the party leader (the ridden and excluded ones skipped), read without the search's compaction and warp: null for
/// none. Player 1's own ride is skipped from its boarding on; another seat's from its boarding to its getoff.
void* NearestFree(SeatRide& s, int seat, void* member, const void* one) noexcept
{
    const void*        shared  = ReadPointer(one, kScooterCoordShared);
    const auto*        list    = static_cast<const std::byte*>(ReadPointer(shared, kScooterSharedList));
    const std::int32_t entries = ListCount(one);
    const auto         count   = static_cast<std::size_t>(std::clamp(entries, 0, kScooterListCap));
    if (entries > kScooterListCap && !s.listCapLogged)
    {
        s.listCapLogged = true;
        platform::Log(L"wheelie ride: player 1's scooter list holds {} entries; {}'s mark is picked among the first {}",
                      entries, players::SeatName(seat), kScooterListCap);
    }
    if (list == nullptr || count == 0)
    {
        return nullptr;
    }
    const void* oneBody = CoordState(one) >= kStateBoarding ? ReadPointer(one, kCoordinatorBody) : nullptr;
    std::array<void*, kScooterListCap> scooters{};
    std::array<float, kScooterListCap> distances{};
    for (std::size_t i = 0; i < count; ++i)
    {
        void* scooter = scopes::leader_scope::ObjectAt(list + i * sizeof(std::uint64_t));
        const bool fit = scooter != nullptr && VTableOf(scooter) == g_scooterVt && Free(scooter) && scooter != oneBody &&
                         !AnotherSeatRides(scooter, seat);
        scooters[i]  = scooter;
        distances[i] = fit ? Between(member, scooter) : -1.0f;  // an unreadable location reads -1 too
    }
    const int pick = ride_rules::NearestPick(std::span<const float>(distances.data(), count));
    return pick >= 0 ? scooters[static_cast<std::size_t>(pick)] : nullptr;
}

void LogMark(SeatRide& s, int seat, const void* coordinator, void* pick, void* member) noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    const MarkLog now{pick, MarkedOf(coordinator), ReadField<std::uint8_t>(coordinator, kScooterCoordMarkerState)};
    if (s.markLogged && now == s.markLog)
    {
        return;
    }
    s.markLog    = now;
    s.markLogged = true;
    try
    {
        const float metres = pick != nullptr ? Between(member, pick) / 100.0f : -1.0f;
        platform::LogDebug(L"wheelie ride: {}'s coordinator marks {}{}; its icon {} on {} (the icon's character answered "
                           L"{}'s member {} time(s))",
                           players::SeatName(seat), pick != nullptr ? players::NameOf(pick) : std::wstring{L"nothing"},
                           metres >= 0.0f ? std::format(L" ({:.1f} m from their member)", metres) : std::wstring{},
                           now.state == 2 ? L"within reach" : now.state == 1 ? L"out of reach" : L"off",
                           now.marked != nullptr ? players::NameOf(now.marked) : std::wstring{L"nothing"},
                           players::SeatName(seat), s.markerAnswers);
    }
    catch (...)
    {
    }
}

/// A coordinator's target search (its call in the coordinator's tick). For a seat's own coordinator: the free scooter of
/// player 1's list nearest the seat's member becomes its target, written with the game's weak-pointer assignment when
/// it differs (the one recreation of this ride: the header says why); the rest of the game's search, the removal of a
/// dormant ride-on point, has nothing to remove for a seat's block. Player 1's coordinator's call is the game's own
/// search, with its rental-return warp measured from player 1's leader.
void UpdateTarget(void* shared) noexcept
{
    const int seat = g_ownMark && shared != nullptr && players::generation::OnGameThread() ? SeatOfShared(shared) : -1;
    if (seat < 1)
    {
        g_fn.updateTarget(shared);
        return;
    }
    SeatRide&   s           = g_seats[seat];
    void*       member      = players::generation::MemberOf(seat);
    const void* one         = Coordinator(players::generation::PlayerOneController());
    const void* coordinator = Coordinator(players::generation::ControllerOf(seat));
    void*       pick        = member != nullptr && one != nullptr ? NearestFree(s, seat, member, one) : nullptr;
    auto* const field       = reinterpret_cast<std::uint64_t*>(static_cast<std::byte*>(shared) + kScooterSharedTarget);
    if (pick != scopes::leader_scope::ObjectAt(field) && !players::weak_object::Assign(field, pick) && !s.markFaulted)
    {
        s.markFaulted = true;
        platform::Log(L"WARNING wheelie ride: {}'s coordinator's target could not be written; it keeps marking what it "
                      L"marked",
                      players::SeatName(seat));
    }
    LogMark(s, seat, coordinator, pick, member);
}

/// The field-leader resolve of a coordinator's idle tick, the character its marker is computed for. A seat's
/// coordinator ticks inside the seat's input pass, where the seat's controller is the primary one: there it answers
/// the seat's member. Player 1's coordinator's (and any resolve outside a seat's scope) gets the game's answer.
void* MarkerLeader(std::uint8_t selector) noexcept
{
    if (g_ownMark && players::generation::OnGameThread())
    {
        for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
        {
            void* member = players::generation::Active(seat) ? players::generation::MemberOf(seat) : nullptr;
            if (member != nullptr && scopes::primary_controller::Is(players::generation::ControllerOf(seat)))
            {
                ++g_seats[seat].markerAnswers;
                return member;
            }
        }
    }
    return g_resolveLeader(selector);
}

/// The coordinator's distance counter widget (the Wheelie_Top), or null.
void* TopWidgetOf(const void* coordinator) noexcept
{
    return WeakAt(coordinator, facts::kScooterCoordTopWidget);
}

/// The seat's own distance counter, asked of the game by the game's own gate but the list: the game makes a
/// coordinator's counter only while the coordinator's own scooter list is not empty, and a seat's list never is
/// (every scooter registers with player 1's), so the same request the game's tick would make is made here, under the
/// game's own latch (a request made and not yet answered, or refused, is never asked again until a close) and
/// disabled byte; the asset load's callback then makes the widget, hidden, and the seat's own riding tick shows it,
/// writes the save's one total and hides it at the getoff, as player 1's does. Its widget is the seat's view's only
/// (hud/field/wheelie_tops.h). The request's answer is said when the widget is first seen.
void KeepTop(SeatRide& s, int seat, void* coordinator) noexcept
{
    if (!g_ownTop || coordinator == nullptr)
    {
        return;
    }
    if (TopWidgetOf(coordinator) != nullptr)
    {
        if (!s.topLogged)
        {
            s.topLogged = true;
            platform::Log(L"wheelie ride: {}'s own distance counter is made (its widget shows while they ride, in their view "
                          L"only; the number is the save's one total, which every rider's scooter adds to)",
                          players::SeatName(seat));
        }
        return;
    }
    s.topLogged = false;
    if (ReadField<std::uint8_t>(coordinator, facts::kScooterCoordTopLatch) != 0 ||
        ReadField<std::uint8_t>(coordinator, facts::kScooterCoordDisabled) != 0)
    {
        return;  // asked already and not answered yet (or refused: the game's latch holds until a close), or disabled
    }
    const bool asked = platform::CallGuarded(g_fn.requestTopUI, coordinator);
    platform::Log(L"wheelie ride: {}'s own distance counter {}", players::SeatName(seat),
                  asked ? L"asked of the game (its widget follows the asset load)" : L"could NOT be asked of the game (the request faulted)");
}

/// The seat's own distance counter closed with the game's own close (the seat is going): its widget off the overlay,
/// or a request still open answered no more (the close releases the package and clears the game's latch). Nothing
/// without the seat's controller: the counter went with it.
void CloseTop(SeatRide& s, int seat) noexcept
{
    void* coordinator = Coordinator(players::generation::ControllerOf(seat));
    s.topLogged       = false;
    if (!g_ownTop || coordinator == nullptr || !players::generation::OnGameThread())
    {
        return;
    }
    const bool widget = TopWidgetOf(coordinator) != nullptr;
    if (!widget && ReadField<std::uint8_t>(coordinator, facts::kScooterCoordTopLatch) == 0)
    {
        return;  // none, none asked
    }
    const bool closed = platform::CallGuarded(g_fn.closeTopUI, coordinator);
    platform::Log(L"wheelie ride: {}'s own distance counter {}{}", players::SeatName(seat),
                  closed ? L"closed" : L"could NOT be closed (the game's close faulted)",
                  widget ? L"" : L" (its request was still open)");
}

/// The coordinator tick's close of its distance counter while its scooter list is empty: a seat's own coordinator
/// keeps its counter (its list is always empty); every other coordinator's call is the game's.
void TickCloseTopUI(void* coordinator) noexcept
{
    if (players::generation::OnGameThread() && SeatOwning(coordinator) >= 1)
    {
        return;
    }
    g_fn.closeTopUI(coordinator);
}

/// The seat's own coordinator, made with the game's creator when there is none (a new one is logged); null when it
/// cannot be.
void* OwnCoordinator(SeatRide& s, int seat, std::int32_t scootersHere) noexcept
{
    void* const controller  = players::generation::ControllerOf(seat);
    void*       coordinator = Coordinator(controller);
    if (coordinator == nullptr && controller != nullptr &&
        (!platform::CallGuarded(g_fn.getOrCreate, coordinator, controller, kScooterKey) || VTableOf(coordinator) != g_coordVt))
    {
        coordinator = nullptr;
    }
    if (coordinator != s.coordinatorLogged)
    {
        s.coordinatorLogged = coordinator;
        platform::Log(L"wheelie ride: {}'s own scooter coordinator {} ({} scooter(s) registered here with player 1's): "
                      L"it marks the free one nearest {}'s member, and their walk-in question is asked of it",
                      players::SeatName(seat), coordinator != nullptr ? L"ready" : L"COULD NOT BE MADE", scootersHere,
                      players::SeatName(seat));
    }
    KeepTop(s, seat, coordinator);
    return coordinator;
}

/// Once per frame while the seat's player is in and does not ride: their own coordinator, as soon as a parked scooter
/// is registered here, so its mark and icon are there before the first walk-in.
void KeepOwnCoordinator(SeatRide& s, int seat) noexcept
{
    if (!g_ownMark || s.stopped)
    {
        return;
    }
    const std::int32_t scootersHere = ListCount(Coordinator(players::generation::PlayerOneController()));
    if (scootersHere > 0)
    {
        (void)OwnCoordinator(s, seat, scootersHere);
    }
}

// ---- Boarding ----

struct GetOffCall
{
    void*        coordinator = nullptr;
    GetOffParams params{};
    bool         ok = false;
};

void RunGetOff(void* context)
{
    auto* call = static_cast<GetOffCall*>(context);
    call->ok   = platform::CallGuarded(g_fn.requestGetOff, call->coordinator, static_cast<const GetOffParams*>(&call->params));
}

/// The game's getoff on the seat's coordinator inside the seat's scopes. `instant`: without the motion (the swap back
/// runs inside this call).
bool GetOff(const Ride& ride, bool instant) noexcept
{
    GetOffCall call{ride.coordinator};
    call.params.bytes[kGetOffType]    = kScooterKey;
    call.params.bytes[kGetOffInstant] = instant ? 1 : 0;
    return input::member_movement::InPrimaryScope(ride.seat, ride.member, &RunGetOff, &call) && call.ok;
}

struct IdleCall
{
    void* coordinator = nullptr;
    int   ticks       = 0;
};

void RunIdle(void* context)
{
    auto* call = static_cast<IdleCall*>(context);
    while (call->ticks < kIdleTicks && CoordState(call->coordinator) != kStateIdle &&
           platform::CallGuarded(g_fn.coordTick, call->coordinator, kIdleTickTime))
    {
        ++call->ticks;
    }
}

/// On a forced end: the coordinator's own tick (as the seat's controller's input pass runs it) until it is idle, so
/// its state-4 exit (the active coordinator cleared, the on/off counters put down) runs while the seat's scopes hold.
void DriveToIdle(const Ride& ride) noexcept
{
    IdleCall call{ride.coordinator};
    input::member_movement::InPrimaryScope(ride.seat, ride.member, &RunIdle, &call);
    platform::Log(L"wheelie ride: {}'s coordinator ticked {} time(s) toward idle: state {} ({})",
                  players::SeatName(ride.seat), call.ticks, CoordState(ride.coordinator),
                  StateName(CoordState(ride.coordinator)));
}

/// The ride is over for the mod: the seat's controller must hold the member again. If the game's getoff did not give
/// her back, the game's possession call; if even that fails, stopped for the session.
void Finish(SeatRide& s, const wchar_t* why) noexcept
{
    const Ride ride = s.ride;
    s.active        = false;
    const bool held = own_ride::GiveBack(ride, g_fn.switchAi, L"wheelie ride", L"the getoff");
    try
    {
        const wchar_t* const name = players::SeatName(ride.seat);
        platform::Log(L"wheelie ride: {} is off the scooter ({}; {} ticks driven, {} refused, {} swap-back(s) in {}'s "
                      L"scope, {} detach notify(s), {} forced getoff(s) followed): {}",
                      name, why, ride.ticks, ride.refused, ride.swapBacks, name, ride.detaches, ride.reissued,
                      LeaseText(ride));
        if (ride.swapBacks == 0)
        {
            platform::Log(L"WARNING wheelie ride: the ride ended without the game's swap back for {} (the scooter and "
                          L"{}'s ride camera may stay as they were)",
                          name, name);
        }
    }
    catch (...)
    {
    }
    if (!held)
    {
        s.stopped = true;
        own_ride::LogNotHeld(ride.seat, L"wheelie ride");
    }
}

/// Ends the seat's ride now (a give-back, a leave, a map load, the member changed): the game's instant getoff inside
/// the seat's scopes, then the coordinator's own ticks to idle.
void End(SeatRide& s, const wchar_t* why) noexcept
{
    if (!s.active)
    {
        return;
    }
    Ride&              ride  = s.ride;
    const std::int32_t state = CoordState(ride.coordinator);
    if (!scopes::leader_scope::StillNames(ride.weak, ride.vehicle))
    {
        s.stopped = true;
        Finish(s, L"the scooter is gone; the game's getoff skipped, rides stopped for this session");
        return;
    }
    if (state == kStateRiding || state == kStateBoarding)
    {
        // Boarding: the game's own forced getoffs skip this state; an instant getoff ends the mount too [ASSUMPTION].
        const bool ok = GetOff(ride, true);
        platform::Log(L"wheelie ride: {} while {}'s coordinator was {}: the game's instant getoff in {}'s scope {} "
                      L"(then {}; member controller {}, scooter controller {})",
                      why, players::SeatName(ride.seat), StateName(state), players::SeatName(ride.seat),
                      ok ? L"ran" : L"COULD NOT RUN", StateName(CoordState(ride.coordinator)),
                      players::Who(players::actor_fields::EffectiveController(ride.member)),
                      players::Who(players::actor_fields::EffectiveController(ride.vehicle)));
    }
    else if (state == kStateGettingOff)
    {
        platform::Log(L"wheelie ride: {} during {}'s getoff animation: the coordinator finishes it on its own", why,
                      players::SeatName(ride.seat));
    }
    if (CoordState(ride.coordinator) != kStateIdle && CoordState(ride.coordinator) >= 0)
    {
        DriveToIdle(ride);
    }
    Finish(s, why);
}

// ---- The game's calls, followed for each seat ----

struct SwapCall
{
    void*          coordinator = nullptr;
    void*          a2          = nullptr;
    void*          a3          = nullptr;
    void*          a4          = nullptr;
    std::uintptr_t returned    = 0;
    bool           ok          = false;
};

void RunSwapBack(void* context)
{
    // Guarded: the animated getoff's end reaches here from the engine's own tick, outside any guard of the mod.
    auto* call = static_cast<SwapCall*>(context);
    call->ok   = platform::CallGuarded(g_swapBackOriginal, call->returned, call->coordinator, call->a2, call->a3, call->a4);
}

/// Every scooter coordinator's vt+0x2F0, the getoff's swap back: the animated getoff runs it from the
/// motion's end, outside every scope, where the possession swap would leave a seat's member to her AI. For a seat's
/// coordinator (the seat found from it) it runs inside that seat's scopes (as player 1's runs as the primary player);
/// others pass through.
std::uintptr_t SwapBack(void* coordinator, void* a2, void* a3, void* a4)
{
    // Off the game thread nothing of the mod's state is read.
    const int seat = coordinator != nullptr && players::generation::OnGameThread()
                         ? ActiveSeatWhere([coordinator](const Ride& ride) { return ride.coordinator == coordinator; })
                         : -1;
    if (seat >= 1)
    {
        SeatRide&            s    = g_seats[seat];
        const wchar_t* const name = players::SeatName(seat);
        SwapCall             call{coordinator, a2, a3, a4};
        if (input::member_movement::InPrimaryScope(seat, s.ride.member, &RunSwapBack, &call))
        {
            ++s.ride.swapBacks;
            if (!call.ok)
            {
                s.stopped = true;  // the next tick ends the ride (rides/wheelie_ride.h, Tick); no new ride
                platform::Log(L"WARNING wheelie ride: the getoff's swap back raised {} in {}'s scope; rides stopped for "
                              L"this session",
                              platform::DescribeFault(platform::LastFault()).text, name);
            }
            LogLine(!call.ok ? L"after the faulted swap back"
                             : BySeat(seat, L"the getoff's swap back ran in player 2's scope",
                                      L"the getoff's swap back ran in player 3's scope"),
                    s.ride);
            return call.returned;
        }
        platform::Log(L"WARNING wheelie ride: the getoff's swap back could not hold {}'s scope; it runs as the game "
                      L"would (the member may be left to her AI)",
                      name);
    }
    return g_swapBackOriginal(coordinator, a2, a3, a4);
}

const wchar_t* SiteName(std::uintptr_t returnAddress) noexcept
{
    for (std::size_t i = 0; i < kForcedSites.size(); ++i)
    {
        if (g_forcedSites[i] != 0 && returnAddress == g_forcedSites[i] + 5)
        {
            return kForcedSites[i].what;
        }
    }
    return L"an unlisted caller";
}

/// One seat's follow of a forced getoff: the same getoff for its riding coordinator inside its scope, with the
/// caller's parameters (its instant choice kept; made instant when the rental-return warp would fire).
void FollowForcedGetOff(int seat, const GetOffParams* params, std::uintptr_t from) noexcept
{
    SeatRide&            s     = g_seats[seat];
    const wchar_t* const name  = players::SeatName(seat);
    const std::uint8_t   type  = params != nullptr ? params->bytes[kGetOffType] : 0;
    const std::int32_t   state = CoordState(s.ride.coordinator);
    const auto           plan  = ride_rules::ForcedGetOffPlanOf(
        type, state, kScooterKey, kStateRiding, params != nullptr && params->bytes[kGetOffInstant] != 0,
        [&s] { return WarpWouldFire(s.ride.vehicle); });
    if (plan == ride_rules::ForcedGetOff::Skip)
    {
        platform::Log(L"wheelie ride: the game's forced getoff from {} not followed for {} (type {}, {}'s "
                      L"coordinator {}; the game ends rides only while riding)",
                      SiteName(from), name, type, name, StateName(state));
        return;
    }
    GetOffCall call{s.ride.coordinator};
    if (params != nullptr)
    {
        call.params = *params;
    }
    const bool warp = plan == ride_rules::ForcedGetOff::FollowInstant;
    if (warp)
    {
        call.params.bytes[kGetOffInstant] = 1;
    }
    const bool ran = input::member_movement::InPrimaryScope(seat, s.ride.member, &RunGetOff, &call) && call.ok;
    ++s.ride.reissued;
    platform::Log(L"wheelie ride: the game's forced getoff from {} (instant {}{}) followed for {}: {}", SiteName(from),
                  call.params.bytes[kGetOffInstant], warp ? L", made instant: player 1 is more than 50 m away" : L"",
                  name, ran ? L"ran" : L"COULD NOT RUN");
}

/// A forced getoff's call of Vehicle_RequestGetOff_PrimaryActive: the game's call as it is (it reaches the
/// primary controller's ride), then the same getoff for each riding seat's coordinator in turn, in seat order, each
/// inside its own scope (FollowForcedGetOff). Called inside a seat's scope, nothing follows: the game's call reached
/// that seat's coordinator, and player 1's ride was not the one ended.
std::uint8_t ForcedGetOff(const GetOffParams* params)
{
    const auto         from   = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const std::uint8_t result = g_fn.getOffPrimary(params);
    if (!players::generation::OnGameThread())
    {
        return result;
    }
    // Called inside a seat's scope (a seat's player is the primary one): the game's call reached that seat's ride and
    // player 1's was not ended, so no other seat follows (the old rule's meaning).
    if (scopes::primary_controller::OpenSeat() != 0)
    {
        return result;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (g_seats[seat].active)
        {
            FollowForcedGetOff(seat, params, from);
        }
    }
    return result;
}

/// The driver's getoff button (scooter slot 16's call): player 1's passes through; a seat's (inside the seat's scope:
/// the seat whose controller is primary) is made instant when player 1 is more than 50 m from the scooter, where the
/// game's rental-return warp would take it home during the getoff animation.
std::uint8_t OwnGetOff(const GetOffParams* params)
{
    const int seat = params != nullptr && players::generation::OnGameThread()
                         ? ActiveSeatWhere([](const Ride& ride) { return scopes::primary_controller::Is(ride.controller); })
                         : -1;
    if (seat >= 1 && params->bytes[kGetOffInstant] == 0 && CoordState(g_seats[seat].ride.coordinator) == kStateRiding &&
        WarpWouldFire(g_seats[seat].ride.vehicle))
    {
        GetOffParams instant = *params;
        instant.bytes[kGetOffInstant] = 1;
        platform::Log(L"wheelie ride: {}'s getoff made instant: player 1 is {:.0f} m from the scooter (the rental-return "
                      L"warp would take it home during the animation)",
                      players::SeatName(seat), Between(PlayerOneBody(), g_seats[seat].ride.vehicle) / 100.0f);
        return g_fn.getOffPrimary(&instant);
    }
    return g_fn.getOffPrimary(params);
}

/// The detach notify's field-leader test: while a seat's member's own notify runs in the seat's scope, she
/// is the rider it asks about, so it answers her; otherwise the game's answer (the party data's field leader).
void* NotifyLeader(std::uint8_t selector)
{
    if (g_detachFor != nullptr && players::generation::OnGameThread())
    {
        if (SeatRide* s = RideOf(g_detachSeat))
        {
            ++s->ride.leaderAnswers;
        }
        return g_detachFor;
    }
    return g_resolveLeader(selector);
}

struct DetachCall
{
    int   seat   = 0;  // whose member's notify
    void* notify = nullptr;
    void* mesh   = nullptr;
    void* a3     = nullptr;
    void* a4     = nullptr;
    bool  ok     = false;
};

void RunDetach(void* context)
{
    // Guarded: the notify runs from the member's mesh, inside the engine's animation update, outside any guard of the mod.
    auto* call   = static_cast<DetachCall*>(context);
    g_detachFor  = g_seats[call->seat].ride.member;
    g_detachSeat = call->seat;
    call->ok     = platform::CallGuarded(g_detachOriginal, call->notify, call->mesh, call->a3, call->a4);
    g_detachFor  = nullptr;
    g_detachSeat = -1;
}

/// The getoff motion's "End DetachFromSBScooter" notify. For a seat's member (the seat from the
/// mesh's owner) while her coordinator is getting off, the game's notify runs in the seat's scope with her as the field
/// leader it tests for: it detaches her, then calls the seat's controller's active coordinator's swap back, as player
/// 1's notify does for player 1. Everything else passes through.
void DetachNotify(void* notify, void* mesh, void* a3, void* a4)
{
    // Off the game thread nothing of the mod's state is read: the game's notify as it is.
    const void* owner = mesh != nullptr && players::generation::OnGameThread() && g_detachFor == nullptr
                            ? ReadPointer(mesh, kMeshOwner)
                            : nullptr;
    const int   seat  = owner != nullptr ? ActiveSeatWhere([owner](const Ride& ride) { return ride.member == owner; })
                                         : -1;
    if (seat >= 1)
    {
        SeatRide&            s     = g_seats[seat];
        Ride&                ride  = s.ride;
        const wchar_t* const name  = players::SeatName(seat);
        const std::int32_t   state = CoordState(ride.coordinator);
        if (state == kStateGettingOff)
        {
            const wchar_t*      before  = AttachText(ride.member, ride.vehicle);
            const std::uint32_t swaps   = ride.swapBacks;
            const std::uint32_t answers = ride.leaderAnswers;
            DetachCall          call{seat, notify, mesh, a3, a4};
            const bool          held = input::member_movement::InPrimaryScope(seat, ride.member, &RunDetach, &call);
            ++ride.detaches;
            if (held && !call.ok)
            {
                s.stopped = true;  // the next tick ends the ride (Tick); no new ride
                platform::Log(L"WARNING wheelie ride: the getoff's detach notify raised {} in {}'s scope; rides stopped "
                              L"for this session",
                              platform::DescribeFault(platform::LastFault()).text, name);
            }
            if (!held)
            {
                platform::Log(L"WARNING wheelie ride: the getoff's detach notify could not hold {}'s scope; it runs as "
                              L"the game would (no swap back for {})",
                              name, name);
                g_detachOriginal(notify, mesh, a3, a4);
            }
            try
            {
                platform::Log(L"wheelie ride: {}'s getoff motion reached its detach notify (in {}'s scope: {}): "
                              L"attached to {} before; the field-leader test answered her {} time(s); the swap back {}: {}",
                              name, name, held ? L"yes" : L"NO", before, ride.leaderAnswers - answers,
                              ride.swapBacks > swaps ? L"ran" : L"DID NOT RUN", LeaseText(ride));
            }
            catch (...)
            {
            }
            return;
        }
        platform::Log(L"wheelie ride: a detach notify for {}'s member while her coordinator is {}: passed through", name,
                      StateName(state));
    }
    g_detachOriginal(notify, mesh, a3, a4);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto find   = [&image](const platform::FunctionFact& fact) { return own_ride::FindOrZero(image, fact); };
    const auto rebase = [&image](std::uintptr_t ida) { return own_ride::RebaseOrZero(image, ida); };
    g_fn.shouldRide    = reinterpret_cast<ShouldRideFn>(find(kShouldRide));
    g_fn.isRiding      = reinterpret_cast<IsRidingFn>(find(kIsRiding));
    g_fn.inputBasis    = reinterpret_cast<InputBasisFn>(find(kInputBasis));
    g_fn.lookup        = reinterpret_cast<LookupFn>(find(kLookup));
    g_fn.getOrCreate   = reinterpret_cast<LookupFn>(find(kGetOrCreateVehicleCoordinator));
    g_fn.dispatch      = reinterpret_cast<DispatchFn>(find(kDispatch));
    g_fn.begin         = reinterpret_cast<BeginFn>(find(kBegin));
    g_fn.flush         = reinterpret_cast<FlushFn>(find(kFlush));
    g_fn.requestGetOff = reinterpret_cast<RequestGetOffFn>(find(kRequestGetOff));
    g_fn.getOffPrimary = reinterpret_cast<GetOffPrimaryFn>(find(kGetOffPrimary));
    g_fn.coordTick     = reinterpret_cast<CoordTickFn>(find(kCoordTick));
    g_fn.switchAi      = reinterpret_cast<SwitchAiFn>(find(kSwitchAiControlAsync));
    g_fn.updateTarget  = reinterpret_cast<UpdateTargetFn>(find(kScooterUpdateTarget));
    g_fn.requestTopUI  = reinterpret_cast<TopUIFn>(find(facts::kScooterRequestTopUI));
    g_fn.closeTopUI    = reinterpret_cast<TopUIFn>(find(facts::kScooterCloseTopUI));
    g_topCloseSite     = rebase(facts::kScooterTickCloseTopUICall);
    g_coordVt          = rebase(kScooterCoordinatorVTable);
    g_scooterVt        = rebase(kScooterCharacterVTable);
    g_moveVt           = rebase(kScooterMoveVt);
    for (std::size_t i = 0; i < kForcedSites.size(); ++i)
    {
        g_forcedSites[i] = rebase(kForcedSites[i].address);
    }
    g_ownSite          = rebase(kOwnGetOffSite);
    g_detachLeaderSite = rebase(kDetachLeaderSite);
    g_updateTargetSite = rebase(facts::kScooterUpdateTargetCall);
    g_markerLeaderSite = rebase(facts::kScooterMarkerLeaderCall);
    g_resolveLeader    = reinterpret_cast<ResolveLeaderFn>(find(facts::kResolveFieldLeader));
    const auto swap    = platform::FindFunction(image, kSwapBackSlot.target);
    g_swapBackOriginal = swap ? reinterpret_cast<SwapBackFn>(*swap) : nullptr;
    const auto detach  = platform::FindFunction(image, kDetachNotifySlot.target);
    g_detachOriginal   = detach ? reinterpret_cast<NotifyFn>(*detach) : nullptr;
    const bool functions = g_fn.shouldRide != nullptr && g_fn.isRiding != nullptr && g_fn.inputBasis != nullptr &&
                           g_fn.lookup != nullptr && g_fn.getOrCreate != nullptr && g_fn.dispatch != nullptr &&
                           g_fn.begin != nullptr && g_fn.flush != nullptr && g_fn.requestGetOff != nullptr &&
                           g_fn.getOffPrimary != nullptr && g_fn.coordTick != nullptr && g_fn.switchAi != nullptr &&
                           facts::InBattleKnown() && g_swapBackOriginal != nullptr && g_detachOriginal != nullptr &&
                           g_resolveLeader != nullptr;
    g_installed = functions && g_coordVt != 0 && g_scooterVt != 0 && g_moveVt != 0 && scopes::leader_scope::Installed() &&
                  platform::ReplaceSlot(image, kSwapBackSlot, reinterpret_cast<std::uintptr_t>(&SwapBack), g_swapBackSlot);
    // The animated getoff's swap back for a seat: without it the member, the scooter and the seat's camera are
    // left on the ride, so no ride is offered.
    g_installed = g_installed && platform::ReplaceSlot(image, kDetachNotifySlot, reinterpret_cast<std::uintptr_t>(&DetachNotify),
                                                       g_detachSlot);
    if (!g_installed)
    {
        (void)g_swapBackSlot.Restore();  // nothing uses them without a ride (they pass everything through anyway)
        (void)g_detachSlot.Restore();
    }
    platform::Log(L"wheelie ride: {}", g_installed ? L"installed (player 2 or 3 boards a Wheelie by walking into a marked "
                                                     L"scooter, with their own coordinator; their own mark follows at the "
                                                     L"first tick)"
                                                   : L"UNAVAILABLE (a check failed; players 2 and 3 cannot board, as before)");
    return g_installed;
}

void Remove() noexcept
{
    if (!g_swapBackSlot.Restore())
    {
        platform::Log(L"wheelie ride: could not restore the swap-back slot; the replacement stays in place");
    }
    if (!g_detachSlot.Restore())
    {
        platform::Log(L"wheelie ride: could not restore the detach notify's slot; the replacement stays in place");
    }
    if (g_detachLeaderCall.IsInstalled() && !g_detachLeaderCall.Restore())
    {
        platform::Log(L"wheelie ride: the detach notify's leader-test call was changed after us; left in place (it passes "
                      L"calls through)");
    }
    for (auto& call : g_forcedCalls)
    {
        if (call.IsInstalled() && !call.Restore())
        {
            platform::Log(L"wheelie ride: a forced-getoff call was changed after us; left in place (it passes calls through)");
        }
    }
    if (g_ownCall.IsInstalled() && !g_ownCall.Restore())
    {
        platform::Log(L"wheelie ride: the getoff button's call was changed after us; left in place (it passes calls through)");
    }
    if ((g_updateTargetCall.IsInstalled() && !g_updateTargetCall.Restore()) ||
        (g_markerLeaderCall.IsInstalled() && !g_markerLeaderCall.Restore()))
    {
        platform::Log(L"wheelie ride: a mark call was changed after us; left in place (it passes calls through)");
    }
    if (g_topCloseCall.IsInstalled() && !g_topCloseCall.Restore())
    {
        platform::Log(L"wheelie ride: the counter's close call was changed after us; left in place (it passes calls through)");
    }
}

void Ask(void* state) noexcept
{
    // The walk-in question is asked of the seat's own coordinator about its mark (the free scooter of player 1's list
    // nearest the seat's member, UpdateTarget), or, without the mark's sites, of player 1's coordinator about its one
    // mark (the free one nearest player 1's leader). The question changes nothing and measures from whoever asks, so
    // every seat may ask; two seats answered yes for one scooter are kept apart at the boarding (AfterMemberTick: the
    // first boards, the second finds it held). research/RE-seats-step6-world-rides.md.
    void*     member = players::actor_fields::MoveStateCharacter(state);
    const int seat   = member != nullptr ? players::generation::SeatOfMember(member) : -1;
    SeatRide* record = RideOf(seat);
    if (!g_installed || record == nullptr || record->active || record->stopped || record->asked.valid ||
        !players::generation::OnGameThread())
    {
        return;
    }
    SeatRide& s = *record;
    // FieldSummonBody's one early-out before the key-4 question (in this scope it asks the seat's controller).
    std::uint8_t riding = 0;
    if (!platform::CallGuarded(g_fn.isRiding, riding) || riding != 0)
    {
        return;
    }
    RideAsk ask{};
    ask.type      = kScooterKey;
    ask.character = member;
    if (!WorldStick(state, member, ask.stick))
    {
        return;  // no move request: the game's question carries no stick and answers no
    }
    void* const        one         = Coordinator(players::generation::PlayerOneController());
    const std::int32_t scootersHere = ListCount(one);
    if (one == nullptr || scootersHere <= 0)
    {
        LogAsk(s, seat, nullptr, -1.0f, false);
        return;  // no parked scooter is registered on this map
    }
    void* coordinator = g_ownMark ? OwnCoordinator(s, seat, scootersHere) : one;
    void* target      = TargetOf(coordinator);
    if (target == nullptr)
    {
        LogAsk(s, seat, nullptr, -1.0f, false);
        return;  // no coordinator, or it marks no scooter (none free)
    }
    LogSettings(target);
    // The question's gates read the leader (PM+0x148) and the primary controller's rides: the seat's here.
    const scopes::leader_scope::LeaderScope leader(scopes::leader_scope::WeakOf(member));
    std::uint8_t                    yes = 0;
    const bool asked = leader.Held() && platform::CallGuarded(g_fn.shouldRide, yes, coordinator, &ask);
    const float distance = Between(member, target);
    LogAsk(s, seat, target, distance, asked && yes != 0);
    if (platform::Enabled(platform::Level::Trace))
    {
        platform::LogTrace(L"wheelie ride: asked {} (leader scope {}), stick {:.2f},{:.2f}, mark {:.0f} cm away", yes,
                           leader.Held() ? L"held" : L"REFUSED", ask.stick[0], ask.stick[1], distance);
    }
    if (!asked || yes == 0)
    {
        return;
    }
    s.asked = {true, member, target, scopes::leader_scope::WeakOf(target), ask.out};
}

void AfterMemberTick(void* controller, void* member) noexcept
{
    // The seat from its controller (the member's tick hands the seat's own).
    const int seat = players::generation::SeatOfController(controller);
    SeatRide* const record = RideOf(seat);
    if (record == nullptr || !record->asked.valid)
    {
        return;
    }
    SeatRide&            s    = *record;
    const wchar_t* const name = players::SeatName(seat);
    const Asked asked = s.asked;
    s.asked           = {};
    if (!g_installed || s.active || s.stopped || asked.member != member || !players::generation::OnGameThread())
    {
        return;
    }
    void* scooter = asked.scooter;
    void* one     = Coordinator(players::generation::PlayerOneController());
    const std::int32_t oneState = CoordState(one);
    if (!scopes::leader_scope::StillNames(asked.weak, scooter) || !Free(scooter) ||
        (ReadPointer(one, kCoordinatorBody) == scooter && oneState >= kStateBoarding))
    {
        platform::Log(L"wheelie ride: {} walked into {}, but it was taken or gone before the boarding", name,
                      players::NameOf(scooter));
        return;
    }
    // Another seat's ride on it (boarding, riding or getting off): the walk-in answered yes for both, the first boarded.
    for (int other = 1; other <= players::kSeatsBuilt; ++other)
    {
        if (other != seat && g_seats[other].active && g_seats[other].ride.vehicle == scooter)
        {
            platform::Log(L"wheelie ride: {} walked into {}, but {} is already on it; not boarded", name,
                          players::NameOf(scooter), players::SeatName(other));
            return;
        }
    }
    // A scooter a getoff left half-swapped (still on a player's controller) cannot be boarded again (seen live).
    const void* holder     = players::actor_fields::EffectiveController(scooter);
    const int   holderSeat = holder != nullptr ? players::generation::SeatOfController(holder) : -1;  // 0 player 1's
    if (holderSeat >= 0)
    {
        if (scooter != s.heldLogged)  // the walk-in asks every frame: once per scooter
        {
            s.heldLogged = scooter;
            platform::Log(L"wheelie ride: {} walked into {}, but it is still on {}'s controller (a getoff did not swap "
                          L"it back); not boarded",
                          name, players::NameOf(scooter), players::SeatName(holderSeat));
        }
        return;
    }
    // The boarding's possession swap runs the game's player-core test on her: as the seat's player, it must pass.
    if (!own_ride::BoardingGateOpen(seat, member, controller, L"wheelie ride"))
    {
        return;
    }
    // Inside the member tick's scopes (primary = the seat's controller, the member's leader answer) plus the party
    // leader: the game's ride start for the seat's controller, as FieldSummonBody's dispatch and TryBeginRide's
    // scripted branch do it.
    const scopes::leader_scope::LeaderScope leader(scopes::leader_scope::WeakOf(member));
    void* coordinator = nullptr;
    if (!leader.Held() || !scopes::primary_controller::Is(controller) ||
        !platform::CallGuarded(g_fn.getOrCreate, coordinator, controller, kScooterKey) ||
        VTableOf(coordinator) != g_coordVt)
    {
        s.stopped = true;
        platform::Log(L"wheelie ride STOPPED for this session: {}'s scooter coordinator could not be made (leader scope "
                      L"{}, primary {}PC{})",
                      name, leader.Held() ? L"held" : L"refused",
                      scopes::primary_controller::Is(controller) ? L"" : L"not ", seat + 1);
        return;
    }
    if (CoordState(coordinator) != kStateIdle || ReadPointer(coordinator, kCoordPending) != nullptr ||
        ReadField<std::uint8_t>(coordinator, kCoordDisabled) != 0)
    {
        platform::Log(L"wheelie ride: not boarded ({}'s coordinator is {} (state {}), pending {}, disabled {})", name,
                      StateName(CoordState(coordinator)), CoordState(coordinator),
                      ReadPointer(coordinator, kCoordPending) != nullptr ? L"yes" : L"no",
                      ReadField<std::uint8_t>(coordinator, kCoordDisabled));
        return;
    }
    BeginRequest request{};
    request.bytes[0] = kScooterKey;
    std::memcpy(request.bytes + kBeginName, &asked.name, sizeof(asked.name));
    request.bytes[kBeginFlagD] = 1;
    if (!platform::CallGuarded(g_fn.dispatch, &request) || ReadField<std::uint8_t>(coordinator, kCoordScripted) == 0)
    {
        s.stopped = true;
        platform::Log(L"wheelie ride STOPPED for this session: the game's ride dispatch {} on {}'s coordinator",
                      ReadField<std::uint8_t>(coordinator, kCoordScripted) == 0 ? L"latched nothing" : L"FAULTED", name);
        return;
    }
    // TryBeginRide's scripted branch with the seat's member as the rider: its own resolver names player 1's field leader.
    WriteField<std::uint8_t>(coordinator, kCoordScripted, 0);
    const bool instant  = ReadField<std::uint8_t>(coordinator, kCoordInstant) != 0;
    const bool applyNow = ReadField<std::uint8_t>(coordinator, kCoordApplyNow) != 0;
    const bool begun    = platform::CallGuarded(g_fn.begin, coordinator, scooter, member, instant, applyNow);
    // The boarding enter runs now, inside these scopes (the possession swap needs the leader answer).
    const bool flushed  = begun && platform::CallGuarded(g_fn.flush, static_cast<void*>(static_cast<std::byte*>(coordinator) +
                                                                                      kCoordStateMachine),
                                                         coordinator);
    s.ride             = {};
    s.ride.seat        = seat;
    s.ride.controller  = controller;
    s.ride.member      = member;
    s.ride.vehicle     = scooter;
    s.ride.coordinator = coordinator;
    s.ride.weak        = asked.weak;
    s.ride.state       = CoordState(coordinator);
    s.active           = true;
    s.summary.Restart(Clock::now());
    const bool owned   = begun && flushed && s.ride.state == kStateBoarding &&
                       ControllerHolds(scooter, controller) &&
                       ReadPointer(controller, kControllerActiveVehicle) == coordinator &&
                       VTableOf(players::actor_fields::MoveState(scooter)) == g_moveVt;
    s.ride.component = owned ? InputComponentOf(scooter) : nullptr;
    s.heldLogged     = nullptr;  // a later refusal (of any scooter) is logged again
    if (!owned)
    {
        LogLine(!(begun && flushed) ? L"the game's begin or flush FAULTED; ending it"
                                    : BySeat(seat, L"the boarding did not hand the scooter to player 2; ending it",
                                             L"the boarding did not hand the scooter to player 3; ending it"),
                s.ride);
        s.stopped = true;
        platform::Log(L"wheelie ride STOPPED for this session: {} does not board again", name);
        End(s, L"the boarding did not complete");
        return;
    }
    try
    {
        platform::Log(L"wheelie ride: {} boards {} with their own coordinator (the game's dispatch, begin and boarding; "
                      L"instant {}, input component {}): {}",
                      name, players::NameOf(scooter), instant, s.ride.component != nullptr ? L"found" : L"none", LeaseText(s.ride));
    }
    catch (...)
    {
    }
}

namespace {

/// The getoff calls, installed once from the game thread (the crossing ones need it; none of them runs meanwhile).
void InstallSites() noexcept
{
    g_sitesTried = true;
    int forced   = 0;
    for (std::size_t i = 0; i < kForcedSites.size(); ++i)
    {
        forced += g_forcedSites[i] != 0 &&
                          g_forcedCalls[i].InstallFromOwnThread(g_forcedSites[i], reinterpret_cast<std::uintptr_t>(g_fn.getOffPrimary),
                                                                reinterpret_cast<std::uintptr_t>(&ForcedGetOff))
                      ? 1
                      : 0;
    }
    const bool own = g_ownSite != 0 && g_ownCall.InstallFromOwnThread(g_ownSite, reinterpret_cast<std::uintptr_t>(g_fn.getOffPrimary),
                                                                      reinterpret_cast<std::uintptr_t>(&OwnGetOff));
    platform::Log(L"wheelie ride: the game's forced getoffs followed for players 2 and 3 at {} of {} callers; their getoff "
                  L"button {}",
                  forced, kForcedSites.size(),
                  own ? L"made instant when player 1 is more than 50 m away (the rental-return warp)"
                      : L"UNWATCHED (an animated getoff may meet the rental-return warp)");
    // The detach notify's field-leader test (in one block).
    const bool leader = g_detachLeaderSite != 0 &&
                        g_detachLeaderCall.InstallFromOwnThread(g_detachLeaderSite, reinterpret_cast<std::uintptr_t>(g_resolveLeader),
                                                                reinterpret_cast<std::uintptr_t>(&NotifyLeader));
    platform::Log(L"wheelie ride: player 2's or 3's animated getoff: the getoff motion's End DetachFromSBScooter notify runs in "
                  L"that player's scope, its field-leader test {}",
                  leader ? L"answers that player's member for her own notify"
                         : L"UNCHANGED (player 2's or 3's animated getoff will not swap back: the mod's fallback gives her back)");
    // The seat's own mark: the target search and the marker's resolve, both or neither (a target without its marker
    // would board a scooter no icon shows; a marker computed for the seat on player 1's mark would point at player 1's
    // scooter).
    const bool search = g_fn.updateTarget != nullptr && g_updateTargetSite != 0 && g_markerLeaderSite != 0 &&
                        g_updateTargetCall.InstallFromOwnThread(g_updateTargetSite, reinterpret_cast<std::uintptr_t>(g_fn.updateTarget),
                                                                reinterpret_cast<std::uintptr_t>(&UpdateTarget));
    g_ownMark = search && g_markerLeaderCall.InstallFromOwnThread(g_markerLeaderSite, reinterpret_cast<std::uintptr_t>(g_resolveLeader),
                                                                  reinterpret_cast<std::uintptr_t>(&MarkerLeader));
    if (search && !g_ownMark && !g_updateTargetCall.Restore())
    {
        platform::Log(L"wheelie ride: the target search's call could not be put back; it passes every call through");
    }
    platform::Log(L"wheelie ride: player 2's or 3's own mark {}",
                  g_ownMark ? L"placed: their own coordinator marks the free scooter nearest their member, shows the icon "
                              L"in their view, and answers their walk-in"
                            : L"NOT placed (a site differs): they walk into player 1's marked scooter only");
    // The seat's own distance counter: the game's request made for the seat's coordinator, and its tick's close (its
    // list is always empty) held off for a seat's.
    g_ownTop = g_ownMark && g_fn.requestTopUI != nullptr && g_fn.closeTopUI != nullptr && g_topCloseSite != 0 &&
               g_topCloseCall.InstallFromOwnThread(g_topCloseSite, reinterpret_cast<std::uintptr_t>(g_fn.closeTopUI),
                                                   reinterpret_cast<std::uintptr_t>(&TickCloseTopUI));
    platform::Log(L"wheelie ride: player 2's or 3's own distance counter {}",
                  g_ownTop ? L"kept: asked of the game for their coordinator, shown by their own riding tick in their view"
                           : L"NOT kept (a check failed, or no own mark): the counter stays player 1's");
}

void LogSummary(SeatRide& s) noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;  // a periodic status line: Debug
    }
    const auto now = Clock::now();
    if (!s.summary.Due(now))
    {
        return;
    }
    try
    {
        platform::LogDebug(L"wheelie ride: driving, {} ticks ({} refused); {}", s.ride.ticks, s.ride.refused, LeaseText(s.ride));
    }
    catch (...)
    {
    }
}

}  // namespace

bool Tick(void* controller, void* member, const input::move_state::EngineTicks& engine) noexcept
{
    if (!g_installed || !players::generation::OnGameThread())
    {
        return false;
    }
    if (!g_sitesTried)
    {
        InstallSites();
    }
    // The seat from its controller (the member's tick hands the seat's own).
    const int seat = players::generation::SeatOfController(controller);
    SeatRide* const record = RideOf(seat);
    if (record == nullptr)
    {
        return false;
    }
    if (!record->active)
    {
        KeepOwnCoordinator(*record, seat);
        return false;
    }
    SeatRide&            s    = *record;
    const wchar_t* const name = players::SeatName(seat);
    Ride&                ride = s.ride;
    if (ride.member != member || ride.controller != controller)
    {
        End(s, BySeat(seat, L"player 2's member changed", L"player 3's member changed"));
        return false;
    }
    if (!scopes::leader_scope::StillNames(ride.weak, ride.vehicle))
    {
        End(s, L"the scooter is gone");
        return false;
    }
    if (s.stopped)
    {
        End(s, L"the getoff's swap back faulted");  // the coordinator to idle, then the game's possession call for the member
        return false;
    }
    const std::int32_t state = CoordState(ride.coordinator);
    if (state != ride.state)
    {
        const std::int32_t was = ride.state;
        ride.state             = state;
        const std::wstring prefix = std::format(L"{}'s coordinator {} -> {}", name, StateName(was), StateName(state));
        LogLine(prefix.c_str(), ride);
        switch (ride_rules::CoordChangeOf(was, state, kStateIdle))
        {
            case ride_rules::CoordChange::Finished:
            {
                // The getoff's exit clears the primary controller's active coordinator: the seat's inside the seat's
                // scope; player 1's if the coordinator was ticked without it (a refused input pass).
                void* const one = players::generation::PlayerOneController();
                if (CoordState(Coordinator(one)) == kStateRiding && ReadPointer(one, kControllerActiveVehicle) == nullptr)
                {
                    platform::Log(L"WARNING wheelie ride: player 1's active ride was cleared as {}'s coordinator got off: "
                                  L"its exit ran outside {}'s scope",
                                  name, name);
                }
                Finish(s, L"the game's getoff finished");
                return false;
            }
            case ride_rules::CoordChange::Gone:
                s.stopped = true;
                Finish(s, BySeat(seat, L"player 2's coordinator is gone; rides stopped for this session",
                                 L"player 3's coordinator is gone; rides stopped for this session"));
                return false;
            case ride_rules::CoordChange::None: break;
        }
    }
    if (state == kStateRiding && facts::InBattle())
    {
        const bool ok = GetOff(ride, true);
        platform::Log(L"wheelie ride: a battle started while {} rode: the game's instant getoff in {}'s scope {}", name,
                      name, ok ? L"ran" : L"COULD NOT RUN");
        return !ControllerHolds(member, controller);
    }
    // The field ticker runs the leader's ride body while it is the primary controller's active ride: the seat's
    // scooter is ticked the same way, on the frames the ticker runs, while the seat's controller holds it.
    void*      state16 = players::actor_fields::MoveState(ride.vehicle);
    const bool holds   = ReadPointer(controller, kControllerActiveVehicle) == ride.coordinator &&
                       ReadPointer(ride.coordinator, kCoordinatorBody) == ride.vehicle &&
                       ControllerHolds(ride.vehicle, controller) && VTableOf(state16) == g_moveVt;
    if (!holds)
    {
        return !ControllerHolds(member, controller);  // the member's own tick once she is the seat's again
    }
    switch (own_ride::TickVehicle(ride, state16, engine, 0))
    {
        case own_ride::VehicleTick::Waiting: return true;  // the field ticker is idle: the seat's scooter waits too
        case own_ride::VehicleTick::Faulted:
            s.stopped = true;
            platform::Log(L"wheelie ride STOPPED for this session: the engine raised an exception inside the scooter's "
                          L"tick");
            End(s, L"the engine raised an exception inside the scooter's tick");
            return false;
        case own_ride::VehicleTick::Ticked:
        case own_ride::VehicleTick::Refused: break;
    }
    LogSummary(s);
    return true;
}

bool Riding(int seat) noexcept
{
    const SeatRide* s = RideOf(seat);
    return s != nullptr && s->active;
}

const void* DrivenMoveState(int seat) noexcept
{
    const SeatRide* s = RideOf(seat);
    if (s == nullptr || !s->active || s->ride.vehicle == nullptr ||
        !scopes::leader_scope::StillNames(s->ride.weak, s->ride.vehicle))
    {
        return nullptr;
    }
    const void* state = players::actor_fields::MoveState(s->ride.vehicle);
    return VTableOf(state) == g_moveVt && players::actor_fields::MoveStateCharacter(state) == s->ride.vehicle ? state : nullptr;
}

void Release(int seat, const wchar_t* why) noexcept
{
    SeatRide* s = RideOf(seat);
    if (s == nullptr)
    {
        return;
    }
    s->asked = {};
    if (s->active && players::generation::OnGameThread())
    {
        End(*s, why);
    }
}

void CloseCounter(int seat) noexcept
{
    SeatRide* s = RideOf(seat);
    if (s != nullptr)
    {
        CloseTop(*s, seat);
    }
}

void* Forget(int seat) noexcept
{
    SeatRide* record = RideOf(seat);
    if (record == nullptr)
    {
        return nullptr;
    }
    SeatRide& s       = *record;
    s.asked           = {};
    void*     scooter = s.active ? s.ride.vehicle : nullptr;
    if (scooter != nullptr)
    {
        platform::Log(L"wheelie ride: the member was destroyed while {} rode; the ride is forgotten",
                      players::SeatName(seat));
        s.stopped = true;  // the seat's coordinator is left as the game left it: no ride is begun again this session
    }
    s.active        = false;
    s.ride          = {};
    s.markFaulted   = false;  // a new map, a new coordinator: its first write fault is logged again
    s.markLogged    = false;
    s.listCapLogged = false;
    CloseTop(s, seat);
    return scooter;
}

namespace {

/// Each player's coordinator's widget by `of` (player 1's at [0], each active seat's own at [seat]); empty when the
/// seats have no coordinators of their own.
PlayerWidgets CoordinatorWidgets(void* (*of)(const void* coordinator)) noexcept
{
    PlayerWidgets widgets;
    if (!g_installed || !g_ownMark || !players::generation::OnGameThread())
    {
        return widgets;
    }
    widgets.widget[0] = of(Coordinator(players::generation::PlayerOneController()));
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (players::generation::Active(seat))
        {
            widgets.widget[seat] = of(Coordinator(players::generation::ControllerOf(seat)));
        }
    }
    return widgets;
}

}  // namespace

MarkerWidgets MarkerWidgetsNow() noexcept
{
    return CoordinatorWidgets(&MarkerWidgetOf);
}

TopWidgets TopWidgetsNow() noexcept
{
    return g_ownTop ? CoordinatorWidgets(&TopWidgetOf) : TopWidgets{};
}

int MarkerOwner(const MarkerWidgets& icons, const void* widget) noexcept
{
    for (int player = 0; widget != nullptr && player < players::kMaxPlayers; ++player)
    {
        if (icons.widget[player] == widget)
        {
            return player;
        }
    }
    return -1;
}

}  // namespace lss::rides::wheelie_ride
