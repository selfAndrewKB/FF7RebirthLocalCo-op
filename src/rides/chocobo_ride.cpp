#include "rides/chocobo_ride.h"

#include "facts/cameras.h"
#include "facts/controller.h"
#include "facts/engine.h"
#include "facts/in_battle.h"
#include "facts/move_state.h"
#include "facts/party.h"
#include "facts/vehicles.h"
#include "facts/world.h"
#include "players/actor_fields.h"
#include "players/describe.h"
#include "players/game_globals.h"
#include "rides/ride_rules.h"
#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>

#include "input/member_movement.h"
#include "platform/build_value.h"
#include "platform/every.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "players/unreal_access.h"
#include "rides/chocobo_call.h"
#include "rides/own_ride.h"
#include "scopes/leader_scope.h"
#include "session/party_member.h"
#include "world/charapop_facts.h"
#include "world/party_warp.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

namespace lss::rides::chocobo_ride {

namespace {

using RC::Unreal::UObject;
using Clock = std::chrono::steady_clock;

// The game's functions (research/RE-p2-chocobo-ride.md; entry bytes read in IDA).
using facts::kGetOrCreateVehicleCoordinator;
using GetCoordinatorFn = void* (*)(void* controller, std::uint8_t type);

constexpr std::uint8_t kBeginRideBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48,
                                            0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x41, 0x56, 0xB8, 0x30, 0x00};
constexpr platform::FunctionFact kBeginRide{L"ChocoboCoordinator_BeginRide_SetActive", 0x143B18214, kBeginRideBytes};
using BeginRideFn = void (*)(void* coordinator, void* chocobo, void* rider, bool instant, bool applyNow);

constexpr std::uint8_t kEndRideBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48,
                                          0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x41, 0x56, 0xB8, 0x90, 0x00};
constexpr platform::FunctionFact kEndRide{L"ChocoboCoordinator_EndRide_ClearActive", 0x143B17AEC, kEndRideBytes};
struct CharacterArray  // TArray<AEndCharacter*>, which EndRide fills with the riders it let go of
{
    void**       data = nullptr;
    std::int32_t num  = 0;
    std::int32_t max  = 0;
};
// The game's 16-byte get-off parameters EndRide reads (byte 1 to coord+0x145, passed to the rider's AI ride-end; byte 2
// to coord+0x146). The game's forced end passes its own, all zero unless its cutscene clip is flagged
// (RE-p2-chocobo-ride-end-fault.md). A null pointer here faults four instructions in (an end fault seen in play).
struct alignas(8) GetOffParams
{
    std::uint8_t bytes[16] = {};
};
using EndRideFn = void (*)(void* coordinator, const GetOffParams* params, CharacterArray* out);

using facts::kSwitchAiControlAsync;
using SwitchAiFn = void (*)(void* character, std::uint8_t toPlayer, void* controller);

using facts::kChocoboForCharacter;
using ChocoboForFn = void* (*)(const void* manager, const void* character);
using GlobalTestFn = std::uint8_t (*)();

// Objects and fields (this build).
using facts::kRootChocoboRideManager;
using facts::kRideManagerGetOff;
using facts::kRideManagerEntries;
using facts::kRideManagerEntrySize;
using facts::kRideManagerPlayerTypes;
constexpr std::uint8_t   kEntryLoaded       = 3;       // the member entry's state byte with her chocobo loaded, not yet placed
constexpr std::uint8_t   kEntryRiding       = 4;       // the member entry's state byte while she rides
constexpr std::uint8_t   kChocoboType       = 1;       // the vehicle type of a chocobo coordinator
using facts::kControllerActiveVehicle;
constexpr std::uint32_t  kCoordState        = 0x58;    // the coordinator's state machine: current row -> u32 id
constexpr std::uint32_t  kCoordPending      = 0x60;    // ... pending row (null when none)
constexpr std::uint32_t  kCoordIdle         = 0;
constexpr std::uint32_t  kCoordRiding       = 2;
using facts::kChocoboRidden;
using facts::kChocoboRideObject;
using facts::kSceneComponentAttachParent;
constexpr std::uint32_t  kRideSubjectField  = 0x2CC8;  // PCM, weak
using facts::kChocoboCoordinatorVTable;
using facts::kChocoboMoveStateVTable;
using facts::kBattlePlayerComponentVTable;
using facts::kChocoboCameraModuleVTable;
constexpr std::uintptr_t kVehicleKeyName    = 0x14901C618;  // FName "Vehicle": the getoff button
// The chocobo move state's row request bytes (handle +0x48): the summon rows, then Dash, Dismount, Jet, Scent, Scour
// (research/RE-p2-chocobo-rows.md); 1 = lit, 2 = dim.
constexpr std::uint32_t kRowRequests[] = {0xE8, 0x138, 0x188, 0x248, 0x298, 0x2E8, 0x338, 0x388};

constexpr auto          kSummaryEvery   = std::chrono::seconds(10);
constexpr auto          kGetOffWait     = std::chrono::seconds(4);  // the game's getoff animation, at most
constexpr std::uint32_t kEndRetryTicks  = 60;  // ticks to wait for the coordinator to reach riding before giving up

struct Functions
{
    GetCoordinatorFn getCoordinator = nullptr;
    BeginRideFn      beginRide      = nullptr;
    EndRideFn        endRide        = nullptr;
    SwitchAiFn       switchAi       = nullptr;
    ChocoboForFn     chocoboFor     = nullptr;
};

