#include "player_two/chocobo_ride.h"

#include "player_two/ride_rules.h"
#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>

#include "platform/build_value.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/charapop_facts.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/member_movement.h"
#include "player_two/object_fields.h"
#include "player_two/party_member.h"
#include "player_two/ride_probe.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

namespace lss::player_two::chocobo_ride {

namespace {

using RC::Unreal::UObject;
using Clock = std::chrono::steady_clock;

// The game's functions (RE-p2-chocobo-ride.md §9.4, §7.1; entry bytes read in IDA).
constexpr std::uint8_t kGetCoordinatorBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x56, 0x57,
                                                 0x41, 0x56, 0xB8, 0x50, 0x00, 0x00, 0x00, 0xE8, 0x4C, 0x7F, 0xEA, 0x00};
constexpr platform::FunctionFact kGetCoordinator{L"EndPC_GetOrCreateVehicleCoordinator", 0x141206FCC, kGetCoordinatorBytes};
using GetCoordinatorFn = void* (*)(void* controller, std::uint8_t type);

constexpr std::uint8_t kBeginRideBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48,
                                            0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x41, 0x56, 0xB8, 0x30, 0x00};
constexpr platform::FunctionFact kBeginRide{L"ChocoboCoordinator_BeginRide_SetActive", 0x143DAB214, kBeginRideBytes};
using BeginRideFn = void (*)(void* coordinator, void* chocobo, void* rider, bool instant, bool applyNow);

constexpr std::uint8_t kEndRideBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48,
                                          0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x41, 0x56, 0xB8, 0x90, 0x00};
constexpr platform::FunctionFact kEndRide{L"ChocoboCoordinator_EndRide_ClearActive", 0x143DAAB98, kEndRideBytes};
struct CharacterArray  // TArray<AEndCharacter*>, which EndRide fills with the riders it let go of
{
    void**       data = nullptr;
    std::int32_t num  = 0;
    std::int32_t max  = 0;
};
// The game's 16-byte get-off parameters EndRide reads (byte 1 to coord+0x145, passed to the rider's AI ride-end; byte 2
// to coord+0x146). The game's forced end passes its own, all zero unless its cutscene clip is flagged
// (RE-p2-chocobo-ride-end-fault.md). A null pointer here faults four instructions in (the v0.45.0 end fault).
struct alignas(8) GetOffParams
{
    std::uint8_t bytes[16] = {};
};
using EndRideFn = void (*)(void* coordinator, const GetOffParams* params, CharacterArray* out);

constexpr std::uint8_t kSwitchAiBytes[] = {0x40, 0x55, 0x48, 0x8D, 0x6C, 0x24, 0xA9, 0xB8, 0x90, 0x00, 0x00, 0x00,
                                           0xE8, 0x23, 0x93, 0x5B, 0x01, 0x48, 0x2B, 0xE0, 0x80, 0x3D, 0x3A, 0x72};
constexpr platform::FunctionFact kSwitchAi{L"AEndCharacter::SwitchAIControl_Async", 0x140AF5BFC, kSwitchAiBytes};
using SwitchAiFn = void (*)(void* character, std::uint8_t toPlayer, void* controller);

constexpr std::uint8_t kChocoboForBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xB8, 0xED, 0x3A, 0xFE,
                                             0x48, 0x2B, 0xE0, 0x45, 0x33, 0xC0, 0x4C, 0x8B, 0xCA, 0x48, 0x8B, 0xD9};
constexpr platform::FunctionFact kChocoboFor{L"ChocoboRideMgr_GetChocoboForCharacter", 0x143D0016C, kChocoboForBytes};
using ChocoboForFn = void* (*)(const void* manager, const void* character);
using GlobalTestFn = std::uint8_t (*)();
using GetterFn     = void* (*)(const void* object);

