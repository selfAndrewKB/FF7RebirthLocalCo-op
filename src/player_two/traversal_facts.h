#pragma once

// The traversal cameras' facts and tables (player_two/traversal_camera.h): the executer classes (ladder, squeeze,
// crawl, pole slide, lifting rope, one-shot move) with the fields and exits the mod reads on them, the move classes
// (climbing wall, monkey bars, spider wall) with player 2's own holder's four game functions, the climbing wall's hip
// calls and the collision watch's reads. Internal to the traversal camera's units (traversal_exec.cpp,
// traversal_holders.cpp, traversal_collision.cpp); nothing here is a public API.
//
// 1.004 addresses; 1.005 through replaced-traversal-l3.json, replaced-traversal-cameras.json and
// replaced-traversal-classes.json (every slot, function and call site read from both exes).

#include "facts/traversal.h"
#include "player_two/world_rules.h"
#include "platform/verified_code.h"

#include <cstddef>
#include <cstdint>
#include <iterator>

namespace lss::player_two::traversal_camera {

// --- executer classes -------------------------------------------------------------------------------------------------

constexpr std::uint8_t kLadderTickBytes[]  = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                              0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x55};
constexpr std::uint8_t kCrackStartBytes[]  = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                              0x24, 0x18, 0x55, 0x57, 0x41, 0x56, 0x48, 0x8D};
constexpr std::uint8_t kCrackEndBytes[]    = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                              0x24, 0x18, 0x55, 0x57, 0x41, 0x56, 0x48, 0x8B};
constexpr std::uint8_t kCrouchStartBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C,
                                              0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0xB8, 0xD0};
constexpr std::uint8_t kCrouchEndBytes[]   = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                              0x24, 0x18, 0x55, 0x57, 0x41, 0x57, 0x48, 0x8B};
constexpr std::uint8_t kPoleStartBytes[]   = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                              0x24, 0x18, 0x55, 0x57, 0x41, 0x56, 0x48, 0x8D};
constexpr std::uint8_t kPoleEndBytes[]     = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C,
                                              0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57};
constexpr std::uint8_t kPoleTickBytes[]    = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                              0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x55};
constexpr std::uint8_t kRopeStartBytes[]   = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                              0x24, 0x18, 0x57, 0x41, 0x56, 0x41, 0x57, 0xB8};
constexpr std::uint8_t kRopeEndBytes[]     = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                              0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x55};
constexpr std::uint8_t kSharedTickBytes[]  = {0x40, 0x53, 0xB8, 0x30, 0x00, 0x00, 0x00};  // then E8 __chkstk
constexpr std::uint8_t kOneShotStartBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                               0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41};
constexpr std::uint8_t kOneShotEndBytes[]   = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                               0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x55};
constexpr std::uint8_t kCrackTickBytes[]   = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                              0x24, 0x18, 0x55, 0x57, 0x41, 0x56, 0x48, 0x8B};
constexpr std::uint8_t kCrouchTickBytes[]  = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                              0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x55};

enum class TickRule : std::uint8_t
{
    None,        // the Tick has no camera call: not replaced
    LadderExit,  // only the climb move's exit phases (3, 6, 10) switch the camera off
    Whole,       // the camera goes on or off inside the Tick: every frame of player 2's executer
    MoveExit,    // the move's exit (state 3) switches the camera off from a worker delegate: replayed once, scoped
};

// What player 2's executer runs at its move's exit (RE-p2-traversal-worker-gates.md W2, W3): the move fires an exit
// delegate on the anim worker whose camera off sits behind IsUCPC, so player 2's camera stayed on until the End.
enum class ExitCall : std::uint8_t
{
    None,
    SqueezeDelegate,  // the delegate body itself (executer**): its test and its camera off, on player 2's handles
    CrawlCameraOff,   // only the camera off of the actor at exec+0xB0: the body's other work already ran on the worker
};