Functions      g_fn;
std::uintptr_t g_gameRoot = 0, g_coordVt = 0, g_moveVt = 0, g_componentVt = 0, g_moduleVt = 0;
const std::uint64_t* g_vehicleKey = nullptr;
bool           g_installed = false;

enum class Phase
{
    Idle,
    Riding,      // the seat's coordinator rides; the mod ticks the chocobo
    GettingOff,  // the game dismounted her; waiting for her getoff animation to detach her
    Ending,      // ending, waiting for the seat's coordinator to reach riding (the game's end needs it)
};

// Game thread. The lease (the seat's controller, member, the chocobo as `vehicle`, the seat's own chocobo
// coordinator) is rides/own_ride's; the rest is this ride's.
struct Ride : own_ride::Lease
{
    std::uint32_t rows        = 0;        // the row request values last logged
    // Its slot 1 (the actor tick's handles and anim sync) in the seat's scope: counts, and the game's ridden-vehicle
    // test outside/inside the scope as last sampled and as last logged (-1 not yet).
    std::uint32_t handles = 0, handlesRefused = 0;
    std::int32_t  riddenOutside = -1, riddenInside = -1, loggedOutside = -2, loggedInside = -2;
    bool          handlesFaulted = false;  // the engine raised an exception in it: the ride ends on the next tick
    Clock::time_point getOffAt{};
    std::uint32_t endTries    = 0;
    bool          rowsCleared = false;
    const wchar_t* endWhy     = nullptr;  // Ending: why
};

/// One seat's ride and what it remembers between rides. Game thread.
struct SeatRide
{
    Phase           phase    = Phase::Idle;
    Ride            ride{};
    const void*     heldBack = nullptr;  // a chocobo not to board again until its ridden flag clears
    bool            stopped  = false;    // a ride failed: the seat's player stays a passenger this session
    platform::Every summary{kSummaryEvery};
    // The chocobo the game seats the member on, held off its follow AI's warp (null: none), and its weak pointer; the
    // one the hold has not taken on yet (logged once, tried again each tick).
    const void*     warpHeld     = nullptr;
    std::uint64_t   warpHeldWeak = 0;
    const void*     warpTried    = nullptr;
};
SeatRide       g_seats[players::kMaxPlayers];  // by seat (players/seats.h); index 0, player 1's, unused
CharacterArray g_released{};  // reused for every EndRide, every seat's in turn: the game keeps one allocation in it

/// The seat's ride record, or null for a number without one.
SeatRide* RideOf(int seat) noexcept
{
    return players::SeatIndex(seat) ? &g_seats[seat] : nullptr;
}

using players::BySeat;  // a text kept by pointer (an end's reason, a log prefix) naming the seat's player

bool Attached(void* member) noexcept
{
    return ReadPointer(players::unreal::RootOf(member), kSceneComponentAttachParent) != nullptr;
}

const std::byte* RideManager() noexcept
{
    const void* root = ReadPointer(reinterpret_cast<const void*>(g_gameRoot), 0);
    return static_cast<const std::byte*>(ReadPointer(root, kRootChocoboRideManager));
}

/// The member's entry state byte in the ride manager, or -1.
int EntryState(const std::byte* manager, const void* member) noexcept
{
    const auto type = session::party_member::PlayerTypeOf(member);
    if (manager == nullptr || !type || *type >= kRideManagerPlayerTypes)
    {
        return -1;
    }
    return ReadField<std::uint8_t>(manager + kRideManagerEntries + kRideManagerEntrySize * *type, 0);
}

/// A chocobo coordinator's state id, or -1.
std::int32_t CoordState(const void* coordinator) noexcept
{
    return own_ride::CoordState(coordinator, g_coordVt, kCoordState);
}

/// The game's EndRide precondition (ChocoboCoordinator_IsRidingState2_NoPending).
bool CoordRiding(const void* coordinator) noexcept
{
    return CoordState(coordinator) == static_cast<std::int32_t>(kCoordRiding) && ReadPointer(coordinator, kCoordPending) == nullptr;
}

/// The seat's camera's slot-15 module is a chocobo camera module (the ride framing).
bool RideCameraOn(const void* controller) noexcept
{
    const void* module = own_ride::CameraModule15(controller);
    return module != nullptr && VTableOf(module) == g_moduleVt;
}

/// The possession and ride fields, for the log.
std::wstring LeaseText(const Ride& ride)
{
    const void* pcm = players::actor_fields::CameraManager(ride.controller);
    return std::format(L"{} (state {}), leader {}; {}'s camera ride subject {}, ride framing {}, view target {}",
                       own_ride::PossessionText(ride, L"chocobo"), CoordState(ride.coordinator),
                       players::actor_fields::LeaderCharacter(ride.controller) == ride.member ? L"the member" : L"other",
                       players::SeatName(ride.seat),
                       scopes::leader_scope::ObjectAt(static_cast<const std::byte*>(pcm) + kRideSubjectField) == ride.vehicle
                           ? L"the chocobo"
                           : L"not the chocobo",
                       RideCameraOn(ride.controller) ? L"on" : L"off",
                       players::actor_fields::ViewTarget(pcm) == ride.member    ? L"the member"
                       : players::actor_fields::ViewTarget(pcm) == ride.vehicle ? L"the chocobo"
                                                                       : L"the camera actor or other");
}