// Objects and fields (§1.1, §9.4; this build).
constexpr std::uint32_t  kRootRideManager   = 0x270;
constexpr std::uint32_t  kManagerGetOff     = 0xEB0;   // u8
constexpr std::uint32_t  kMemberEntries     = 0x40;
constexpr std::uint32_t  kMemberEntrySize   = 0x2C;
constexpr std::uint8_t   kPlayerTypes       = 15;
constexpr std::uint8_t   kEntryRiding       = 4;       // the member entry's state byte while she rides
constexpr std::uint8_t   kChocoboType       = 1;       // the vehicle type of a chocobo coordinator
constexpr std::uint32_t  kActiveVehicle     = 0x5B8;   // AEndPlayerController::ActiveVehicleCoordinator
constexpr std::uint32_t  kCoordState        = 0x58;    // the coordinator's state machine: current row -> u32 id
constexpr std::uint32_t  kCoordPending      = 0x60;    // ... pending row (null when none)
constexpr std::uint32_t  kCoordIdle         = 0;
constexpr std::uint32_t  kCoordRiding       = 2;
constexpr platform::PerBuild<std::uint32_t> kChocoboRidden     = {0x1924, 0x1944};  // u8; 1.005: ChocoboCoordinator GetRideBody 0x143B13E00 / FinishGetOff 0x143B14F58 [rdi+1944h]
constexpr platform::PerBuild<std::uint32_t> kChocoboRideObject = {0x1918, 0x1938};  // 1.005: Chocobo_ReleaseRideObject 0x1411903AC, chocobo tick 0x140A413DC [rbx+1938h]
constexpr std::uint32_t  kControlHolder     = 0x638;   // its vt+0x40 gives the input component
constexpr std::uint32_t  kHolderComponent   = 0x40;
constexpr std::uint32_t  kAttachParent      = 0xC0;    // USceneComponent::AttachParent
constexpr std::uint32_t  kViewTarget        = 0x1000;  // PCM ViewTarget.Target
constexpr std::uint32_t  kRideSubjectField  = 0x2CC8;  // PCM, weak
constexpr std::uint32_t  kFieldOperator     = 0x2CB0;  // PCM: the Field operator
constexpr std::uint32_t  kOperatorModules   = 0x6E0;   // op: the module slots (16 bytes each)
constexpr std::uint32_t  kModuleSlot15      = 15 * 16;
constexpr std::uintptr_t kChocoboCoordVt    = 0x1467EB420;
constexpr std::uintptr_t kChocoboMoveVt     = 0x1466B9E18;
constexpr std::uintptr_t kInputComponentVt  = 0x145795ED8;  // BattlePlayerControllerComp
constexpr std::uintptr_t kCameraModuleVt    = 0x145CE2C50;  // FEndChocoboCameraModule
constexpr std::uintptr_t kVehicleKeyName    = 0x1491E4538;  // FName "Vehicle": the getoff button
// The chocobo move state's row request bytes (handle +0x48): the summon rows, then Dash, Dismount, Jet, Scent, Scour
// (RE-p2-chocobo-rows.md §2); 1 = lit, 2 = dim.
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
    GlobalTestFn     inBattle       = nullptr;
};

Functions      g_fn;
std::uintptr_t g_gameRoot = 0, g_coordVt = 0, g_moveVt = 0, g_componentVt = 0, g_moduleVt = 0;
const std::uint64_t* g_vehicleKey = nullptr;
bool           g_installed = false;

enum class Phase
{
    Idle,
    Riding,      // player 2's coordinator rides; the mod ticks the chocobo
    GettingOff,  // the game dismounted her; waiting for her getoff animation to detach her
    Ending,      // ending, waiting for player 2's coordinator to reach riding (the game's end needs it)
};

// Game thread.
struct Ride
{
    void*         playerTwo   = nullptr;
    void*         member      = nullptr;
    void*         chocobo     = nullptr;
    void*         coordinator = nullptr;  // player 2's own chocobo coordinator
    std::uint64_t weak        = 0;        // the chocobo, as the game's weak pointer names it
    const void*   component   = nullptr;  // its input component (the leader answer's second yes)
    std::uint32_t ticks       = 0, refused = 0;
    std::uint32_t rows        = 0;        // the row request values last logged
    // Its slot 1 (the actor tick's handles and anim sync) in player 2's scope: counts, and the game's ridden-vehicle
    // test outside/inside the scope as last sampled and as last logged (-1 not yet).
    std::uint32_t handles = 0, handlesRefused = 0;
    std::int32_t  riddenOutside = -1, riddenInside = -1, loggedOutside = -2, loggedInside = -2;
    bool          handlesFaulted = false;  // the engine raised an exception in it: the ride ends on the next tick
    Clock::time_point getOffAt{};
    std::uint32_t endTries    = 0;
    bool          rowsCleared = false;
    const wchar_t* endWhy     = nullptr;  // Ending: why
};
Phase             g_phase    = Phase::Idle;
Ride              g_ride{};
const void*       g_heldBack = nullptr;  // a chocobo not to board again until its ridden flag clears
bool              g_stopped  = false;    // a ride failed: player 2 stays a passenger this session
std::int32_t      g_rootOffset = -1;
Clock::time_point g_lastSummary{};
CharacterArray    g_released{};          // reused for every EndRide: the game keeps one allocation in it