struct ExecClass
{
    const wchar_t*     name;
    std::uintptr_t     actorVt;       // the actor class (1.004); the handle is swapped only on a matching actor
    std::uint32_t      actorAtStart;  // executer field holding the actor's FWeakObjectPtr when Start runs
    std::uint32_t      actorLater;    // the same at Tick and End (the pole's and crawl's copy at +0xB0: their +0x58 is
                                      // the state machine; RE-p2-traversal-cameras.md, pole section)
    std::uint32_t      handle;        // TWeakPtr handle(s) on the actor (object, reference controller); 0: none
    std::uint32_t      handleSize;    // 16 per handle
    bool               leaderAtStart; // the installer reads the party leader on every actor (squeezes: the side)
    bool               installInTick; // the camera goes on inside the Tick (the rail leader scope goes there)
    TickRule           tick;
    platform::SlotFact start;  // slot 0: the slot is replaced elsewhere (the ladder's, in traversal_watch)
    platform::SlotFact end;
    platform::SlotFact tickSlot;
    std::uint32_t      exitMove = 0;  // MoveExit: executer field holding the move (TSharedPtr object first)
    ExitCall           exitCall = ExitCall::None;
    std::uint32_t      cameraFlag = 0;  // no handle: the actor's u8 "camera installed" flag (logged; 0: none)
};

constexpr platform::SlotFact kNoSlot{L"", 0, {L"", 0, {}}};

inline const ExecClass kExecClasses[] = {
    {L"ladder", facts::kLadderVTable, 0x08, 0xB4, 0x760, 16, false, false, TickRule::LadderExit, kNoSlot, kNoSlot,
     {L"the ladder executer's Tick slot (+0x20)", 0x1458125E0, {L"LadderExecuter_Tick_Vt20", 0x14132F658, kLadderTickBytes}}},
    {L"squeeze", facts::kSqueezeVTable, 0x58, 0x58, 0x808, 32, true, false, TickRule::MoveExit,
     {L"the squeeze executer's Start slot", 0x145812470, {L"CrackExecuter_Start_Vt10", 0x141413D04, kCrackStartBytes}},
     {L"the squeeze executer's End slot", 0x145812478, {L"CrackExecuter_End_Vt18", 0x141413280, kCrackEndBytes}},
     {L"the squeeze executer's Tick slot", 0x145812480, {L"CrackExecuter_Tick_Vt20", 0x141330054, kCrackTickBytes}},
     0x80, ExitCall::SqueezeDelegate},
    {L"crawl", facts::kCrawlVTable, 0x08, 0xB0, 0, 0, false, false, TickRule::MoveExit,
     {L"the crawl executer's Start slot", 0x145812530, {L"CrouchedExecuter_Start_Vt10", 0x14151F3DC, kCrouchStartBytes}},
     {L"the crawl executer's End slot", 0x145812538, {L"CrouchedExecuter_End_Vt18", 0x14151F04C, kCrouchEndBytes}},
     {L"the crawl executer's Tick slot", 0x145812540, {L"CrouchedExecuter_Tick_Vt20", 0x141331314, kCrouchTickBytes}},
     0xB8, ExitCall::CrawlCameraOff},
    {L"pole slide", facts::kPoleSlideVTable, 0x08, 0xB0, 0x760, 16, false, false, TickRule::Whole,
     {L"the pole executer's Start slot", 0x1466ED7C0, {L"PoleSliderExecuter_Start_Vt10", 0x143635638, kPoleStartBytes}},
     {L"the pole executer's End slot", 0x1466ED7C8, {L"PoleSliderExecuter_End_Vt18", 0x143638D00, kPoleEndBytes}},
     {L"the pole executer's Tick slot", 0x1466ED7D0, {L"PoleSliderExecuter_Tick_Vt20", 0x143647374, kPoleTickBytes}}},
    {L"lifting rope", facts::kLiftingRopeVTable, 0x58, 0x58, 0x7E8, 16, false, true, TickRule::Whole,
     {L"the rope executer's Start slot", 0x1466FF520, {L"LiftingRopeExecuter_Start_Vt10", 0x14366B55C, kRopeStartBytes}},
     {L"the rope executer's End slot", 0x1466FF528, {L"LiftingRopeExecuter_End_Vt18", 0x14366E374, kRopeEndBytes}},
     {L"the rope executer's Tick slot", 0x1466FF530, {L"FieldActionExecuter_Tick_Vt20_Shared", 0x1436827D0, kSharedTickBytes}}},
    // Every one-shot type (squeeze, crouch, door steps, point to point) is this one class (type at actor+0x710); the
    // camera (squeeze and crouch only) goes on in the Tick's phase update behind IsUCPC_Core, with no handle: the
    // actor's flag +0x88C records it. Start runs unscoped for the leader: its only leader read records a non-leader's
    // safe spot for the save file (RE-p2-traversal-cameras.md §11), and to the save player 2's member is a party member.
    {L"one-shot move", facts::kOneShotMoveVTable, 0x58, 0x58, 0, 0, false, true, TickRule::Whole,
     {L"the one-shot executer's Start slot", 0x146706C50, {L"OneShotMoveExecuter_Start_Vt10", 0x143693618, kOneShotStartBytes}},
     {L"the one-shot executer's End slot", 0x146706C58, {L"OneShotMoveExecuter_End_Vt18", 0x143695630, kOneShotEndBytes}},
     {L"the one-shot executer's Tick slot", 0x146706C60, {L"FieldActionExecuter_Tick_Vt20_Shared", 0x1436827D0, kSharedTickBytes}},
     0, ExitCall::None, 0x88C},
};
constexpr std::size_t kExecCount = std::size(kExecClasses);
constexpr std::size_t kLadder    = 0;
constexpr std::size_t kOneShot   = 5;