void LogLine(const wchar_t* prefix, const Ride& ride) noexcept
{
    try
    {
        platform::Log(L"chocobo ride: {}: {}", prefix, LeaseText(ride));
    }
    catch (...)
    {
    }
}

/// The chocobo's input component (rides/own_ride.h), checked by vtable.
const void* InputComponentOf(const void* chocobo) noexcept
{
    const void* found = own_ride::InputComponentOf(chocobo);
    return VTableOf(found) == g_componentVt ? found : nullptr;
}

struct BeginCall
{
    void* controller  = nullptr;
    void* chocobo     = nullptr;
    void* member      = nullptr;
    void* coordinator = nullptr;
    bool  called      = false;
    bool  ok          = false;
};

/// Inside the primary scope (the seat's): the game's coordinator for the seat's controller, then the leader's boarding
/// for her (instant, applied now: the boarding enter runs inside this call, so inside the scope).
void RunBegin(void* context)
{
    auto* call = static_cast<BeginCall*>(context);
    if (!platform::CallGuarded(g_fn.getCoordinator, call->coordinator, call->controller, kChocoboType) ||
        call->coordinator == nullptr || CoordState(call->coordinator) != static_cast<std::int32_t>(kCoordIdle))
    {
        return;
    }
    call->called = true;
    call->ok     = platform::CallGuarded(g_fn.beginRide, call->coordinator, call->chocobo, call->member, true, true);
}

struct EndCall
{
    void*           coordinator = nullptr;
    bool            ok          = false;
    platform::Fault fault;  // when not ok
};

void RunEnd(void* context)
{
    auto* call      = static_cast<EndCall*>(context);
    g_released.num  = 0;  // the game adds the riders it let go of; its allocation is kept for the next ride
    const GetOffParams params{};  // the plain get-off: no cutscene flags
    call->ok        = platform::CallGuarded(g_fn.endRide, call->coordinator, &params, &g_released);
    if (!call->ok)
    {
        call->fault = platform::LastFault();
    }
}

/// The ride is over for the mod: the seat's controller must hold the member again. If the game's end did not give
/// her back (or could not run), the game's possession call; if even that fails, stopped for the session.
void Finish(SeatRide& s, const Ride& ride, const wchar_t* why, const wchar_t* how) noexcept
{
    s.phase    = Phase::Idle;
    s.heldBack = ride.vehicle;
    const bool held = own_ride::GiveBack(ride, g_fn.switchAi, L"chocobo ride", L"the end");
    try
    {
        platform::Log(L"chocobo ride: {} is off their chocobo ({}; {}; {} ticks driven, {} refused): {}",
                      players::SeatName(ride.seat), why, how, ride.ticks, ride.refused, LeaseText(ride));
    }
    catch (...)
    {
    }
    if (!held)
    {
        s.stopped = true;
        own_ride::LogNotHeld(ride.seat, L"chocobo ride");
    }
}

/// Ends the seat's ride with the game's own end (the possession back, the ride object, the camera module and
/// ride subject, the active coordinator cleared). `force`: now, whatever the coordinator's state (a give-back, a
/// leave); otherwise a coordinator not riding yet is retried on the next ticks.
void End(SeatRide& s, const wchar_t* why, bool force) noexcept
{
    if (s.phase == Phase::Idle)
    {
        return;
    }
    Ride& ride = s.ride;
    if (void* state = players::actor_fields::MoveState(ride.vehicle);
        !ride.rowsCleared && scopes::leader_scope::StillNames(ride.weak, ride.vehicle) && VTableOf(state) == g_moveVt)
    {
        input::member_movement::ClearRideRows(state);  // the seat's rows go with the ride
        ride.rowsCleared = true;
    }
    if (!scopes::leader_scope::StillNames(ride.weak, ride.vehicle))
    {
        // The game's end reads the coordinator's chocobo: not with a destroyed one. The seat's coordinator stays as the
        // game left it, so no ride is begun again this session.
        s.stopped = true;
        Finish(s, ride, why, L"the chocobo is gone, the game's end skipped; rides stopped for this session");
        return;
    }
    if (!CoordRiding(ride.coordinator))
    {
        if (ride_rules::EndRetryAllowed(force, ride.endTries, kEndRetryTicks))
        {
            s.phase     = Phase::Ending;  // boarding's completion reaches riding on the seat's next input tick
            ride.endWhy = why;
            return;
        }
        // The game's end cannot run: the seat's coordinator stays half-boarded (its menu locks on, the chocobo without
        // its AI). Reached only by a forced end (a give-back, a leave, a map load) within the tick boarding takes, or
        // a coordinator that never reaches riding.
        s.stopped = true;
        platform::Log(L"WARNING chocobo ride: the game's end of {}'s coordinator could not run (state {}); its "
                      L"boarding is left as it was",
                      players::SeatName(ride.seat), CoordState(ride.coordinator));
        Finish(s, ride, why,
               BySeat(ride.seat,
                      L"player 2's coordinator was not riding, the game's end skipped; rides stopped for this session",
                      L"player 3's coordinator was not riding, the game's end skipped; rides stopped for this session"));
        return;
    }
    EndCall call{ride.coordinator};
    const bool scoped = input::member_movement::InPrimaryScope(ride.seat, ride.member, &RunEnd, &call);
    if (scoped && !call.ok)
    {
        // Where the game's end stopped: the steps after the fault did not run (EndRide's order: rider detached, swap
        // back, ride object and camera subject cleared, active coordinator cleared, state 0 requested).
        platform::Log(L"WARNING chocobo ride: the game's end of {}'s coordinator raised {}; after it: coordinator "
                      L"state {}, {}'s active coordinator {}, the chocobo's controller {}PC{}, the member's {}PC{}",
                      players::SeatName(ride.seat), platform::DescribeFault(call.fault).text,
                      CoordState(ride.coordinator), players::SeatName(ride.seat),
                      ReadPointer(ride.controller, kControllerActiveVehicle) == ride.coordinator ? L"still it"
                                                                                                  : L"cleared",
                      players::actor_fields::Controller(ride.vehicle) == ride.controller ? L"" : L"not ",
                      ride.seat + 1,
                      players::actor_fields::Controller(ride.member) == ride.controller ? L"" : L"not ",
                      ride.seat + 1);
    }
    const Ride ended  = ride;
    Finish(s, ended, why,
           !scoped   ? L"the game's end not run (no scope)"
           : call.ok ? (ReadPointer(ended.controller, kControllerActiveVehicle) == nullptr ? L"the game's end ran"
                                                                                : L"the game's end ran, the coordinator is still active")
                     : L"the game's end FAULTED");
}