const void* RootOf(void* actor) noexcept
{
    if (actor == nullptr)
    {
        return nullptr;
    }
    // The one-time reflection lookup dereferences the actor unguarded: only on an actor the game still names.
    if (g_rootOffset < 0 && leader_scope::StillNames(leader_scope::WeakOf(actor), actor))
    {
        try
        {
            auto* property = static_cast<UObject*>(actor)->GetPropertyByNameInChain(STR("RootComponent"));
            if (property == nullptr || property->GetSize() != sizeof(void*))
            {
                return nullptr;
            }
            g_rootOffset = property->GetOffset_Internal();
        }
        catch (...)
        {
            return nullptr;
        }
    }
    return g_rootOffset < 0 ? nullptr : ReadPointer(actor, static_cast<std::uint32_t>(g_rootOffset));
}

bool Attached(void* member) noexcept
{
    return ReadPointer(RootOf(member), kAttachParent) != nullptr;
}

const std::byte* RideManager() noexcept
{
    const void* root = ReadPointer(reinterpret_cast<const void*>(g_gameRoot), 0);
    return static_cast<const std::byte*>(ReadPointer(root, kRootRideManager));
}

/// The member's entry state byte in the ride manager, or -1.
int EntryState(const std::byte* manager, const void* member) noexcept
{
    const auto type = party_member::PlayerTypeOf(member);
    if (manager == nullptr || !type || *type >= kPlayerTypes)
    {
        return -1;
    }
    return ReadField<std::uint8_t>(manager + kMemberEntries + kMemberEntrySize * *type, 0);
}

bool InBattle() noexcept
{
    std::uint8_t answer = 0;
    return g_fn.inBattle != nullptr && platform::CallGuarded(g_fn.inBattle, answer) && answer != 0;
}

/// A chocobo coordinator's state id, or -1.
std::int32_t CoordState(const void* coordinator) noexcept
{
    const void* row = VTableOf(coordinator) == g_coordVt ? ReadPointer(coordinator, kCoordState) : nullptr;
    return row != nullptr ? ReadField<std::int32_t>(row, 0) : -1;
}

/// The game's EndRide precondition (ChocoboCoordinator_IsRidingState2_NoPending 0x143DA1044).
bool CoordRiding(const void* coordinator) noexcept
{
    return CoordState(coordinator) == static_cast<std::int32_t>(kCoordRiding) && ReadPointer(coordinator, kCoordPending) == nullptr;
}

std::wstring Who(const void* controller, const void* playerTwo)
{
    if (controller == nullptr)
    {
        return L"none";
    }
    if (controller == playerTwo)
    {
        return L"PC2";
    }
    if (controller == generation::PlayerOneController())
    {
        return L"PC1";
    }
    return std::format(L"{}", controller);
}

/// Player 2's camera's slot-15 module is a chocobo camera module (the ride framing).
bool RideCameraOn(const void* playerTwo) noexcept
{
    const void* pcm     = ReadPointer(playerTwo, facts::kControllerCameraManager);
    const void* op      = ReadPointer(pcm, kFieldOperator);
    const void* modules = ReadPointer(op, kOperatorModules);
    const void* module  = modules != nullptr ? ReadPointer(modules, kModuleSlot15) : nullptr;
    return module != nullptr && VTableOf(module) == g_moduleVt;
}