// The one-shot's camera off (RE-p2-traversal-cameras.md §10): the actor's leave (vt+0x8F8; 1.005 +0x900) tests and
// deactivates slot 15 of the camera manager GetEndPlayerCameraManager answers, then clears +0x88C. The action's end
// delegate reaches it with the leaving character (exec+0x10): on the anim worker at a normal finish, on the game thread
// from the executer End's abort; EndPlay calls it with no character. For player 2's member (or, with no character, the
// actor player 2's camera went on) the game's own leave runs inside climb_context's camera context, where the getter
// answers player 2's camera manager on that thread. Rail one-shots switch op 7 off before the slot (not covered; logged).
constexpr std::uint8_t kOneShotLeaveBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00};  // then E8 __chkstk
inline const platform::SlotFact kOneShotLeaveSlot{L"the one-shot actor's leave slot (+0x8F8)", 0x14580F7F8,
                                           {L"OneShotMove_Vt8F8_PrimaryPCM_DeactSlot15_If88C", 0x1436A9130,
                                            kOneShotLeaveBytes}};

using TickFn = std::uintptr_t (*)(void* executer, double dt, void* third, std::int64_t fourth);  // xmm1, r8, r9 passed on

constexpr std::uint32_t kExecCharacter     = 0x10;   // FWeakObjectPtr, every executer
constexpr std::uint32_t kExecFactoryActor  = 0x08;   // FWeakObjectPtr set before Start (the fallback)
constexpr std::uint32_t kLadderClimbMove   = 0xC0;   // ladder executer: the climb move (TSharedPtr object)
constexpr std::uint32_t kClimbMoveState    = 0x4E8;  // u8: 3, 6 and 10 are the exit phases
constexpr std::uint32_t kActorRail         = 0x501;  // bUseRailCamera: rail operator 7, offset from the leader
constexpr std::uint32_t kModuleActive      = 0x44;   // FEndCameraModule: active
constexpr std::uint32_t kLadderSlide       = 0x434;  // FEndLadderCameraModule: sliding (a wider FOV), set by UpdateInput
constexpr std::size_t   kMaxHandle         = 32;

// The ladder's smooth exit (RE-p2-traversal-ladder-exit.md §2, §6): the climb move's state delegate (anim worker) asks,
// through IsUCPC_Core and a global count, for the ladder module's exit state S2, a 1 s blend into the follow camera
// after which the module switches itself off; the executer's camera off then finds S2 and does nothing. Off the game
// thread the member's answer is the engine's no, and the ladder holds player 1's handle, so player 2 had a cut instead.
// The mod calls the same delegate body for player 2's executer at its first exit Tick, with player 2's handle in and the
// scopes open; that Tick runs unscoped (the queued task must set S2 before a camera off), later ones scoped once S2
// shows, or after kExitWait Ticks (the game's gate refused: player 1 would get the cut then too).
constexpr std::uint8_t kRequestCamExitBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                                 0x24, 0x18, 0x57, 0xB8, 0x70, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kRequestCamExit{L"LadderExecuter_OnClimbStateChanged_RequestCamExit", 0x1422AB010,
                                                 kRequestCamExitBytes};