/// Metres from `actor` to player 1's pawn (their ride body while they ride), or -1.
float MetresFromPlayerOne(const void* actor) noexcept
{
    float at[3]{}, oneAt[3]{};
    const void* oneBody = players::actor_fields::Pawn(players::generation::PlayerOneController());
    return players::unreal::ActorLocation(actor, at) && players::unreal::ActorLocation(oneBody, oneAt)
               ? world::charapop::Distance(oneAt, at) / 100.0f
               : -1.0f;
}

/// Lifts the no-teleport hold from the chocobo it was put on (when it still exists): the chocobo's follow AI warps as
/// the game's again.
void LiftWarpHold(SeatRide& s, int seat, const wchar_t* why) noexcept
{
    if (s.warpHeld == nullptr)
    {
        return;
    }
    if (scopes::leader_scope::StillNames(s.warpHeldWeak, s.warpHeld))
    {
        const auto held = world::party_warp::HoldOffWarp(const_cast<void*>(s.warpHeld),
                                                         world::party_warp::Subject::Chocobo, false);
        platform::Log(L"chocobo ride: {}'s chocobo no longer held off its follow warp ({}): {}, {:.0f} m from player 1's "
                      L"ride body",
                      players::SeatName(seat), why,
                      held == false ? L"lifted" : held == true ? L"could NOT be lifted" : L"not in FOLLOW (nothing to lift)",
                      MetresFromPlayerOne(s.warpHeld));
    }
    s.warpHeld     = nullptr;
    s.warpHeldWeak = 0;
}

/// The chocobo the ride manager holds for `member` (her entry's), or null.
void* EntryChocobo(const std::byte* manager, const void* member) noexcept
{
    void* chocobo = nullptr;
    return manager != nullptr && platform::CallGuarded(g_fn.chocoboFor, chocobo, static_cast<const void*>(manager), member)
               ? chocobo
               : nullptr;
}

/// While the game seats the seat's member on her own chocobo (its entry loaded or riding) and that chocobo's own AI
/// drives it, the game's no-teleport hold keeps its follow warp from taking it to player 1 during her mount animation,
/// and her with it when the mount attaches her (research/RE-chocobo-summon-gather.md). Kept for a passenger's whole
/// ride, and set again on a chocobo the mod gave back to its AI while the entry still rides (players 2 and 3 are never
/// warped by the game's AI); lifted before the boarding and when the game lets the chocobo go. A held chocobo whose hold
/// reads clear again (its follow state rebuilt) is held again.
void HoldOffFollowWarp(SeatRide& s, int seat, void* controller, void* member) noexcept
{
    if (!world::party_warp::CanHoldOffWarp())
    {
        return;
    }
    const std::byte* manager = RideManager();
    const int        entry   = EntryState(manager, member);
    void* chocobo = entry == kEntryLoaded || entry == kEntryRiding ? EntryChocobo(manager, member) : nullptr;
    const bool same = s.warpHeld != nullptr && chocobo == s.warpHeld &&
                      scopes::leader_scope::StillNames(s.warpHeldWeak, chocobo);
    if (s.warpHeld != nullptr && !same)
    {
        LiftWarpHold(s, seat, L"the game let it go");  // a chocobo gone since is only forgotten (the weak check)
    }
    if (chocobo == nullptr || players::actor_fields::Controller(chocobo) == controller ||
        (same && world::party_warp::WarpHeldOff(chocobo, world::party_warp::Subject::Chocobo) != false))
    {
        return;
    }
    const auto held = world::party_warp::HoldOffWarp(chocobo, world::party_warp::Subject::Chocobo, true);
    if (held == true)
    {
        s.warpTried = nullptr;
        if (same)
        {
            platform::LogDebug(L"chocobo ride: {}'s chocobo's no-teleport hold read clear again (its follow state "
                               L"rebuilt?); set again",
                               players::SeatName(seat));
            return;
        }
        s.warpHeld     = chocobo;
        s.warpHeldWeak = scopes::leader_scope::WeakOf(chocobo);
        platform::Log(L"chocobo ride: {}'s chocobo held off its follow warp (the game's no-teleport hold, as its lifts "
                      L"set it; entry state {}), {:.0f} m from player 1's ride body",
                      players::SeatName(seat), entry, MetresFromPlayerOne(chocobo));
        return;
    }
    if (s.warpTried != chocobo)
    {
        // The setter acts on its controller's current AI state, the read-back on its retained AI's brain state: both
        // named here, should the hold never read set.
        s.warpTried = chocobo;
        try
        {
            platform::LogDebug(L"chocobo ride: {}'s chocobo not held off its follow warp yet (entry state {}, read back "
                               L"{}; its controller {}, its {}); tried again each tick",
                               players::SeatName(seat), entry, !held ? L"no follow state" : L"clear",
                               players::actor_fields::Controller(chocobo),
                               world::party_warp::AiText(chocobo, world::party_warp::Subject::Chocobo));
        }
        catch (...)
        {
        }
    }
}