/// The possession and ride fields, for the log.
std::wstring LeaseText(const Ride& ride)
{
    const void* pcm = ReadPointer(ride.playerTwo, facts::kControllerCameraManager);
    const void* pawn = ReadPointer(ride.playerTwo, facts::kControllerPawn);
    return std::format(L"member controller {} (effective {}); chocobo controller {} (effective {}); PC2's pawn {}, leader {}, "
                       L"active coordinator {} (state {}); player 2's camera ride subject {}, ride framing {}, view target {}",
                       Who(ReadPointer(ride.member, facts::kPawnController), ride.playerTwo),
                       Who(ReadPointer(ride.member, facts::kCharacterEffectiveController), ride.playerTwo),
                       Who(ReadPointer(ride.chocobo, facts::kPawnController), ride.playerTwo),
                       Who(ReadPointer(ride.chocobo, facts::kCharacterEffectiveController), ride.playerTwo),
                       pawn == ride.chocobo ? L"the chocobo" : pawn == ride.member ? L"the member" : pawn ? L"other" : L"none",
                       ReadPointer(ride.playerTwo, facts::kControllerLeader) == ride.member ? L"the member" : L"other",
                       ReadPointer(ride.playerTwo, kActiveVehicle) == ride.coordinator && ride.coordinator ? L"player 2's own"
                       : ReadPointer(ride.playerTwo, kActiveVehicle) != nullptr                             ? L"other"
                                                                                                            : L"none",
                       CoordState(ride.coordinator),
                       leader_scope::ObjectAt(static_cast<const std::byte*>(pcm) + kRideSubjectField) == ride.chocobo
                           ? L"the chocobo"
                           : L"not the chocobo",
                       RideCameraOn(ride.playerTwo) ? L"on" : L"off",
                       ReadPointer(pcm, kViewTarget) == ride.member    ? L"the member"
                       : ReadPointer(pcm, kViewTarget) == ride.chocobo ? L"the chocobo"
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

/// The chocobo's input component: its control holder's getter (+0x638 -> vt+0x40), checked by vtable.
const void* InputComponentOf(const void* chocobo) noexcept
{
    const void* holder = ReadPointer(chocobo, kControlHolder);
    const void* vt     = reinterpret_cast<const void*>(VTableOf(holder));
    const auto  getter = vt != nullptr ? ReadField<GetterFn>(vt, kHolderComponent) : nullptr;
    void*       found  = nullptr;
    if (getter == nullptr || !platform::CallGuarded(getter, found, holder))
    {
        return nullptr;
    }
    return VTableOf(found) == g_componentVt ? found : nullptr;
}

bool PlayerTwoHoldsMember(const void* member, const void* playerTwo) noexcept
{
    return ReadPointer(member, facts::kCharacterEffectiveController) == playerTwo &&
           ReadPointer(playerTwo, facts::kControllerPawn) == member;
}

struct BeginCall
{
    void* playerTwo   = nullptr;
    void* chocobo     = nullptr;
    void* member      = nullptr;
    void* coordinator = nullptr;
    bool  called      = false;
    bool  ok          = false;
};

/// Inside the primary scope (player 2): the game's coordinator for player 2's controller, then the leader's boarding
/// for her (instant, applied now: the boarding enter runs inside this call, so inside the scope; §9.2).
void RunBegin(void* context)
{
    auto* call = static_cast<BeginCall*>(context);
    if (!platform::CallGuarded(g_fn.getCoordinator, call->coordinator, call->playerTwo, kChocoboType) ||
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

struct RestoreCall
{
    void* member    = nullptr;
    void* playerTwo = nullptr;
    bool  ok        = false;
};

void RunRestore(void* context)
{
    auto* call = static_cast<RestoreCall*>(context);
    call->ok   = platform::CallGuarded(g_fn.switchAi, call->member, std::uint8_t{1}, call->playerTwo);
}

/// The ride is over for the mod: player 2's controller must hold the member again. If the game's end did not give
/// her back (or could not run), the game's possession call; if even that fails, stopped for the session.
void Finish(const Ride& ride, const wchar_t* why, const wchar_t* how) noexcept
{
    g_phase    = Phase::Idle;
    g_heldBack = ride.chocobo;
    bool held  = PlayerTwoHoldsMember(ride.member, ride.playerTwo);
    if (!held && ReadPointer(ride.member, facts::kPawnController) != ride.playerTwo)
    {
        RestoreCall restore{ride.member, ride.playerTwo};
        member_movement::InPrimaryScope(ride.member, &RunRestore, &restore);
        held = PlayerTwoHoldsMember(ride.member, ride.playerTwo);
        platform::Log(L"chocobo ride: the end left the member to her AI; the game's possession call {}",
                      held ? L"gave her back to player 2" : L"DID NOT give her back");
    }
    try
    {
        platform::Log(L"chocobo ride: player 2 is off their chocobo ({}; {}; {} ticks driven, {} refused): {}", why, how,
                      ride.ticks, ride.refused, LeaseText(ride));
    }
    catch (...)
    {
    }
    if (!held)
    {
        g_stopped = true;
        platform::Log(L"chocobo ride STOPPED: player 2's controller does not hold the member after the ride; player 2 stays "
                      L"a passenger this session (the session's own checks take it from here)");
    }
}

/// Ends player 2's ride with the game's own end (§9.2: the possession back, the ride object, the camera module and
/// ride subject, the active coordinator cleared). `force`: now, whatever the coordinator's state (a give-back, a
/// leave); otherwise a coordinator not riding yet is retried on the next ticks.
void End(const wchar_t* why, bool force) noexcept
{
    if (g_phase == Phase::Idle)
    {
        return;
    }
    Ride& ride = g_ride;
    if (void* state = ReadPointer(ride.chocobo, facts::kCharacterMoveState);
        !ride.rowsCleared && leader_scope::StillNames(ride.weak, ride.chocobo) && VTableOf(state) == g_moveVt)
    {
        member_movement::ClearRideRows(state);  // player 2's rows go with the ride
        ride.rowsCleared = true;
    }
    if (!leader_scope::StillNames(ride.weak, ride.chocobo))
    {
        // The game's end reads the coordinator's chocobo: not with a destroyed one. Player 2's coordinator stays as the
        // game left it, so no ride is begun again this session.
        g_stopped = true;
        Finish(ride, why, L"the chocobo is gone, the game's end skipped; rides stopped for this session");
        return;
    }
    if (!CoordRiding(ride.coordinator))
    {
        if (ride_rules::EndRetryAllowed(force, ride.endTries, kEndRetryTicks))
        {
            g_phase     = Phase::Ending;  // boarding's completion reaches riding on player 2's next input tick
            ride.endWhy = why;
            return;
        }
        // The game's end cannot run: player 2's coordinator stays half-boarded (its menu locks on, the chocobo without
        // its AI). Reached only by a forced end (a give-back, a leave, a map load) within the tick boarding takes, or
        // a coordinator that never reaches riding.
        g_stopped = true;
        platform::Log(L"WARNING chocobo ride: the game's end of player 2's coordinator could not run (state {}); its "
                      L"boarding is left as it was",
                      CoordState(ride.coordinator));
        Finish(ride, why, L"player 2's coordinator was not riding, the game's end skipped; rides stopped for this session");
        return;
    }
    EndCall call{ride.coordinator};
    const bool scoped = member_movement::InPrimaryScope(ride.member, &RunEnd, &call);
    if (scoped && !call.ok)
    {
        // Where the game's end stopped: the steps after the fault did not run (EndRide's order: rider detached, swap
        // back, ride object and camera subject cleared, active coordinator cleared, state 0 requested).
        platform::Log(L"WARNING chocobo ride: the game's end of player 2's coordinator raised {}; after it: coordinator "
                      L"state {}, player 2's active coordinator {}, the chocobo's controller {}, the member's {}",
                      platform::DescribeFault(call.fault).text, CoordState(ride.coordinator),
                      ReadPointer(ride.playerTwo, kActiveVehicle) == ride.coordinator ? L"still it" : L"cleared",
                      ReadPointer(ride.chocobo, facts::kPawnController) == ride.playerTwo ? L"PC2" : L"not PC2",
                      ReadPointer(ride.member, facts::kPawnController) == ride.playerTwo ? L"PC2" : L"not PC2");
    }
    const Ride ended  = ride;
    Finish(ended, why,
           !scoped   ? L"the game's end not run (no scope)"
           : call.ok ? (ReadPointer(ended.playerTwo, kActiveVehicle) == nullptr ? L"the game's end ran"
                                                                                : L"the game's end ran, the coordinator is still active")
                     : L"the game's end FAULTED");
}

/// The game has seated player 2's member on her own chocobo (§9.3: her chocobo appears by her and she is seated where
/// she stands; taken at once, before the chocobo's follow AI can pull it toward player 1). Null when not.
void* SeatedChocobo(void* playerTwo, void* member) noexcept
{
    if (ReadPointer(member, facts::kPawnController) != playerTwo || ReadPointer(playerTwo, facts::kControllerLeader) != member)
    {
        return nullptr;
    }
    const std::byte* manager = RideManager();
    void*            chocobo = nullptr;
    if (manager == nullptr || ReadField<std::uint8_t>(manager, kManagerGetOff) != 0 || EntryState(manager, member) != kEntryRiding ||
        !platform::CallGuarded(g_fn.chocoboFor, chocobo, static_cast<const void*>(manager), static_cast<const void*>(member)) ||
        chocobo == nullptr)
    {
        return nullptr;
    }
    switch (ride_rules::SeatedVerdict(ReadField<std::uint8_t>(chocobo, kChocoboRidden) != 0, g_heldBack == chocobo,
                                      Attached(member), InBattle()))
    {
        case ride_rules::Seated::Unridden:
            if (g_heldBack == chocobo)
            {
                g_heldBack = nullptr;  // that ride is over: the next mount may be boarded
            }
            return nullptr;
        case ride_rules::Seated::HeldBack:
        case ride_rules::Seated::Detached:
        case ride_rules::Seated::Battle: return nullptr;
        case ride_rules::Seated::Seated: break;
    }
    return chocobo;
}

void Begin(void* playerTwo, void* member, void* chocobo) noexcept
{
    // The boarding's swap runs the game's player-core test on her (§7.4): as player 2, it must pass.
    const auto gate = member_movement::PlayerCoreAsPlayerTwo(member, member);
    if (gate.answer != 1 || gate.primary != playerTwo)
    {
        g_heldBack = chocobo;
        platform::Log(L"chocobo ride: not taken (the boarding's player-core test as player 2 reads {}, primary {}); player 2 "
                      L"stays a passenger for this ride",
                      gate.answer, gate.primary == playerTwo ? L"PC2" : L"not PC2");
        return;
    }
    const void* oldObject = ReadPointer(chocobo, kChocoboRideObject);
    BeginCall   call{playerTwo, chocobo, member};
    ride_probe::Boarding(L"before the game's boarding", chocobo);
    member_movement::InPrimaryScope(member, &RunBegin, &call);
    ride_probe::Boarding(L"after the game's boarding", chocobo);
    g_ride = {};
    g_ride.playerTwo   = playerTwo;
    g_ride.member      = member;
    g_ride.chocobo     = chocobo;
    g_ride.coordinator = call.coordinator;
    g_ride.weak        = leader_scope::WeakOf(chocobo);
    if (!call.called)
    {
        g_heldBack = chocobo;
        g_stopped  = true;
        platform::Log(L"chocobo ride STOPPED for this session: player 2's coordinator {} (state {}); player 2 stays a passenger",
                      call.coordinator == nullptr ? L"could not be made" : L"is not idle", CoordState(call.coordinator));
        return;
    }
    g_phase          = Phase::Riding;
    void* state      = ReadPointer(chocobo, facts::kCharacterMoveState);
    const bool owned = call.ok && ReadPointer(chocobo, facts::kCharacterEffectiveController) == playerTwo &&
                       ReadPointer(playerTwo, facts::kControllerPawn) == chocobo &&
                       ReadPointer(playerTwo, kActiveVehicle) == call.coordinator && VTableOf(state) == g_moveVt;
    g_ride.component = owned ? InputComponentOf(chocobo) : nullptr;
    if (!owned || g_ride.component == nullptr)
    {
        LogLine(call.ok ? L"the boarding did not hand the chocobo to player 2; ending it" : L"the boarding FAULTED; ending it",
                g_ride);
        g_stopped = true;  // no new ride; this one ends with the game's end once the coordinator rides (Ending)
        platform::Log(L"chocobo ride STOPPED for this session: player 2 stays a passenger");
        End(L"the boarding did not complete", false);
        return;
    }
    g_lastSummary = Clock::now();
    try
    {
        platform::Log(L"chocobo ride: player 2 boards their chocobo with their own coordinator (the game's boarding, instant; "
                      L"ride object {}): {}",
                      ReadPointer(chocobo, kChocoboRideObject) != oldObject ? L"replaced" : L"kept", LeaseText(g_ride));
    }
    catch (...)
    {
    }
}

/// Why the ride ends now, or null. `getOff`: the game dismounted her (the ridden flag cleared): wait for the getoff.
const wchar_t* EndReason(const Ride& ride, bool& getOff) noexcept
{
    ride_rules::EndReads reads;
    reads.gone = !leader_scope::StillNames(ride.weak, ride.chocobo);
    if (!reads.gone)  // the rest is read only while the chocobo is still there
    {
        reads.ridden      = ReadField<std::uint8_t>(ride.chocobo, kChocoboRidden) != 0;
        reads.inBattle    = InBattle();
        reads.entryRiding = EntryState(RideManager(), ride.member) == kEntryRiding;
        reads.attached    = Attached(ride.member);
        reads.holds       = ReadPointer(ride.playerTwo, facts::kControllerPawn) == ride.chocobo &&
                            ReadPointer(ride.chocobo, facts::kCharacterEffectiveController) == ride.playerTwo;
    }
    const auto reason = ride_rules::EndReasonOf(reads);
    getOff            = reason == ride_rules::EndReason::Dismounted;
    return ride_rules::Describe(reason);
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
        platform::LogDebug(L"chocobo ride: player 2's chocobo asks for rows at{}", text.empty() ? L" none" : text.c_str());
    }
    catch (...)
    {
    }
}

void LogSummary(const Ride& ride) noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;  // a periodic status line: Debug
    }
    const auto now = Clock::now();
    if (now - g_lastSummary < kSummaryEvery)
    {
        return;
    }
    g_lastSummary = now;
    float two[3]{}, one[3]{};
    const void* oneBody = ReadPointer(generation::PlayerOneController(), facts::kControllerPawn);
    const bool  read    = charapop::ActorLocation(ride.chocobo, two) && charapop::ActorLocation(const_cast<void*>(oneBody), one);
    try
    {
        platform::LogDebug(L"chocobo ride: driving, {} ticks ({} refused); its own slot 1 in player 2's scope {} times ({} refused); "
                      L"{:.0f} m from player 1's ride body; {}",
                      ride.ticks, ride.refused, ride.handles, ride.handlesRefused,
                      read ? charapop::Distance(one, two) / 100.0f : -1.0f, LeaseText(ride));
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
    const auto find = [&image](const platform::FunctionFact& fact) {
        const auto live = platform::FindFunction(image, fact);
        return live ? *live : std::uintptr_t{0};
    };
    const auto rebase = [&image](std::uintptr_t ida) {
        const auto live = image.Rebase(ida);
        return live ? *live : std::uintptr_t{0};
    };
    g_fn.getCoordinator = reinterpret_cast<GetCoordinatorFn>(find(kGetCoordinator));
    g_fn.beginRide      = reinterpret_cast<BeginRideFn>(find(kBeginRide));
    g_fn.endRide        = reinterpret_cast<EndRideFn>(find(kEndRide));
    g_fn.switchAi       = reinterpret_cast<SwitchAiFn>(find(kSwitchAi));
    g_fn.chocoboFor     = reinterpret_cast<ChocoboForFn>(find(kChocoboFor));
    g_fn.inBattle       = reinterpret_cast<GlobalTestFn>(find(facts::kIsInBattle));
    const auto root     = image.Rebase(charapop::kGameRoot, sizeof(void*));
    const auto key      = image.Rebase(kVehicleKeyName, sizeof(std::uint64_t));
    g_gameRoot          = root ? *root : 0;
    g_vehicleKey        = key ? reinterpret_cast<const std::uint64_t*>(*key) : nullptr;
    g_coordVt           = rebase(kChocoboCoordVt);
    g_moveVt            = rebase(kChocoboMoveVt);
    g_componentVt       = rebase(kInputComponentVt);
    g_moduleVt          = rebase(kCameraModuleVt);
    g_installed = g_fn.getCoordinator != nullptr && g_fn.beginRide != nullptr && g_fn.endRide != nullptr &&
                  g_fn.switchAi != nullptr && g_fn.chocoboFor != nullptr && g_fn.inBattle != nullptr && g_gameRoot != 0 &&
                  g_vehicleKey != nullptr && g_coordVt != 0 && g_moveVt != 0 && g_componentVt != 0 && g_moduleVt != 0 &&
                  leader_scope::Install(image);
    platform::Log(L"chocobo ride: {}", g_installed ? L"installed (player 2 rides their own chocobo with their own coordinator)"
                                                   : L"UNAVAILABLE (a check failed; player 2 stays a passenger)");
    return g_installed;
}

bool Tick(void* playerTwoController, void* member, const move_state::EngineTicks& engine) noexcept
{
    if (!g_installed || !generation::OnGameThread())
    {
        return false;
    }
    if (g_phase == Phase::Idle)
    {
        void* chocobo = g_stopped ? nullptr : SeatedChocobo(playerTwoController, member);
        if (chocobo == nullptr)
        {
            return false;
        }
        Begin(playerTwoController, member, chocobo);
        return g_phase != Phase::Idle;
    }
    if (g_ride.member != member || g_ride.playerTwo != playerTwoController)
    {
        End(L"player 2's member changed", true);
        return false;
    }
    if (g_phase == Phase::Ending)
    {
        End(g_ride.endWhy != nullptr ? g_ride.endWhy : L"ending", false);
        return g_phase != Phase::Idle;
    }
    if (g_phase == Phase::GettingOff)
    {
        // The game's getoff animation detaches her; the end then gives player 2 the member back (§9.2, the end order).
        const bool done = ride_rules::GetOffDone(Attached(member), InBattle(), Clock::now() - g_ride.getOffAt >= kGetOffWait,
                                                 !leader_scope::StillNames(g_ride.weak, g_ride.chocobo));
        if (done)
        {
            End(L"the game dismounted the party", false);
        }
        return g_phase != Phase::Idle;
    }
    bool getOff = false;
    if (const wchar_t* why = EndReason(g_ride, getOff))
    {
        if (getOff)
        {
            g_phase         = Phase::GettingOff;
            g_ride.getOffAt = Clock::now();
            if (void* state = ReadPointer(g_ride.chocobo, facts::kCharacterMoveState); VTableOf(state) == g_moveVt)
            {
                member_movement::ClearRideRows(state);
                g_ride.rowsCleared = true;
            }
            LogLine(L"the game dismounted the party; player 2 waits for the getoff", g_ride);
            return true;
        }
        End(why, false);
        return g_phase != Phase::Idle;
    }
    if (g_ride.handlesFaulted)
    {
        End(L"the engine raised an exception inside the chocobo's own slot 1", true);
        g_stopped = true;
        platform::Log(L"chocobo ride STOPPED for this session: player 2 stays a passenger");
        return false;
    }
    if (engine.count == 0 && !engine.rideTicked)
    {
        return true;  // the field ticker is idle (a menu, an event): player 2's chocobo waits too
    }
    void* state = ReadPointer(g_ride.chocobo, facts::kCharacterMoveState);
    if (VTableOf(state) != g_moveVt)
    {
        End(L"the chocobo's move state changed", false);
        return g_phase != Phase::Idle;
    }
    // Pad 2's Vehicle button is kept out while player 2 rides: player 1 owns mounting (it would dismount the party).
    const auto result = member_movement::TickRide(playerTwoController, member, state, g_ride.component,
                                                  engine.rideTicked ? engine.rideDeltaTime : engine.deltaTime, VehicleKey());
    if (result == member_movement::RideTick::Faulted)
    {
        End(L"the engine raised an exception inside the chocobo's tick", true);
        g_stopped = true;
        platform::Log(L"chocobo ride STOPPED for this session: player 2 stays a passenger");
        return false;
    }
    ++(result == member_movement::RideTick::Ticked ? g_ride.ticks : g_ride.refused);
    LogRows(g_ride, state);
    LogSummary(g_ride);
    return true;
}

bool Driving() noexcept
{
    return g_phase != Phase::Idle;
}

const void* DrivenChocobo() noexcept
{
    return (g_phase == Phase::Riding || g_phase == Phase::GettingOff) && leader_scope::StillNames(g_ride.weak, g_ride.chocobo)
               ? g_ride.chocobo
               : nullptr;
}

const void* RiddenChocobo() noexcept
{
    return g_phase == Phase::Riding && leader_scope::StillNames(g_ride.weak, g_ride.chocobo) ? g_ride.chocobo : nullptr;
}

const void* DrivenMoveState() noexcept
{
    if (g_phase != Phase::Riding || !leader_scope::StillNames(g_ride.weak, g_ride.chocobo))
    {
        return nullptr;
    }
    const void* state = ReadPointer(g_ride.chocobo, facts::kCharacterMoveState);
    return VTableOf(state) == g_moveVt && ReadPointer(state, facts::kMoveStateCharacter) == g_ride.chocobo ? state : nullptr;
}

bool RunAsRider(const void* chocobo, void (*call)(void* context), void* context) noexcept
{
    if (!generation::OnGameThread() || chocobo == nullptr || chocobo != RiddenChocobo())
    {
        return false;
    }
    return member_movement::InPrimaryScope(g_ride.member, call, context);
}

bool TickHandles(void* state, float deltaTime, std::uintptr_t (*engine)(void* state, float deltaTime),
                 std::uintptr_t& returned) noexcept
{
    if (!generation::OnGameThread() || state == nullptr || g_ride.handlesFaulted || state != DrivenMoveState())
    {
        return false;
    }
    // The game's ridden-vehicle test, outside and inside the scope, every 30th call: logged when it changes.
    const bool sample = g_ride.handles % 30 == 0;
    const auto tick   = member_movement::TickRideHandles(g_ride.member, state, g_ride.chocobo, g_ride.component, deltaTime,
                                                         engine, sample);
    if (sample)
    {
        g_ride.riddenOutside = tick.riddenOutside;
        g_ride.riddenInside  = tick.riddenInside;
        if (g_ride.riddenOutside != g_ride.loggedOutside || g_ride.riddenInside != g_ride.loggedInside)
        {
            g_ride.loggedOutside = g_ride.riddenOutside;
            g_ride.loggedInside  = g_ride.riddenInside;
            try
            {
                platform::LogDebug(L"chocobo ride: the game's ridden-vehicle test for player 2's chocobo: {} outside player 2's "
                              L"scope, {} inside (1 = the primary player's ridden chocobo: the game's anim sync leaves it "
                              L"to the input)",
                              g_ride.riddenOutside, g_ride.riddenInside);
            }
            catch (...)
            {
            }
        }
    }
    switch (tick.result)
    {
        case member_movement::RideTick::Ticked:
            ++g_ride.handles;
            returned = tick.returned;
            return true;
        case member_movement::RideTick::Faulted:
            g_ride.handlesFaulted = true;  // the ride ends on the next mod tick, outside the chocobo's own tick
            platform::Log(L"chocobo ride: the engine raised an exception inside the chocobo's own slot 1 in player 2's scope");
            returned = tick.returned;
            return true;  // it ran (up to the exception): not run again
        case member_movement::RideTick::Refused:
            break;
    }
    ++g_ride.handlesRefused;
    return false;
}

void Release(const wchar_t* why) noexcept
{
    if (g_phase != Phase::Idle && generation::OnGameThread())
    {
        End(why, true);
    }
}

void* Forget() noexcept
{
    void* chocobo = g_phase != Phase::Idle ? g_ride.chocobo : nullptr;
    if (chocobo != nullptr)
    {
        platform::Log(L"chocobo ride: the member was destroyed while player 2 rode; the ride is forgotten");
        g_stopped = true;  // player 2's coordinator is left as the game left it: no ride is begun again this session
    }
    g_phase    = Phase::Idle;
    g_ride     = {};
    g_heldBack = nullptr;
    return chocobo;
}

}  // namespace lss::player_two::chocobo_ride