using RequestFn = std::uint8_t (*)(void* const* capture, std::uint8_t state);  // capture: a slot holding the executer

constexpr std::uint32_t kModuleStateCurrent = 0x498;  // the module's sub-state machine: entry pointers, id first
constexpr std::uint32_t kModuleStatePending = 0x4A0;
constexpr std::int32_t  kLadderExitBlend    = 2;      // S2
constexpr unsigned      kExitWait           = 4;      // Ticks without S2 before the scoped Tick's camera off runs

// The squeeze's and crawl's exits (RE-p2-traversal-worker-gates.md W2, W3): the move's state handler sets +0x4E0 = 3 near
// the far end and fires the executer's exit delegate (anim worker), which switches the camera off behind IsUCPC. For
// player 2 the mod runs the same off once, at the first Tick that sees state 3, in player 2's scopes.
constexpr std::uint8_t kSqueezeExitBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kSqueezeExit{L"CrackExecuter_ExitDelegate_UCPC_CameraOff", 0x1422A92E0,
                                              kSqueezeExitBytes};
constexpr std::uint8_t kCameraOffBytes[] = {0x40, 0x53, 0xB8, 0x70, 0x00, 0x00, 0x00};  // then E8 __chkstk
constexpr platform::FunctionFact kCameraOff{L"StateManagedActor_CameraOff_RailOp7OrVt8F8", 0x14132EB5C, kCameraOffBytes};
using OneArgFn = std::uintptr_t (*)(void* arg);
constexpr std::uint32_t kMoveExitState = 0x4E0;  // u8 on the squeeze / crawl move
constexpr std::uint8_t  kMoveAtExit    = 3;
constexpr std::uint32_t kCrawlExitActor = 0xB0;  // FWeakObjectPtr: the actor the crawl's exit switches off

/// The climb move's exit phases: 3, 6, 10 switch the camera off in the Tick; 11, 12 only ask for the exit blend.
inline bool ExitState(std::uint8_t state) noexcept
{
    return world_rules::ClimbExitState(state);
}

// The climbing wall's hip framing (RE-p2-traversal-camera-collision.md §3, §6): entering phase 3 (on the wall, between
// holds) sets the holder's module +0x484, which moves the camera's look-at, the start of the collision sweep, onto the
// hip raised by CameraDefaultSweepOffsetZ; leaving it clears the flag. Both are behind IsUCPC_Core(character), on the
// anim worker, where player 2's member is the engine's no: player 2 never had the hip framing. The two calls answer yes
// for player 2's member while player 2's Begin holds the wall's holder.
constexpr std::uintptr_t kHipCalls[]     = {0x14141C056, 0x14141B946};  // enter, exit (`mov rcx, rdi; call IsUCPC_Core`)
constexpr const wchar_t* kHipCallNames[] = {L"enters", L"leaves"};
using CoreFn = std::uint8_t (*)(const void* character);

constexpr std::uintptr_t kClimbingModuleVt = 0x145854970;  // FEndClimbingCameraModule
constexpr std::uint32_t  kClimbHip         = 0x484;        // u8: hip framing wanted
constexpr std::uint32_t  kClimbHipBlend    = 0x488;        // float: eases toward it at 5/s

// The camera manager's collision sweep result (§1.1): "pulled in this frame", and the hit actor's weak reference.
constexpr std::uint32_t kPcmPulledIn = 0x2DE4;
constexpr std::uint32_t kPcmHitActor = 0x2DC0;
constexpr unsigned      kAfterFrames = 60;  // frames watched after a traversal module leaves slot 15 (the ladder top)
constexpr std::uint32_t kSlotFifteen = 15;  // the ride camera module's slot (player_two/camera_access.h)