/// The game has seated the seat's member on her own chocobo (her chocobo appears by her and she is seated where
/// she stands; taken at once, before the chocobo's follow AI can pull it toward player 1). Null when not.
void* SeatedChocobo(SeatRide& s, void* controller, void* member) noexcept
{
    if (players::actor_fields::Controller(member) != controller || players::actor_fields::LeaderCharacter(controller) != member)
    {
        return nullptr;
    }
    const std::byte* manager = RideManager();
    if (manager == nullptr || ReadField<std::uint8_t>(manager, kRideManagerGetOff) != 0 ||
        EntryState(manager, member) != kEntryRiding)
    {
        return nullptr;
    }
    void* const chocobo = EntryChocobo(manager, member);
    if (chocobo == nullptr)
    {
        return nullptr;
    }
    switch (ride_rules::SeatedVerdict(ReadField<std::uint8_t>(chocobo, kChocoboRidden) != 0, s.heldBack == chocobo,
                                      Attached(member), facts::InBattle()))
    {
        case ride_rules::Seated::Unridden:
            if (s.heldBack == chocobo)
            {
                s.heldBack = nullptr;  // that ride is over: the next mount may be boarded
            }
            return nullptr;
        case ride_rules::Seated::HeldBack:
        case ride_rules::Seated::Detached:
        case ride_rules::Seated::Battle: return nullptr;
        case ride_rules::Seated::Seated: break;
    }
    return chocobo;
}

void Begin(SeatRide& s, int seat, void* controller, void* member, void* chocobo) noexcept
{
    // The boarding's swap runs the game's player-core test on her: as the seat's player, it must pass.
    if (!own_ride::BoardingGateOpen(seat, member, controller, L"chocobo ride"))
    {
        s.heldBack = chocobo;  // the seat's player stays a passenger for this ride
        return;
    }
    // While its AI still holds the chocobo (the setter needs it): the mod's own strand warp of the ridden chocobo, and its
    // AI after the getoff, work as the game's.
    if (s.warpHeld == chocobo)
    {
        LiftWarpHold(s, seat, L"before the boarding");
    }
    const void* oldObject = ReadPointer(chocobo, kChocoboRideObject);
    BeginCall   call{controller, chocobo, member};
    input::member_movement::InPrimaryScope(seat, member, &RunBegin, &call);
    s.ride = {};
    s.ride.seat        = seat;
    s.ride.controller  = controller;
    s.ride.member      = member;
    s.ride.vehicle     = chocobo;
    s.ride.coordinator = call.coordinator;
    s.ride.weak        = scopes::leader_scope::WeakOf(chocobo);
    if (!call.called)
    {
        s.heldBack = chocobo;
        s.stopped  = true;
        platform::Log(L"chocobo ride STOPPED for this session: {}'s coordinator {} (state {}); {} stays a passenger",
                      players::SeatName(seat), call.coordinator == nullptr ? L"could not be made" : L"is not idle",
                      CoordState(call.coordinator), players::SeatName(seat));
        return;
    }
    s.phase          = Phase::Riding;
    void* state      = players::actor_fields::MoveState(chocobo);
    const bool owned = call.ok && players::actor_fields::EffectiveController(chocobo) == controller &&
                       players::actor_fields::Pawn(controller) == chocobo &&
                       ReadPointer(controller, kControllerActiveVehicle) == call.coordinator &&
                       VTableOf(state) == g_moveVt;
    s.ride.component = owned ? InputComponentOf(chocobo) : nullptr;
    if (!owned || s.ride.component == nullptr)
    {
        LogLine(!call.ok ? L"the boarding FAULTED; ending it"
                         : BySeat(seat, L"the boarding did not hand the chocobo to player 2; ending it",
                                  L"the boarding did not hand the chocobo to player 3; ending it"),
                s.ride);
        s.stopped = true;  // no new ride; this one ends with the game's end once the coordinator rides (Ending)
        platform::Log(L"chocobo ride STOPPED for this session: {} stays a passenger", players::SeatName(seat));
        End(s, L"the boarding did not complete", false);
        return;
    }
    s.summary.Restart(Clock::now());
    try
    {
        platform::Log(L"chocobo ride: {} boards their chocobo with their own coordinator (the game's boarding, instant; "
                      L"ride object {}): {}",
                      players::SeatName(seat), ReadPointer(chocobo, kChocoboRideObject) != oldObject ? L"replaced" : L"kept",
                      LeaseText(s.ride));
    }
    catch (...)
    {
    }
}

/// Why the ride ends now, or null. `getOff`: the game dismounted her (the ridden flag cleared): wait for the getoff.
const wchar_t* EndReason(const Ride& ride, bool& getOff) noexcept
{
    ride_rules::EndReads reads;
    reads.gone = !scopes::leader_scope::StillNames(ride.weak, ride.vehicle);
    if (!reads.gone)  // the rest is read only while the chocobo is still there
    {
        reads.ridden      = ReadField<std::uint8_t>(ride.vehicle, kChocoboRidden) != 0;
        reads.inBattle    = facts::InBattle();
        reads.entryRiding = EntryState(RideManager(), ride.member) == kEntryRiding;
        reads.attached    = Attached(ride.member);
        reads.holds       = own_ride::ControllerHolds(ride.vehicle, ride.controller);
    }
    const auto reason = ride_rules::EndReasonOf(reads);
    getOff            = reason == ride_rules::EndReason::Dismounted;
    return ride_rules::Describe(reason, ride.seat);
}

void LogRows(Ride& ride, void* state) noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        ride.rows = ~0u;  // each change of the rows asked for: Debug; off, the next line starts fresh
        return;
    }
    std::uint32_t values = 0;  // two bits per request: its value (0..3)
    for (std::size_t i = 0; i < std::size(kRowRequests); ++i)
    {
        values |= (ReadField<std::uint8_t>(state, kRowRequests[i]) & 3u) << (2 * i);
    }
    if (values == ride.rows)
    {
        return;
    }
    ride.rows = values;
    try
    {
        std::wstring text;
        for (std::size_t i = 0; i < std::size(kRowRequests); ++i)
        {
            const std::uint32_t value = (values >> (2 * i)) & 3u;
            if (value != 0)
            {
                text += std::format(L" {:X}:{}", kRowRequests[i], value);
            }
        }
        platform::LogDebug(L"chocobo ride: {}'s chocobo asks for rows at{}", players::SeatName(ride.seat),
                           text.empty() ? L" none" : text.c_str());
    }
    catch (...)
    {
    }
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
    const Ride& ride = s.ride;
    try
    {
        platform::LogDebug(L"chocobo ride: driving, {} ticks ({} refused); its own slot 1 in {}'s scope {} times ({} refused); "
                      L"{:.0f} m from player 1's ride body; {}",
                      ride.ticks, ride.refused, players::SeatName(ride.seat), ride.handles, ride.handlesRefused,
                      MetresFromPlayerOne(ride.vehicle), LeaseText(ride));
    }
    catch (...)
    {
    }
}

std::uint64_t VehicleKey() noexcept
{
    return g_vehicleKey != nullptr ? ReadField<std::uint64_t>(g_vehicleKey, 0) : 0;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto find   = [&image](const platform::FunctionFact& fact) { return own_ride::FindOrZero(image, fact); };
    const auto rebase = [&image](std::uintptr_t ida) { return own_ride::RebaseOrZero(image, ida); };
    g_fn.getCoordinator = reinterpret_cast<GetCoordinatorFn>(find(kGetOrCreateVehicleCoordinator));
    g_fn.beginRide      = reinterpret_cast<BeginRideFn>(find(kBeginRide));
    g_fn.endRide        = reinterpret_cast<EndRideFn>(find(kEndRide));
    g_fn.switchAi       = reinterpret_cast<SwitchAiFn>(find(kSwitchAiControlAsync));
    g_fn.chocoboFor     = reinterpret_cast<ChocoboForFn>(find(kChocoboForCharacter));
    const auto root     = players::game_globals::Address(players::game_globals::Global::GameRoot);
    const auto key      = image.Rebase(kVehicleKeyName, sizeof(std::uint64_t));
    g_gameRoot          = root;
    g_vehicleKey        = key ? reinterpret_cast<const std::uint64_t*>(*key) : nullptr;
    g_coordVt           = rebase(kChocoboCoordinatorVTable);
    g_moveVt            = rebase(kChocoboMoveStateVTable);
    g_componentVt       = rebase(kBattlePlayerComponentVTable);
    g_moduleVt          = rebase(kChocoboCameraModuleVTable);
    g_installed = g_fn.getCoordinator != nullptr && g_fn.beginRide != nullptr && g_fn.endRide != nullptr &&
                  g_fn.switchAi != nullptr && g_fn.chocoboFor != nullptr && facts::InBattleKnown() && g_gameRoot != 0 &&
                  g_vehicleKey != nullptr && g_coordVt != 0 && g_moveVt != 0 && g_componentVt != 0 && g_moduleVt != 0 &&
                  scopes::leader_scope::Installed();
    platform::Log(L"chocobo ride: {}", g_installed ? L"installed (player 2 or 3 rides their own chocobo with their own coordinator)"
                                                   : L"UNAVAILABLE (a check failed; players 2 and 3 stay passengers)");
    return g_installed;
}