// --- move classes ---------------------------------------------------------------------------------------------------

constexpr std::uint8_t kClimbBeginBytes[]  = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                              0x24, 0x18, 0x55, 0x57, 0x41, 0x54, 0x41, 0x56};
constexpr std::uint8_t kBarsBeginBytes[]   = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                              0x24, 0x18, 0x55, 0x57, 0x41, 0x56, 0x48, 0x8D};
constexpr std::uint8_t kSpiderBeginBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48,
                                              0x89, 0x70, 0x10, 0x48, 0x89, 0x78, 0x18, 0x4C};
constexpr std::uint8_t kMoveGateBytes[]    = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00};  // then E8 __chkstk
constexpr platform::FunctionFact kMoveGate{L"FieldActionMove_IsPlayerChar_UCPCOrLeaderTypeTag", 0x141634C30, kMoveGateBytes};

struct MoveClass
{
    const wchar_t*     name;
    std::uintptr_t     actorVt;
    platform::SlotFact begin;     // the move's Begin (+0x98): the camera goes on here
    std::uintptr_t     gateCall;  // the per-frame rotation's call of the move gate (0: none, the rotation is ungated)
};

inline const MoveClass kMoveClasses[] = {
    {L"climbing wall", facts::kClimbingWallVTable,
     {L"the climbing move's Begin slot", 0x145833800, {L"ClimbingMove_Begin_Vt98", 0x1414182B0, kClimbBeginBytes}}, 0},
    {L"monkey bars", facts::kMonkeyBarsVTable,
     {L"the monkey-bars move's Begin slot", 0x146695AA0, {L"MonkeyBarsMove_Begin_Vt98", 0x1432F0790, kBarsBeginBytes}},
     0x1432FAC10},
    {L"spider wall", facts::kSpiderMoveVTable,
     {L"the spider move's Begin slot", 0x146696D80, {L"SpiderMoveMove_Begin_Vt98", 0x143304A90, kSpiderBeginBytes}},
     0x143312F31},
};
constexpr std::size_t kMoveCount = std::size(kMoveClasses);
constexpr std::size_t kClimbingWall = 0;

// The holders' check (RE-p2-traversal-worker-gates.md W1): every field-actor manager tick (game thread, outside the mod's
// scopes) the actor's vt+0x790 (1.005 +0x798) keeps the holder's camera only if its installer is a user of the actor
// AND has the party leader's type (PM+0x150); else it leaves, one tick after player 2's install. For a holder player
// 2's member installed, the check runs with the leader type answering the member's. Same order as kMoveClasses.
constexpr std::uint8_t kValidateBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                           0x24, 0x10, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kValidateFn{L"StateManagedActor_Vt790_CanDeactivate_ValidatesHolder", 0x140E5D070,
                                             kValidateBytes};
inline const platform::SlotFact kValidateSlots[] = {
    {L"the climbing wall's holder check slot (+0x790)", 0x145833378, kValidateFn},
    {L"the monkey bars' holder check slot (+0x790)", 0x1466EF7E0, kValidateFn},
    {L"the spider wall's holder check slot (+0x790)", 0x146709D88, kValidateFn},
};
static_assert(std::size(kValidateSlots) == kMoveCount);
using ValidateFn = std::uintptr_t (*)(void* actor);

using BeginFn = std::uintptr_t (*)(void* move);
using GateFn  = std::uint8_t (*)(const void* move);

constexpr std::uint32_t kMoveCharacter = 0x450;  // FWeakObjectPtr
constexpr std::uint32_t kMoveActor     = 0x458;  // FWeakObjectPtr
constexpr std::uint32_t kHolder        = 0x6D0;  // camera-module holder: u8 active, then the installer's weak at +0x4
constexpr std::uint32_t kHolderOwner   = 0x6D4;

// Player 2's own holder per actor (RE-p2-traversal-climb-holder.md): the actor's holder records one installer, and a
// second install evicts the first (player 1 getting on a wall player 2 climbs switched player 2's camera off). Player 2
// gets a holder of the same layout, owned here, which the game's own four holder functions run on for player 2's calls:
// the install (player 2's Begin), the module getter (player 2's Begin and player 2's climb Tick, climb_context), the
// leave (keyed by the leaving character) and the validate. The actor's holder is never written for player 2.
constexpr std::uint32_t kHolderSize    = 0x28;
constexpr std::uint32_t kHolderActorAt = 0x20;  // the owning actor (the install registers it with the manager)
constexpr std::size_t   kTwoHolders    = 4;