bool Tick(void* controller, void* member, const input::move_state::EngineTicks& engine) noexcept
{
    if (!g_installed || !players::generation::OnGameThread())
    {
        return false;
    }
    // The seat from its controller (the member's tick hands the seat's own).
    const int seat = players::generation::SeatOfController(controller);
    SeatRide* const ride = RideOf(seat);
    if (ride == nullptr)
    {
        return false;
    }
    SeatRide& s = *ride;
    if (s.phase == Phase::Idle)
    {
        HoldOffFollowWarp(s, seat, controller, member);  // a passenger's too
        void* chocobo = s.stopped ? nullptr : SeatedChocobo(s, controller, member);
        if (chocobo == nullptr)
        {
            return false;
        }
        Begin(s, seat, controller, member, chocobo);
        return s.phase != Phase::Idle;
    }
    if (s.ride.member != member || s.ride.controller != controller)
    {
        End(s, BySeat(seat, L"player 2's member changed", L"player 3's member changed"), true);
        return false;
    }
    if (s.phase == Phase::Ending)
    {
        End(s, s.ride.endWhy != nullptr ? s.ride.endWhy : L"ending", false);
        return s.phase != Phase::Idle;
    }
    if (s.phase == Phase::GettingOff)
    {
        // The game's getoff animation detaches her; the end then gives the seat the member back (the end order).
        const bool done = ride_rules::GetOffDone(Attached(member), facts::InBattle(),
                                                 Clock::now() - s.ride.getOffAt >= kGetOffWait,
                                                 !scopes::leader_scope::StillNames(s.ride.weak, s.ride.vehicle));
        if (done)
        {
            End(s, L"the game dismounted the party", false);
        }
        return s.phase != Phase::Idle;
    }
    bool getOff = false;
    if (const wchar_t* why = EndReason(s.ride, getOff))
    {
        if (getOff)
        {
            s.phase         = Phase::GettingOff;
            s.ride.getOffAt = Clock::now();
            if (void* state = players::actor_fields::MoveState(s.ride.vehicle); VTableOf(state) == g_moveVt)
            {
                input::member_movement::ClearRideRows(state);
                s.ride.rowsCleared = true;
            }
            LogLine(BySeat(seat, L"the game dismounted the party; player 2 waits for the getoff",
                           L"the game dismounted the party; player 3 waits for the getoff"),
                    s.ride);
            return true;
        }
        End(s, why, false);
        return s.phase != Phase::Idle;
    }
    if (s.ride.handlesFaulted)
    {
        End(s, L"the engine raised an exception inside the chocobo's own slot 1", true);
        s.stopped = true;
        platform::Log(L"chocobo ride STOPPED for this session: {} stays a passenger", players::SeatName(seat));
        return false;
    }
    void* state = players::actor_fields::MoveState(s.ride.vehicle);
    if (VTableOf(state) != g_moveVt)
    {
        End(s, L"the chocobo's move state changed", false);
        return s.phase != Phase::Idle;
    }
    // The seat's pad's Vehicle button is kept out while it rides on the party's ride: player 1 owns mounting (it would
    // dismount the party). On a chocobo of their own call the press is theirs: it ends their call (rides/chocobo_call.h).
    switch (own_ride::TickVehicle(s.ride, state, engine, chocobo_call::OwnCall(seat) ? 0 : VehicleKey()))
    {
        case own_ride::VehicleTick::Waiting: return true;  // the field ticker is idle: the seat's chocobo waits too
        case own_ride::VehicleTick::Faulted:
            End(s, L"the engine raised an exception inside the chocobo's tick", true);
            s.stopped = true;
            platform::Log(L"chocobo ride STOPPED for this session: {} stays a passenger", players::SeatName(seat));
            return false;
        case own_ride::VehicleTick::Ticked:
        case own_ride::VehicleTick::Refused: break;
    }
    LogRows(s.ride, state);
    LogSummary(s);
    return true;
}

bool Driving(int seat) noexcept
{
    const SeatRide* s = RideOf(seat);
    return s != nullptr && s->phase != Phase::Idle;
}

const void* DrivenChocobo(int seat) noexcept
{
    const SeatRide* s = RideOf(seat);
    if (s == nullptr)
    {
        return nullptr;
    }
    const bool riding = s->phase == Phase::Riding || s->phase == Phase::GettingOff;
    return riding && scopes::leader_scope::StillNames(s->ride.weak, s->ride.vehicle) ? s->ride.vehicle : nullptr;
}

const void* RiddenChocobo(int seat) noexcept
{
    const SeatRide* s = RideOf(seat);
    return s != nullptr && s->phase == Phase::Riding && scopes::leader_scope::StillNames(s->ride.weak, s->ride.vehicle)
               ? s->ride.vehicle
               : nullptr;
}

const void* DrivenMoveState(int seat) noexcept
{
    const SeatRide* s = RideOf(seat);
    if (s == nullptr || s->phase != Phase::Riding || !scopes::leader_scope::StillNames(s->ride.weak, s->ride.vehicle))
    {
        return nullptr;
    }
    const void* state = players::actor_fields::MoveState(s->ride.vehicle);
    return VTableOf(state) == g_moveVt && players::actor_fields::MoveStateCharacter(state) == s->ride.vehicle ? state
                                                                                                            : nullptr;
}

const void* RideManagerNow() noexcept
{
    return RideManager();
}

int MemberEntryState(const void* member) noexcept
{
    return EntryState(RideManager(), member);
}

int BoardsChocobo(const void* chocobo) noexcept
{
    if (chocobo == nullptr || !g_installed || !players::generation::OnGameThread())
    {
        return 0;
    }
    const std::byte* manager = RideManager();
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const SeatRide& s      = g_seats[seat];
        void*           member = players::generation::Active(seat) ? players::generation::MemberOf(seat) : nullptr;
        if (member != nullptr && !s.stopped && s.heldBack != chocobo && EntryChocobo(manager, member) == chocobo)
        {
            return seat;
        }
    }
    return 0;
}