constexpr std::uint8_t kInstallBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10};
// CameraModuleHolder_InstallSharedModule
constexpr platform::FunctionFact kInstall{L"CameraModuleHolder_InstallSharedModule", 0x141683E04, kInstallBytes};
constexpr std::size_t  kInstallFirst   = 5;  // mov [rsp+8], rbx (a home-area store), then mov [rsp+10h], rsi
constexpr std::size_t  kInstallSecond  = 5;
constexpr std::uint8_t kGetModuleBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0xB8, 0x30, 0x00, 0x00, 0x00};
// CameraModuleHolder_GetModule
constexpr platform::FunctionFact kGetModule{L"CameraModuleHolder_GetModule", 0x141634DA8, kGetModuleBytes};
constexpr std::size_t    kGetModuleFirst = 5;
constexpr std::uint8_t kHolderLeaveBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18};
inline const platform::FunctionFact kHolderLeave{L"CameraModuleHolder_Leave_IfOwnerChar", 0x14128E8EC,
                                                 kHolderLeaveBytes};
constexpr std::uint8_t kHolderValidateBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57};
inline const platform::FunctionFact kHolderValidate{L"CameraModuleHolder_Validate_OwnerIsLeaderTypeUser_ElseLeave",
                                                    0x140E5D17C, kHolderValidateBytes};
constexpr std::uint8_t kLeaveSlotBytes[]     = {0xB8, 0x28, 0x00, 0x00, 0x00};  // then E8 __chkstk
inline const platform::FunctionFact kLeaveSlotFn{L"StateManagedActor_Vt8F8_HolderLeave_6D0", 0x14132DA18,
                                                 kLeaveSlotBytes};
constexpr std::uint8_t kClimbEndPlayBytes[]  = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr std::uint8_t kEndPlayThunkBytes[]  = {0xB8, 0x28, 0x00, 0x00, 0x00};
inline const platform::FunctionFact kClimbEndPlay{L"Climbing_Vt340_EndPlay", 0x14158AAD8, kClimbEndPlayBytes};
inline const platform::FunctionFact kEndPlayThunk{L"StateManagedActor_Vt340_EndPlay_Thunk", 0x1418D1B14,
                                                  kEndPlayThunkBytes};
// Same order as kMoveClasses; 1.005 slots are +8 (+0x900, +0x348).
inline const platform::SlotFact kLeaveSlots[] = {
    {L"the climbing wall's holder leave slot (+0x8F8)", 0x1458334E0, kLeaveSlotFn},
    {L"the monkey bars' holder leave slot (+0x8F8)", 0x1466EF948, kLeaveSlotFn},
    {L"the spider wall's holder leave slot (+0x8F8)", 0x146709EF0, kLeaveSlotFn},
};
inline const platform::SlotFact kEndPlaySlots[] = {
    {L"the climbing wall's EndPlay slot (+0x340)", 0x145832F28, kClimbEndPlay},
    {L"the monkey bars' EndPlay slot (+0x340)", 0x1466EF390, kEndPlayThunk},
    {L"the spider wall's EndPlay slot (+0x340)", 0x146709938, kEndPlayThunk},
};
static_assert(std::size(kLeaveSlots) == kMoveCount && std::size(kEndPlaySlots) == kMoveCount);
using InstallFn        = std::uintptr_t (*)(void* holder, void* module, void* args);
using GetModuleFn      = void* (*)(void* holder);
using HolderLeaveFn    = void (*)(void* holder, const void* character, std::uint8_t force);
using HolderValidateFn = void (*)(void* holder);
using LeaveSlotFn      = std::uintptr_t (*)(void* actor, void* character);
using EndPlayFn        = std::uintptr_t (*)(void* actor, std::uintptr_t reason);

}  // namespace lss::player_two::traversal_camera