int SeatRiding(const void* chocobo) noexcept
{
    for (int seat = 1; chocobo != nullptr && seat <= players::kSeatsBuilt; ++seat)
    {
        if (RiddenChocobo(seat) == chocobo)
        {
            return seat;
        }
    }
    return -1;
}

int SeatDriving(const void* chocobo) noexcept
{
    for (int seat = 1; chocobo != nullptr && seat <= players::kSeatsBuilt; ++seat)
    {
        if (DrivenChocobo(seat) == chocobo)
        {
            return seat;
        }
    }
    return -1;
}

bool RunAsRider(const void* chocobo, void (*call)(void* context), void* context) noexcept
{
    if (!players::generation::OnGameThread() || chocobo == nullptr)
    {
        return false;
    }
    const int seat = SeatRiding(chocobo);  // the seat from the chocobo it rides
    return seat >= 1 && input::member_movement::InPrimaryScope(seat, g_seats[seat].ride.member, call, context);
}

bool RunAsRiderWithButtons(const void* chocobo, void (*call)(void* context), void* context) noexcept
{
    if (!players::generation::OnGameThread() || chocobo == nullptr)
    {
        return false;
    }
    const int seat = SeatRiding(chocobo);
    return seat >= 1 && input::member_movement::InPrimaryScopeWithButtons(seat, g_seats[seat].ride.member, call, context);
}

bool TickHandles(void* state, float deltaTime, std::uintptr_t (*engine)(void* state, float deltaTime),
                 std::uintptr_t& returned) noexcept
{
    if (!players::generation::OnGameThread() || state == nullptr)
    {
        return false;
    }
    // The seat from the move state it drives.
    int seat = -1;
    for (int each = 1; each <= players::kSeatsBuilt && seat < 0; ++each)
    {
        seat = !g_seats[each].ride.handlesFaulted && state == DrivenMoveState(each) ? each : -1;
    }
    if (seat < 0)
    {
        return false;
    }
    Ride& ride = g_seats[seat].ride;
    // The game's ridden-vehicle test, outside and inside the scope, every 30th call: logged when it changes.
    const bool sample = ride.handles % 30 == 0;
    const auto tick   = input::member_movement::TickRideHandles(seat, ride.member, state, ride.vehicle, ride.component,
                                                         deltaTime, engine, sample);
    if (sample)
    {
        ride.riddenOutside = tick.riddenOutside;
        ride.riddenInside  = tick.riddenInside;
        if (ride.riddenOutside != ride.loggedOutside || ride.riddenInside != ride.loggedInside)
        {
            ride.loggedOutside = ride.riddenOutside;
            ride.loggedInside  = ride.riddenInside;
            try
            {
                platform::LogDebug(L"chocobo ride: the game's ridden-vehicle test for {}'s chocobo: {} outside {}'s "
                                   L"scope, {} inside (1 = the primary player's ridden chocobo: the game's anim sync "
                                   L"leaves it to the input)",
                                   players::SeatName(seat), ride.riddenOutside, players::SeatName(seat),
                                   ride.riddenInside);
            }
            catch (...)
            {
            }
        }
    }
    switch (tick.result)
    {
        case input::member_movement::RideTick::Ticked:
            ++ride.handles;
            returned = tick.returned;
            return true;
        case input::member_movement::RideTick::Faulted:
            ride.handlesFaulted = true;  // the ride ends on the next mod tick, outside the chocobo's own tick
            platform::Log(L"chocobo ride: the engine raised an exception inside the chocobo's own slot 1 in {}'s scope",
                          players::SeatName(seat));
            returned = tick.returned;
            return true;  // it ran (up to the exception): not run again
        case input::member_movement::RideTick::Refused:
            break;
    }
    ++ride.handlesRefused;
    return false;
}

void Release(int seat, const wchar_t* why) noexcept
{
    SeatRide* s = RideOf(seat);
    if (s == nullptr || !players::generation::OnGameThread())
    {
        return;
    }
    // The seat's player out: the chocobo the game seated their member on is the game's AI member's again, warps
    // included.
    LiftWarpHold(*s, seat, why);
    if (s->phase != Phase::Idle)
    {
        End(*s, why, true);
    }
}

void* Forget(int seat) noexcept
{
    SeatRide* ride = RideOf(seat);
    if (ride == nullptr)
    {
        return nullptr;
    }
    SeatRide& s       = *ride;
    if (players::generation::OnGameThread())
    {
        LiftWarpHold(s, seat, L"the member was destroyed");  // the chocobo may outlive her (the weak check)
    }
    void*     chocobo = s.phase != Phase::Idle ? s.ride.vehicle : nullptr;
    if (chocobo != nullptr)
    {
        platform::Log(L"chocobo ride: the member was destroyed while {} rode; the ride is forgotten",
                      players::SeatName(seat));
        s.stopped = true;  // the seat's coordinator is left as the game left it: no ride is begun again this session
    }
    s.phase        = Phase::Idle;
    s.ride         = {};
    s.heldBack     = nullptr;
    s.warpHeld     = nullptr;
    s.warpHeldWeak = 0;
    s.warpTried    = nullptr;
    return chocobo;
}

}  // namespace lss::rides::chocobo_ride
