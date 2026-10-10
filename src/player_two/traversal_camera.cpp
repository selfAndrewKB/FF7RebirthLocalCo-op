#include "player_two/traversal_camera.h"

#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "player_two/climb_context.h"
#include "player_two/describe.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/member_movement.h"
#include "player_two/object_fields.h"
#include "player_two/party_member.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>

namespace lss::player_two::traversal_camera {

namespace {

// 1.004 addresses; 1.005 through replaced-traversal-l3.json, replaced-traversal-cameras.json and
// replaced-traversal-classes.json (every slot, function and call site read from both exes).

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

const ExecClass kExecClasses[] = {
    {L"ladder", 0x145940748, 0x08, 0xB4, 0x760, 16, false, false, TickRule::LadderExit, kNoSlot, kNoSlot,
     {L"the ladder executer's Tick slot (+0x20)", 0x1458125E0, {L"LadderExecuter_Tick_Vt20", 0x14132F658, kLadderTickBytes}}},
    {L"squeeze", 0x1466E8B68, 0x58, 0x58, 0x808, 32, true, false, TickRule::MoveExit,
     {L"the squeeze executer's Start slot", 0x145812470, {L"CrackExecuter_Start_Vt10", 0x141413D04, kCrackStartBytes}},
     {L"the squeeze executer's End slot", 0x145812478, {L"CrackExecuter_End_Vt18", 0x141413280, kCrackEndBytes}},
     {L"the squeeze executer's Tick slot", 0x145812480, {L"CrackExecuter_Tick_Vt20", 0x141330054, kCrackTickBytes}},
     0x80, ExitCall::SqueezeDelegate},
    {L"crawl", 0x1457D8960, 0x08, 0xB0, 0, 0, false, false, TickRule::MoveExit,
     {L"the crawl executer's Start slot", 0x145812530, {L"CrouchedExecuter_Start_Vt10", 0x14151F3DC, kCrouchStartBytes}},
     {L"the crawl executer's End slot", 0x145812538, {L"CrouchedExecuter_End_Vt18", 0x14151F04C, kCrouchEndBytes}},
     {L"the crawl executer's Tick slot", 0x145812540, {L"CrouchedExecuter_Tick_Vt20", 0x141331314, kCrouchTickBytes}},
     0xB8, ExitCall::CrawlCameraOff},
    {L"pole slide", 0x1458B41F8, 0x08, 0xB0, 0x760, 16, false, false, TickRule::Whole,
     {L"the pole executer's Start slot", 0x1466ED7C0, {L"PoleSliderExecuter_Start_Vt10", 0x143635638, kPoleStartBytes}},
     {L"the pole executer's End slot", 0x1466ED7C8, {L"PoleSliderExecuter_End_Vt18", 0x143638D00, kPoleEndBytes}},
     {L"the pole executer's Tick slot", 0x1466ED7D0, {L"PoleSliderExecuter_Tick_Vt20", 0x143647374, kPoleTickBytes}}},
    {L"lifting rope", 0x1466FE5D0, 0x58, 0x58, 0x7E8, 16, false, true, TickRule::Whole,
     {L"the rope executer's Start slot", 0x1466FF520, {L"LiftingRopeExecuter_Start_Vt10", 0x14366B55C, kRopeStartBytes}},
     {L"the rope executer's End slot", 0x1466FF528, {L"LiftingRopeExecuter_End_Vt18", 0x14366E374, kRopeEndBytes}},
     {L"the rope executer's Tick slot", 0x1466FF530, {L"FieldActionExecuter_Tick_Vt20_Shared", 0x1436827D0, kSharedTickBytes}}},
    // Every one-shot type (squeeze, crouch, door steps, point to point) is this one class (type at actor+0x710); the
    // camera (squeeze and crouch only) goes on in the Tick's phase update behind IsUCPC_Core, with no handle: the
    // actor's flag +0x88C records it. Start runs unscoped for the leader: its only leader read records a non-leader's
    // safe spot for the save file (RE-p2-traversal-cameras.md §11), and to the save player 2's member is a party member.
    {L"one-shot move", 0x14580EF00, 0x58, 0x58, 0, 0, false, true, TickRule::Whole,
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
const platform::SlotFact kOneShotLeaveSlot{L"the one-shot actor's leave slot (+0x8F8)", 0x14580F7F8,
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
bool ExitState(std::uint8_t state) noexcept
{
    return state == 3 || state == 6 || state == 10 || state == 11 || state == 12;
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
constexpr std::uint32_t kOpModules   = 0x6E0;    // the Field operator's module table, 16 bytes a slot
constexpr std::uint32_t kSlotFifteen = 15 * 16;

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

const MoveClass kMoveClasses[] = {
    {L"climbing wall", 0x145832BE8,
     {L"the climbing move's Begin slot", 0x145833800, {L"ClimbingMove_Begin_Vt98", 0x1414182B0, kClimbBeginBytes}}, 0},
    {L"monkey bars", 0x1466EF050,
     {L"the monkey-bars move's Begin slot", 0x146695AA0, {L"MonkeyBarsMove_Begin_Vt98", 0x1432F0790, kBarsBeginBytes}},
     0x1432FAC10},
    {L"spider wall", 0x1467095F8,
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
const platform::SlotFact kValidateSlots[] = {
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
constexpr std::size_t  kInstallFirst   = 5;  // mov [rsp+8], rbx (a home-area store), then mov [rsp+10h], rsi
constexpr std::size_t  kInstallSecond  = 5;
constexpr std::uintptr_t kInstall      = 0x141683E04;  // CameraModuleHolder_InstallSharedModule
constexpr std::uint8_t kGetModuleBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0xB8, 0x30, 0x00, 0x00, 0x00};
constexpr std::uintptr_t kGetModule      = 0x141634DA8;  // CameraModuleHolder_GetModule
constexpr std::size_t    kGetModuleFirst = 5;
constexpr std::uint8_t kHolderLeaveBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18};
const platform::FunctionFact kHolderLeave{L"CameraModuleHolder_Leave_IfOwnerChar", 0x14128E8EC, kHolderLeaveBytes};
constexpr std::uint8_t kHolderValidateBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57};
const platform::FunctionFact kHolderValidate{L"CameraModuleHolder_Validate_OwnerIsLeaderTypeUser_ElseLeave", 0x140E5D17C,
                                             kHolderValidateBytes};
constexpr std::uint8_t kLeaveSlotBytes[]     = {0xB8, 0x28, 0x00, 0x00, 0x00};  // then E8 __chkstk
const platform::FunctionFact kLeaveSlotFn{L"StateManagedActor_Vt8F8_HolderLeave_6D0", 0x14132DA18, kLeaveSlotBytes};
constexpr std::uint8_t kClimbEndPlayBytes[]  = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr std::uint8_t kEndPlayThunkBytes[]  = {0xB8, 0x28, 0x00, 0x00, 0x00};
const platform::FunctionFact kClimbEndPlay{L"Climbing_Vt340_EndPlay", 0x14158AAD8, kClimbEndPlayBytes};
const platform::FunctionFact kEndPlayThunk{L"StateManagedActor_Vt340_EndPlay_Thunk", 0x1418D1B14, kEndPlayThunkBytes};
// Same order as kMoveClasses; 1.005 slots are +8 (+0x900, +0x348).
const platform::SlotFact kLeaveSlots[] = {
    {L"the climbing wall's holder leave slot (+0x8F8)", 0x1458334E0, kLeaveSlotFn},
    {L"the monkey bars' holder leave slot (+0x8F8)", 0x1466EF948, kLeaveSlotFn},
    {L"the spider wall's holder leave slot (+0x8F8)", 0x146709EF0, kLeaveSlotFn},
};
const platform::SlotFact kEndPlaySlots[] = {
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

// --- state ------------------------------------------------------------------------------------------------------------

struct ExecLive
{
    std::uintptr_t        actorVt = 0;
    platform::VTableEntry start, end, tick;
    bool                  startPlaced = false, endPlaced = false, tickPlaced = false;
};
std::array<ExecLive, kExecCount> g_exec;

struct MoveLive
{
    std::uintptr_t         actorVt = 0;
    platform::VTableEntry  begin;
    platform::CallRedirect gate;
    platform::VTableEntry  validate;
    platform::VTableEntry  leave;    // player 2's own holder: the leave and EndPlay slots
    platform::VTableEntry  endPlay;
    bool                   beginPlaced    = false;
    bool                   validatePlaced = false;
    bool                   leavePlaced    = false;
    bool                   endPlayPlaced  = false;
    const void*            seenActor      = nullptr;  // the validate's last logged outcome (game thread)
    bool                   seenKept       = false;
};
std::array<MoveLive, kMoveCount> g_move;

// Player 2's holders. A slot is claimed and released on the game thread; `actor` is published last (release) and read
// on any thread (acquire), the holder's own bytes are the game's to read and write.
struct TwoHolder
{
    alignas(16) std::byte    bytes[kHolderSize]{};
    std::atomic<const void*> actor{nullptr};
    bool                     loggedActive = false;  // game thread: the last logged state
};
std::array<TwoHolder, kTwoHolders> g_twoHolders;
platform::EntryRedirect            g_installEntry;
platform::EntryRedirect            g_getModuleEntry;
HolderLeaveFn                      g_holderLeave    = nullptr;
HolderValidateFn                   g_holderValidate = nullptr;
bool                               g_twoHoldersOn   = false;  // every hook placed (set at install)
std::atomic<const void*>           g_memberNow{nullptr};      // player 2's member (game thread, per tick)
std::atomic<std::uint64_t>         g_memberWeakNow{0};
thread_local bool                  t_twoBegin = false;        // inside player 2's move Begin (game thread)

/// Holds t_twoBegin for one Begin.
struct TwoBegin
{
    TwoBegin() noexcept { t_twoBegin = true; }
    ~TwoBegin() { t_twoBegin = false; }
    TwoBegin(const TwoBegin&)            = delete;
    TwoBegin& operator=(const TwoBegin&) = delete;
};

bool g_inside = false;  // inside a scoped call of player 2's (game thread)

/// Holds g_inside for one scoped call; reset on every way out.
struct Inside
{
    Inside() noexcept { g_inside = true; }
    ~Inside() { g_inside = false; }
    Inside(const Inside&)            = delete;
    Inside& operator=(const Inside&) = delete;
};

// Player 2's camera handles per actor (by the actor's weak value), moved in and out of the actor, never copied, so the
// reference counts stay the game's. An entry whose actor is gone keeps its weak reference (one small block); past
// kHandlePrune entries those are dropped from the map.
using HandleBytes = std::array<std::uint64_t, kMaxHandle / 8>;
constexpr std::size_t                          kHandlePrune = 32;
std::unordered_map<std::uint64_t, HandleBytes> g_handles;
const void*                                    g_exitLogged = nullptr;

// Player 2's move whose holder camera player 2 installed, for the per-frame rotation gate (read on the anim worker).
std::atomic<const void*>   g_playerTwoMove{nullptr};
std::atomic<std::uint64_t> g_playerTwoMemberWeak{0};

// Player 2's ladder exit (game thread): the executer whose exit blend was asked for, and the Ticks waited since.
struct LadderExit
{
    const void* executer  = nullptr;
    bool        requested = false;
    bool        scoped    = false;  // the exit's camera off ran in player 2's scopes (logged once)
    unsigned    waited    = 0;
};
LadderExit g_ladderExit;
RequestFn  g_requestCamExit = nullptr;

// The one-shot actor's leave slot, and the actor player 2's one-shot camera went on, as a weak value (game thread writes;
// the leave resolves it on any thread, so a freed actor whose memory is reused never matches).
platform::VTableEntry      g_oneShotLeave;
bool                       g_oneShotLeavePlaced = false;
std::atomic<std::uint64_t> g_oneShotTwoWeak{0};

// The squeeze's and crawl's exit calls, and the executer whose exit ran (per class; reset at Start).
OneArgFn                               g_squeezeExit = nullptr;
OneArgFn                               g_cameraOff   = nullptr;
std::array<const void*, kExecCount>    g_exitRan{};

// The climbing wall player 2's Begin installed its camera on, and player 2's member then (written on the game thread,
// read by the hip calls on the anim worker).
std::array<platform::CallRedirect, std::size(kHipCalls)> g_hipCalls;
std::atomic<const void*>                                g_climbActor{nullptr};
std::atomic<const void*>                                g_climbMember{nullptr};

std::optional<platform::GameImage> g_image;
std::uintptr_t                     g_climbingModuleVt = 0;        // live
const void*                        g_ladderModule     = nullptr;  // player 2's last ladder Start's module (watch only)

void* ObjectIn(const void* object, std::uint32_t field) noexcept
{
    return object != nullptr ? leader_scope::ObjectAt(static_cast<const std::byte*>(object) + field) : nullptr;
}

/// The executer's character is player 2's member (game thread).
bool PlayerTwos(void* executer) noexcept
{
    void* member = generation::Member();
    return member != nullptr && generation::Active() && generation::OnGameThread() &&
           ObjectIn(executer, kExecCharacter) == member;
}

HandleBytes ReadHandle(const void* actor, const ExecClass& cls) noexcept
{
    HandleBytes bytes{};
    for (std::uint32_t i = 0; i < cls.handleSize / 8; ++i)
    {
        bytes[i] = ReadField<std::uint64_t>(actor, cls.handle + 8 * i);
    }
    return bytes;
}

void WriteHandle(void* actor, const ExecClass& cls, const HandleBytes& bytes) noexcept
{
    for (std::uint32_t i = 0; i < cls.handleSize / 8; ++i)
    {
        WriteField<std::uint64_t>(actor, cls.handle + 8 * i, bytes[i]);
    }
}

/// "active", "inactive", "gone" or "none" for the first module of a handle. The weak reference keeps the reference
/// controller alive; the module only while its shared count (controller +0x8) is above 0, as the game's leaves check.
const wchar_t* ModuleText(const HandleBytes& handle) noexcept
{
    if (handle[0] == 0 || handle[1] == 0)
    {
        return L"none";
    }
    if (ReadField<std::int32_t>(reinterpret_cast<const void*>(handle[1]), 8) <= 0)
    {
        return L"gone";
    }
    return ReadField<std::uint8_t>(reinterpret_cast<const void*>(handle[0]), kModuleActive) != 0 ? L"active"
                                                                                                  : L"inactive";
}

/// The slide flag of a handle's module (player 2's own, or the ladder's), or -1 when it has no live module.
int SlideFlag(const HandleBytes& handle) noexcept
{
    const std::wstring_view state(ModuleText(handle));
    return state == L"none" || state == L"gone"
               ? -1
               : ReadField<std::uint8_t>(reinterpret_cast<const void*>(handle[0]), kLadderSlide);
}

std::array<int, 2> g_slideSeen{-1, -1};  // game thread: each player's last logged slide flag

/// A line when a player's ladder camera's slide flag changes (W4).
void NoteSlide(std::size_t player, int flag) noexcept
{
    if (flag < 0 || flag == g_slideSeen[player])
    {
        return;
    }
    g_slideSeen[player] = flag;
    platform::LogDebug(L"traversal camera: player {}'s ladder camera: slide view {}", player + 1,
                       flag != 0 ? L"on (the wider slide framing)" : L"off");
}

enum class Kind : std::uint8_t
{
    Start,
    Tick,
    End,
    Request,  // the ladder's exit-blend request (the climb move's state delegate body)
    OneArg    // a one-argument game call: the executer's capture slot, or `arg`
};

struct Call
{
    Kind           kind;
    ExecFn         exec     = nullptr;
    TickFn         tick     = nullptr;
    void*          executer = nullptr;
    double         dt       = 0;
    void*          third    = nullptr;
    std::int64_t   fourth   = 0;
    std::uintptr_t result   = 0;
    RequestFn      request  = nullptr;
    std::uint8_t   state    = 0;
    OneArgFn       one      = nullptr;
    void*          arg      = nullptr;  // nullptr: the capture slot holding the executer

    std::uintptr_t Run() noexcept
    {
        switch (kind)
        {
        case Kind::Tick:
            return tick(executer, dt, third, fourth);
        case Kind::Request:
            return request(&executer, state);  // the delegate's capture: a slot holding the executer
        case Kind::OneArg:
            return one(arg != nullptr ? arg : static_cast<void*>(&executer));
        default:
            return exec(executer);
        }
    }
};

void RunCall(void* context)
{
    auto* call   = static_cast<Call*>(context);
    call->result = call->Run();
}

struct Outcome
{
    bool           scoped = false;
    bool           rail   = false;
    bool           swapped = false;
    const void*    actor  = nullptr;
    HandleBytes    after{};                          // player 2's handle
    const wchar_t* beforeText = L"(no handle)";      // its first module, read before the call
    const wchar_t* afterText  = L"(no handle)";      // the same after it
};

/// The camera text of a class without a handle: its actor's flag, or "(no handle)".
const wchar_t* FlagText(const void* actor, const ExecClass& cls) noexcept
{
    if (cls.cameraFlag == 0 || actor == nullptr)
    {
        return L"(no handle)";
    }
    return ReadField<std::uint8_t>(actor, cls.cameraFlag) != 0 ? L"on (the actor's flag)" : L"off (the actor's flag)";
}

/// Runs `call` for player 2's executer of class `index`: player 2's handle in the actor, the primary scope and the
/// leader answer, and a LeaderScope where the installer reads the leader. Unscoped (a scope refused), the game's own
/// answer stands, still with player 2's handle in place.
Outcome RunAsPlayerTwo(std::size_t index, Call& call) noexcept
{
    const ExecClass& cls = kExecClasses[index];
    Outcome          out;
    void*            member = generation::Member();
    void*            actor  = ObjectIn(call.executer, call.kind == Kind::Start ? cls.actorAtStart : cls.actorLater);
    if (actor == nullptr)
    {
        actor = ObjectIn(call.executer, kExecFactoryActor);  // Start before the copy; Tick/End once a copy is cleared
    }
    if (actor != nullptr && VTableOf(actor) != g_exec[index].actorVt)
    {
        actor = nullptr;  // not the class's actor: no swap, no rail test
    }
    out.actor             = actor;
    const auto actorWeak  = actor != nullptr ? leader_scope::WeakOf(actor) : 0;
    HandleBytes* mine     = nullptr;
    HandleBytes  theirs{};
    if (actorWeak != 0 && cls.handle != 0)
    {
        try
        {
            if (g_handles.size() >= kHandlePrune && !g_handles.contains(actorWeak))
            {
                std::erase_if(g_handles,
                              [](const auto& entry) { return leader_scope::ObjectAt(&entry.first) == nullptr; });
            }
            mine = &g_handles[actorWeak];
        }
        catch (...)
        {
            mine = nullptr;
        }
    }
    if (mine != nullptr)
    {
        out.beforeText = ModuleText(*mine);
        theirs         = ReadHandle(actor, cls);
        WriteHandle(actor, cls, *mine);
        out.swapped = true;
    }
    if (mine == nullptr)
    {
        out.beforeText = FlagText(actor, cls);
    }
    out.rail = actor != nullptr && ReadField<std::uint8_t>(actor, kActorRail) != 0;
    const bool installs = cls.installInTick ? call.kind == Kind::Tick : call.kind == Kind::Start;
    {
        const Inside                             inside;
        std::optional<leader_scope::LeaderScope> leader;
        if ((call.kind == Kind::Start && cls.leaderAtStart) || (installs && out.rail))
        {
            leader.emplace(leader_scope::WeakOf(member));
        }
        out.scoped = member_movement::InPrimaryScope(member, &RunCall, &call);
        if (!out.scoped)
        {
            call.result = call.Run();
        }
    }
    if (mine != nullptr)
    {
        if (leader_scope::StillNames(actorWeak, actor))
        {
            *mine = ReadHandle(actor, cls);
            WriteHandle(actor, cls, theirs);
        }
        else
        {
            *mine = {};  // the actor went inside the call: its memory is the game's now
        }
        out.after     = *mine;
        out.afterText = ModuleText(out.after);
    }
    else if (actor != nullptr && leader_scope::StillNames(actorWeak, actor))
    {
        out.afterText = FlagText(actor, cls);
    }
    return out;
}

std::wstring ActorName(const void* actor)
{
    return actor != nullptr ? NameOf(actor) : std::wstring(L"(no actor of the class)");
}

void LogStep(std::size_t index, const wchar_t* step, const Outcome& out)
{
    const ExecClass& cls = kExecClasses[index];
    platform::LogDebug(L"traversal camera: player 2's {} on {} {}: {}{}; player 2's {} camera {} -> {}", cls.name,
                       ActorName(out.actor), step,
                       out.scoped ? L"as player 2" : L"a scope was refused, the game's answer stands",
                       out.rail ? L" (rail camera)" : L"", cls.name,
                       out.beforeText, out.afterText);
}

std::uintptr_t ExecStep(std::size_t index, Kind kind, Call& call)
{
    const Outcome out = RunAsPlayerTwo(index, call);
    if (index == kOneShot && kind == Kind::Tick && out.scoped && out.actor != nullptr &&
        ReadField<std::uint8_t>(out.actor, kExecClasses[kOneShot].cameraFlag) != 0)
    {
        g_oneShotTwoWeak.store(leader_scope::WeakOf(const_cast<void*>(out.actor)), std::memory_order_release);  // player 2's camera is on there
    }
    try
    {
        if (kind == Kind::Start)
        {
            g_exitLogged    = nullptr;
            g_exitRan[index] = nullptr;
            if (index == kLadder)
            {
                g_ladderExit  = LadderExit{};
                g_ladderModule = reinterpret_cast<const void*>(out.after[0]);
            }
            LogStep(index, L"starts", out);
            if (index == kOneShot && out.rail)
            {
                platform::Log(L"traversal camera: player 2's one-shot move on {} uses a rail camera: its off at the end "
                              L"reaches the primary camera's rail operator (not covered yet)",
                              ActorName(out.actor));
            }
        }
        else if (kind == Kind::End)
        {
            LogStep(index,
                    kExecClasses[index].tick == TickRule::MoveExit && g_exitRan[index] != call.executer
                        ? L"ends (its move's exit was never seen at state 3: the End switched the camera off)"
                        : L"ends",
                    out);
        }
        else if (index == kLadder ? g_exitLogged != call.executer
                                  : std::wstring_view(out.beforeText) != out.afterText)
        {
            g_exitLogged = call.executer;
            LogStep(index, index == kLadder ? L"reaches its exit" : L"changes its camera in the tick", out);
        }
    }
    catch (...)
    {
    }
    return call.result;
}

template <std::size_t Index>
std::uintptr_t ExecStart(void* executer) noexcept
{
    const auto original = reinterpret_cast<ExecFn>(g_exec[Index].start.Original());
    if (g_inside || !PlayerTwos(executer))
    {
        return original(executer);
    }
    Call call{Kind::Start, original, nullptr, executer};
    return ExecStep(Index, Kind::Start, call);
}

template <std::size_t Index>
std::uintptr_t ExecEnd(void* executer) noexcept
{
    const auto original = reinterpret_cast<ExecFn>(g_exec[Index].end.Original());
    if (g_inside || !PlayerTwos(executer))
    {
        return original(executer);
    }
    Call call{Kind::End, original, nullptr, executer};
    return ExecStep(Index, Kind::End, call);
}

/// Player 2's ladder module's exit: its sub-state machine's current or pending entry is S2, or the module is off.
struct ModuleExit
{
    bool         blending = false;  // S2 current or pending
    bool         off      = false;  // no module, gone, or inactive
    std::int32_t current = -1, pending = -1;
};

ModuleExit LadderModuleExit(const void* executer) noexcept
{
    ModuleExit  out;
    void*       actor = ObjectIn(executer, kExecClasses[kLadder].actorLater);
    const auto  found = actor != nullptr && VTableOf(actor) == g_exec[kLadder].actorVt
                            ? g_handles.find(leader_scope::WeakOf(actor))
                            : g_handles.end();
    if (found == g_handles.end() || std::wstring_view(ModuleText(found->second)) != L"active")
    {
        out.off = true;
        return out;
    }
    const void* module = reinterpret_cast<const void*>(found->second[0]);
    const void* cur    = ReadPointer(module, kModuleStateCurrent);
    const void* pend   = ReadPointer(module, kModuleStatePending);
    out.current        = cur != nullptr ? ReadField<std::int32_t>(cur, 0) : -1;
    out.pending        = pend != nullptr ? ReadField<std::int32_t>(pend, 0) : -1;
    out.blending       = out.current == kLadderExitBlend || out.pending == kLadderExitBlend;
    return out;
}

/// Player 2's ladder Tick at an exit phase. Returns nothing to do (the caller runs the Tick unscoped) or runs it.
std::optional<std::uintptr_t> LadderExitTick(Call& tick, std::uint8_t state) noexcept
{
    if (g_ladderExit.executer != tick.executer)
    {
        g_ladderExit = LadderExit{tick.executer};
    }
    if (!g_ladderExit.requested && g_requestCamExit != nullptr)
    {
        g_ladderExit.requested = true;
        Call request{Kind::Request};
        request.request        = g_requestCamExit;
        request.executer       = tick.executer;
        request.state          = state;
        const Outcome    out   = RunAsPlayerTwo(kLadder, request);
        const ModuleExit after = LadderModuleExit(tick.executer);
        try
        {
            platform::LogDebug(L"traversal camera: player 2's ladder on {} reaches its exit (climb state {}): the game's "
                               L"exit blend asked for {}; module {}, sub-state {} pending {} (the blend starts on the "
                               L"game's task: this Tick runs unscoped)",
                               ActorName(out.actor), state,
                               out.scoped ? L"as player 2" : L"with a scope refused (the game's answer stands)",
                               ModuleText(out.after), after.current, after.pending);
        }
        catch (...)
        {
        }
        return std::nullopt;
    }
    if (state == 11 || state == 12)
    {
        return std::nullopt;  // these exits switch no camera in the Tick: the blend does it all
    }
    const ModuleExit now = LadderModuleExit(tick.executer);
    if (g_requestCamExit != nullptr && !now.blending && !now.off && g_ladderExit.waited < kExitWait)
    {
        ++g_ladderExit.waited;
        return std::nullopt;
    }
    if (!g_ladderExit.scoped)
    {
        g_ladderExit.scoped = true;
        try
        {
            platform::LogDebug(L"traversal camera: player 2's ladder exit's camera off runs as player 2 {}",
                               now.blending ? L"during the exit blend (it does nothing: the blend ends the camera)"
                               : now.off    ? L"with the camera already off"
                                            : L"WITHOUT the exit blend (the game's gate refused it): a cut, as player "
                                              L"1's would be");
        }
        catch (...)
        {
        }
    }
    return ExecStep(kLadder, Kind::Tick, tick);
}

/// Player 2's squeeze or crawl reaches its move's exit: the camera off the worker's delegate refused, as player 2.
void MoveExitCall(std::size_t index, void* executer) noexcept
{
    const ExecClass& cls = kExecClasses[index];
    Call             call{Kind::OneArg};
    call.executer = executer;
    if (cls.exitCall == ExitCall::SqueezeDelegate)
    {
        call.one = g_squeezeExit;  // the body takes the delegate's capture: a slot holding the executer
    }
    else
    {
        call.one = g_cameraOff;
        call.arg = ObjectIn(executer, kCrawlExitActor);
    }
    if (call.one == nullptr || (cls.exitCall == ExitCall::CrawlCameraOff && call.arg == nullptr))
    {
        try
        {
            platform::LogDebug(L"traversal camera: player 2's {} reaches its exit: {}; its camera ends at the End",
                               cls.name, call.one == nullptr ? L"the exit call is unavailable" : L"no actor at the exit");
        }
        catch (...)
        {
        }
        return;
    }
    const Outcome out = RunAsPlayerTwo(index, call);
    try
    {
        platform::LogDebug(L"traversal camera: player 2's {} on {} reaches its exit: the game's exit camera off ran {}; "
                           L"player 2's {} camera {} -> {}",
                           cls.name, ActorName(out.actor),
                           out.scoped ? L"as player 2" : L"with a scope refused (the game's answer stands)", cls.name,
                           out.beforeText, out.afterText);
    }
    catch (...)
    {
    }
}

template <std::size_t Index>
std::uintptr_t ExecTick(void* executer, double dt, void* third, std::int64_t fourth) noexcept
{
    const auto original = reinterpret_cast<TickFn>(g_exec[Index].tick.Original());
    if (g_inside || !PlayerTwos(executer))
    {
        return original(executer, dt, third, fourth);
    }
    if (kExecClasses[Index].tick == TickRule::MoveExit)
    {
        const void* move = ReadPointer(executer, kExecClasses[Index].exitMove);
        if (g_exitRan[Index] != executer && move != nullptr &&
            ReadField<std::uint8_t>(move, kMoveExitState) == kMoveAtExit)
        {
            g_exitRan[Index] = executer;
            MoveExitCall(Index, executer);
        }
        return original(executer, dt, third, fourth);
    }
    Call call{Kind::Tick, nullptr, original, executer, dt, third, fourth};
    if (kExecClasses[Index].tick == TickRule::LadderExit)
    {
        const void* move  = ReadPointer(executer, kLadderClimbMove);
        const auto  state = move != nullptr ? ReadField<std::uint8_t>(move, kClimbMoveState) : std::uint8_t{0};
        if (!ExitState(state))
        {
            return original(executer, dt, third, fourth);
        }
        const auto ran = LadderExitTick(call, state);
        return ran ? *ran : original(executer, dt, third, fourth);
    }
    return ExecStep(Index, Kind::Tick, call);
}

// --- moves ------------------------------------------------------------------------------------------------------------

struct BeginCall
{
    BeginFn        begin;
    void*          move;
    std::uintptr_t result = 0;
};

void RunBegin(void* context)
{
    auto* call   = static_cast<BeginCall*>(context);
    call->result = call->begin(call->move);
}

bool HeldBy(const void* actor, std::uint64_t weak) noexcept
{
    return actor != nullptr && weak != 0 && ReadField<std::uint8_t>(actor, kHolder) != 0 &&
           ReadField<std::uint64_t>(actor, kHolderOwner) == weak;
}

/// Player 2's holder for `actor`, or null. Any thread.
std::byte* TwoHolderOf(const void* actor) noexcept
{
    if (actor == nullptr)
    {
        return nullptr;
    }
    for (TwoHolder& two : g_twoHolders)
    {
        if (two.actor.load(std::memory_order_acquire) == actor)
        {
            return two.bytes;
        }
    }
    return nullptr;
}

/// A holder's active byte (+0x0).
bool HolderActive(const std::byte* holder) noexcept
{
    return holder != nullptr && std::to_integer<std::uint8_t>(holder[0]) != 0;
}

/// A holder's active byte and installer weak (a holder is read plainly, as the game reads it).
bool HolderNames(const std::byte* holder, std::uint64_t weak) noexcept
{
    if (weak == 0 || !HolderActive(holder))
    {
        return false;
    }
    std::uint64_t owner = 0;
    std::memcpy(&owner, holder + 4, sizeof(owner));
    return owner == weak;
}

/// Player 2's member installed a camera on `actor`: on player 2's own holder, or on the actor's (the fallback).
bool HeldByTwo(const void* actor, std::uint64_t weak) noexcept
{
    return HolderNames(TwoHolderOf(actor), weak) || HeldBy(actor, weak);
}

/// Claims player 2's holder for `actor` (game thread): the existing one, or a free slot set up as the game's BeginPlay
/// sets the actor's (zeroed, the owning actor at +0x20). Null when all are in use.
std::byte* ClaimTwoHolder(void* actor) noexcept
{
    if (std::byte* held = TwoHolderOf(actor))
    {
        return held;
    }
    for (TwoHolder& two : g_twoHolders)
    {
        if (two.actor.load(std::memory_order_relaxed) == nullptr)
        {
            std::memset(two.bytes, 0, sizeof(two.bytes));
            std::memcpy(two.bytes + kHolderActorAt, &actor, sizeof(actor));
            two.loggedActive = false;
            two.actor.store(actor, std::memory_order_release);
            return two.bytes;
        }
    }
    return nullptr;
}

/// Releases a holder slot (game thread), leaving its camera first with the game's forced leave (as EndPlay leaves the
/// actor's). The leave reaches the module only while its shared count is above zero, so it is safe after player 2's
/// camera manager is gone.
void ReleaseTwoHolder(TwoHolder& two, const wchar_t* why) noexcept
{
    const void* actor = two.actor.load(std::memory_order_relaxed);
    if (actor == nullptr)
    {
        return;
    }
    const bool active = HolderActive(two.bytes);
    if (active && g_holderLeave != nullptr)
    {
        g_holderLeave(two.bytes, nullptr, 1);
    }
    two.actor.store(nullptr, std::memory_order_release);
    // Player 2's move whose camera this was: the rotation gate stops naming it (its move may be freed and reused).
    g_playerTwoMove.store(nullptr, std::memory_order_release);
    g_playerTwoMemberWeak.store(0, std::memory_order_relaxed);
    platform::LogDebug(L"traversal camera: player 2's own holder for actor 0x{:X} released ({}){}",
                       reinterpret_cast<std::uintptr_t>(actor), why, active ? L"; its camera left first" : L"");
}

/// The holder install (game thread). Player 2's Begin installs on player 2's own holder for the actor: player 1's camera
/// on the actor's holder stays, and player 1's later install never evicts player 2's.
std::uintptr_t HolderInstall(void* holder, void* module, void* args) noexcept
{
    const auto original = reinterpret_cast<InstallFn>(g_installEntry.Original());
    if (!t_twoBegin || holder == nullptr || args == nullptr || ReadPointer(args, 0) != generation::Member())
    {
        return original(holder, module, args);
    }
    std::byte* two = TwoHolderOf(static_cast<const std::byte*>(holder) - kHolder);
    return original(two != nullptr ? static_cast<void*>(two) : holder, module, args);
}

/// The holder's module getter (game thread in the installers, anim worker in the per-frame setters). Inside player 2's
/// Begin or player 2's climb Tick: player 2's own holder; with none, the actor's only if player 2's member installed it,
/// else no module (player 2's climb never moves player 1's camera). Everything else: the game's.
void* HolderModule(void* holder) noexcept
{
    const auto original = reinterpret_cast<GetModuleFn>(g_getModuleEntry.Original());
    if (holder == nullptr || (!t_twoBegin && climb_context::PlayerHere() != 2))
    {
        return original(holder);
    }
    if (std::byte* two = TwoHolderOf(static_cast<const std::byte*>(holder) - kHolder))
    {
        return original(two);
    }
    return HolderNames(static_cast<const std::byte*>(holder), g_memberWeakNow.load(std::memory_order_relaxed))
               ? original(holder)
               : nullptr;
}

/// The actor's holder leave (`vt+0x8F8`, from the camera off; anim worker on a normal finish, game thread on an abort):
/// player 2's member leaves player 2's own holder too. The game's leave on the actor's holder follows (its owner check
/// keeps player 1's camera).
template <std::size_t Index>
std::uintptr_t HolderLeave(void* actor, void* character) noexcept
{
    const auto original = reinterpret_cast<LeaveSlotFn>(g_move[Index].leave.Original());
    if (character != nullptr && character == g_memberNow.load(std::memory_order_relaxed))
    {
        if (std::byte* two = TwoHolderOf(actor))
        {
            g_holderLeave(two, character, 0);
        }
    }
    return original(actor, character);
}

struct LeaveCall
{
    LeaveSlotFn    leave;
    void*          actor;
    void*          character;
    std::uintptr_t result = 0;
};

void RunLeave(void* context)
{
    auto* call   = static_cast<LeaveCall*>(context);
    call->result = call->leave(call->actor, call->character);
}

/// The one-shot actor's leave (any thread): for player 2's member, or with no character the actor player 2's camera went
/// on, the game's leave ends the camera on player 2's camera manager. Everyone else's is the game's.
std::uintptr_t OneShotLeave(void* actor, void* character) noexcept
{
    const auto  original = reinterpret_cast<LeaveSlotFn>(g_oneShotLeave.Original());
    const void* member   = g_memberNow.load(std::memory_order_relaxed);
    const bool  two      = member != nullptr && actor != nullptr &&
                     (character != nullptr ? character == member
                                           : leader_scope::StillNames(g_oneShotTwoWeak.load(std::memory_order_acquire),
                                                                      actor));
    if (!two)
    {
        return original(actor, character);
    }
    const auto flag = kExecClasses[kOneShot].cameraFlag;
    const bool was  = ReadField<std::uint8_t>(actor, flag) != 0;
    LeaveCall  call{original, actor, character};
    const bool ran  = climb_context::WithPlayerTwoCamera(&RunLeave, &call);
    if (!ran)
    {
        // Player 2's camera manager is gone (player 2 left): no camera of theirs to end. The leave's own last step
        // only, so a later leave of this actor cannot reach player 1's slot 15.
        WriteField<std::uint8_t>(actor, flag, 0);
    }
    std::uint64_t recorded = g_oneShotTwoWeak.load(std::memory_order_acquire);
    if (leader_scope::StillNames(recorded, actor))
    {
        g_oneShotTwoWeak.compare_exchange_strong(recorded, 0, std::memory_order_acq_rel);
    }
    try
    {
        platform::LogDebug(L"traversal camera: player 2's one-shot move ends ({} thread{}): {}",
                           generation::OnGameThread() ? L"game" : L"animation",
                           character == nullptr ? L", the actor's EndPlay" : L"",
                           !was  ? L"its camera flag was already off (no camera, or ended before)"
                           : ran ? L"the game's leave ran on player 2's camera; player 1's untouched"
                                 : L"player 2's camera manager is gone: the flag cleared, player 1's camera untouched");
    }
    catch (...)
    {
    }
    return call.result;
}

/// The actor's EndPlay (game thread): player 2's holder for it is left and released before the game's own.
template <std::size_t Index>
std::uintptr_t HolderEndPlay(void* actor, std::uintptr_t reason) noexcept
{
    const auto original = reinterpret_cast<EndPlayFn>(g_move[Index].endPlay.Original());
    if (generation::OnGameThread())
    {
        for (TwoHolder& two : g_twoHolders)
        {
            if (two.actor.load(std::memory_order_relaxed) == actor)
            {
                ReleaseTwoHolder(two, L"the actor ends play");
            }
        }
    }
    return original(actor, reason);
}

template <std::size_t Index>
std::uintptr_t MoveBegin(void* move) noexcept
{
    const auto original = reinterpret_cast<BeginFn>(g_move[Index].begin.Original());
    if (g_inside || !generation::Active() || !generation::OnGameThread())
    {
        return original(move);
    }
    void*      member     = generation::Member();
    const auto memberWeak = member != nullptr ? leader_scope::WeakOf(member) : 0;
    void*      actor      = ObjectIn(move, kMoveActor);
    if (actor != nullptr && VTableOf(actor) != g_move[Index].actorVt)
    {
        actor = nullptr;
    }
    const MoveClass& cls = kMoveClasses[Index];
    if (member == nullptr || ObjectIn(move, kMoveCharacter) != member)
    {
        // Anyone else's Begin: with player 2's own holder it never touches player 2's camera. Without (the fallback, on
        // the actor's holder), if it takes over a holder player 2's camera held, player 2's rotation stops with it.
        if (g_twoHoldersOn)
        {
            return original(move);
        }
        const bool heldByTwo = HeldBy(actor, memberWeak);
        const auto result    = original(move);
        if (heldByTwo && !HeldBy(actor, memberWeak))
        {
            g_playerTwoMove.store(nullptr, std::memory_order_release);
            try
            {
                platform::LogDebug(L"traversal camera: another character's {} on {} took the camera holder from player 2",
                                   cls.name, ActorName(actor));
            }
            catch (...)
            {
            }
        }
        return result;
    }
    // Player 2's new move: the hip calls follow only a climbing wall this Begin installs player 2's camera on.
    g_climbActor.store(nullptr, std::memory_order_release);
    // Player 2's own holder for this actor; without one (every slot in use, or the hooks are not placed), the actor's.
    const std::byte* own = g_twoHoldersOn && actor != nullptr ? ClaimTwoHolder(actor) : nullptr;
    if (g_twoHoldersOn && actor != nullptr && own == nullptr)
    {
        try
        {
            platform::Log(L"traversal camera: player 2's own camera holders are all in use; player 2's {} on {} uses the "
                          L"wall's own",
                          cls.name, ActorName(actor));
        }
        catch (...)
        {
        }
    }
    // Player 1 holds this actor's camera: a second install would evict it (the holder deactivates the previous module).
    if (own == nullptr && actor != nullptr && ReadField<std::uint8_t>(actor, kHolder) != 0 && !HeldBy(actor, memberWeak))
    {
        try
        {
            platform::LogDebug(L"traversal camera: player 2's {} on {} begins without a camera: another character's "
                               L"camera holds it",
                               cls.name, ActorName(actor));
        }
        catch (...)
        {
        }
        return original(move);
    }
    const bool rail = actor != nullptr && ReadField<std::uint8_t>(actor, kActorRail) != 0;
    BeginCall  call{original, move};
    bool       scoped = false;
    {
        const Inside                             inside;
        const TwoBegin                           twoBegin;
        std::optional<leader_scope::LeaderScope> leader;
        if (rail)
        {
            leader.emplace(memberWeak);
        }
        scoped = member_movement::InPrimaryScope(member, &RunBegin, &call);
        if (!scoped)
        {
            call.result = original(move);
        }
    }
    const bool installed = HeldByTwo(actor, memberWeak);
    if (installed)
    {
        g_playerTwoMemberWeak.store(memberWeak, std::memory_order_relaxed);
        g_playerTwoMove.store(move, std::memory_order_release);
        if (Index == kClimbingWall)
        {
            g_climbMember.store(member, std::memory_order_relaxed);
            g_climbActor.store(actor, std::memory_order_release);
        }
    }
    try
    {
        platform::LogDebug(L"traversal camera: player 2's {} on {} begins: {}{}; camera holder {}", cls.name,
                           ActorName(actor), scoped ? L"as player 2" : L"a scope was refused, the game's answer stands",
                           rail ? L" (rail camera)" : L"",
                           HolderNames(own, memberWeak) ? L"player 2's own (the wall's stays the game's)"
                           : installed                  ? L"the wall's, player 2's"
                                                        : L"not player 2's");
    }
    catch (...)
    {
    }
    return call.result;
}

/// The per-frame rotation's move gate (anim worker): the game's answer, or yes for player 2's installed move.
template <std::size_t Index>
std::uint8_t MoveGateCall(const void* move) noexcept
{
    const auto answer = reinterpret_cast<GateFn>(g_move[Index].gate.Original())(move);
    if (answer != 0 || move == nullptr || move != g_playerTwoMove.load(std::memory_order_acquire))
    {
        return answer;
    }
    const auto weak = g_playerTwoMemberWeak.load(std::memory_order_relaxed);
    return weak != 0 && ReadField<std::uint64_t>(move, kMoveCharacter) == weak ? std::uint8_t{1} : answer;
}

/// The actor's holder check (field-actor manager tick, game thread): a holder player 2's member installed is checked
/// with the party leader's type answering the member's, so the game keeps player 2's camera as it keeps player 1's.
/// Every other holder, and anything off the game thread, gets the game's own check. Logged when the outcome changes.
template <std::size_t Index>
std::uintptr_t ValidateHolder(void* actor) noexcept
{
    const auto original = reinterpret_cast<ValidateFn>(g_move[Index].validate.Original());
    if (!generation::Active() || !generation::OnGameThread())
    {
        return original(actor);
    }
    void*      member     = generation::Member();
    const auto memberWeak = member != nullptr ? leader_scope::WeakOf(member) : 0;
    std::byte* own        = TwoHolderOf(actor);
    const bool ownOn      = HolderActive(own);
    const bool wallTwo    = HeldBy(actor, memberWeak);  // the fallback: player 2's camera on the actor's holder
    if (!ownOn && !wallTwo)
    {
        return original(actor);
    }
    const auto     type  = party_member::PlayerTypeOf(member);
    bool           typed = false;
    std::uintptr_t result;
    {
        std::optional<leader_scope::LeaderTypeScope> scope;
        if (wallTwo && type)
        {
            scope.emplace(*type);
            typed = scope->Held();
        }
        result = original(actor);
    }
    bool kept = wallTwo && HeldBy(actor, memberWeak);
    if (ownOn)
    {
        // The game's validate on player 2's own holder, with the leader type answering the member's (W1).
        {
            std::optional<leader_scope::LeaderTypeScope> scope;
            if (type)
            {
                scope.emplace(*type);
                typed = scope->Held();
            }
            g_holderValidate(own);
        }
        kept = HolderActive(own);
        if (kept)
        {
            result &= ~std::uintptr_t{0xFF};  // "can deactivate": not while player 2's camera is on
        }
    }
    MoveLive& live = g_move[Index];
    if (live.seenActor != actor || live.seenKept != kept)
    {
        live.seenActor = actor;
        live.seenKept  = kept;
        try
        {
            const auto level = kept ? platform::Level::Debug : platform::Level::Info;
            platform::LogAt(level, L"traversal camera: the game's holder check {} player 2's {} camera on {} ({})",
                            kept ? L"keeps" : L"still REMOVED", kMoveClasses[Index].name, ActorName(actor),
                            typed ? L"player 2's member counted as the leader's type"
                                  : L"the leader type could not be set: the game's own check");
        }
        catch (...)
        {
        }
    }
    return result;
}

/// The climbing move's phase-3 player test before the hip-framing setter (anim worker; game thread on an abort): the
/// game's answer, or yes for player 2's member on the wall whose holder player 2's Begin filled. The holder is read
/// guarded (the wall is cached raw), and must still name player 2's member: a later install by anyone else ends it.
template <std::size_t Site>
std::uint8_t HipTrackCall(const void* character) noexcept
{
    const auto answer = reinterpret_cast<CoreFn>(g_hipCalls[Site].Original())(character);
    if (answer != 0 || character == nullptr || character != g_climbMember.load(std::memory_order_relaxed))
    {
        return answer;
    }
    const void* actor = g_climbActor.load(std::memory_order_acquire);
    const auto  weak  = g_playerTwoMemberWeak.load(std::memory_order_relaxed);
    struct
    {
        std::uint8_t  active;
        std::uint8_t  pad[3];
        std::uint32_t ownerLow, ownerHigh;  // the installer's FWeakObjectPtr at +0x4 (unaligned for a u64)
    } holder{};
    const std::byte* own  = TwoHolderOf(actor);  // player 2's own holder, else the wall's
    const bool       held = actor != nullptr && weak != 0 && generation::Active() &&
                      platform::CopyGuarded(&holder, own != nullptr ? own : static_cast<const std::byte*>(actor) + kHolder,
                                            sizeof(holder)) &&
                      holder.active != 0 &&
                      ((static_cast<std::uint64_t>(holder.ownerHigh) << 32) | holder.ownerLow) == weak;
    try
    {
        platform::LogDebug(L"traversal camera: player 2's climbing move {} the wall's hold phase ({} thread): hip framing "
                           L"{}",
                           kHipCallNames[Site], generation::OnGameThread() ? L"game" : L"animation",
                           held ? (Site == 0 ? L"on, as player 1's" : L"off, as player 1's")
                                : L"left as the game decides (player 2's camera does not hold this wall)");
    }
    catch (...)
    {
    }
    return held ? std::uint8_t{1} : answer;
}

// --- collision watch (game thread, per frame; lines on change) ------------------------------------------------------

/// One traversal camera's time on a player's slot 15: frames, and frames the collision pulled the camera in, with the
/// climbing wall's hip framing off and on, then the frames after it left (the hand-over to the follow camera).
struct CollisionWatch
{
    const void*    module   = nullptr;
    std::uintptr_t vtable   = 0;  // research
    unsigned       frames[3]{}, pulled[3]{};
    std::wstring   firstHit[3];
    unsigned       after = 0;  // frames counted since the module left (0: none)
};
std::array<CollisionWatch, 2> g_collision;

const void* SlotFifteen(const void* cameraManager) noexcept
{
    const void* op      = ReadPointer(cameraManager, facts::kCameraManagerFieldOperator);
    const void* modules = op != nullptr ? ReadPointer(op, kOpModules) : nullptr;
    return modules != nullptr ? ReadPointer(modules, kSlotFifteen) : nullptr;
}

void LogCollision(int player, const CollisionWatch& w)
{
    static constexpr const wchar_t* kBucket[] = {L"hip framing off", L"hip framing on", L"after it ended"};
    std::wstring text;
    for (int b = 0; b < 3; ++b)
    {
        if (w.frames[b] == 0)
        {
            continue;
        }
        text += std::format(L"{}{}: pulled in {} of {} frame(s){}", text.empty() ? L"" : L"; ", kBucket[b], w.pulled[b],
                            w.frames[b], w.firstHit[b].empty() ? L"" : L", first hit " + w.firstHit[b]);
    }
    platform::LogDebug(L"traversal camera: player {}'s {} camera (vt 0x{:X}) collision: {}", player + 1,
                       w.vtable == kClimbingModuleVt ? L"climbing wall" : L"traversal", w.vtable,
                       text.empty() ? std::wstring(L"no frames") : text);
}

void WatchCollision(int player, const void* cameraManager)
{
    CollisionWatch& w      = g_collision[player];
    const void*     module = cameraManager != nullptr ? SlotFifteen(cameraManager) : nullptr;
    const auto      vt     = module != nullptr ? VTableOf(module) : 0;
    // Only the traversal modules this file installs or watches: the climbing wall's and player 2's ladder's (any other
    // slot-15 module, a ride framing or a cutscene, is left to its own watch).
    const bool traversal =
        module != nullptr && (vt == g_climbingModuleVt || (player == 1 && module == g_ladderModule));
    if (module != w.module && w.module != nullptr && w.after == 0)
    {
        w.after = 1;  // the traversal camera left: watch the hand-over
    }
    if (traversal && module != w.module)
    {
        if (w.module != nullptr)
        {
            LogCollision(player, w);
        }
        w        = CollisionWatch{};
        w.module = module;
        w.vtable = g_image ? g_image->ResearchAddress(vt) : vt;
    }
    if (w.module == nullptr || cameraManager == nullptr)
    {
        return;
    }
    int bucket = 2;
    if (w.after == 0)
    {
        bucket = vt == g_climbingModuleVt && ReadField<float>(module, kClimbHipBlend) > 0.5f ? 1 : 0;
    }
    ++w.frames[bucket];
    if (ReadField<std::uint8_t>(cameraManager, kPcmPulledIn) != 0)
    {
        ++w.pulled[bucket];
        if (w.firstHit[bucket].empty())
        {
            const void* hit = leader_scope::ObjectAt(static_cast<const std::byte*>(cameraManager) + kPcmHitActor);
            w.firstHit[bucket] = hit != nullptr ? NameOf(hit) : std::wstring(L"(no actor)");
        }
    }
    if (w.after != 0 && ++w.after > kAfterFrames)
    {
        LogCollision(player, w);
        w = CollisionWatch{};
    }
}

// --- install ----------------------------------------------------------------------------------------------------------

std::uintptr_t Live(const platform::GameImage& image, std::uintptr_t research, std::size_t size) noexcept
{
    const auto at = image.Rebase(research, size);
    return at ? *at : 0;
}

template <std::size_t Index>
std::wstring PlaceExec(const platform::GameImage& image)
{
    const ExecClass& cls  = kExecClasses[Index];
    ExecLive&        live = g_exec[Index];
    if (cls.handleSize > kMaxHandle || cls.handleSize % 8 != 0)
    {
        return std::format(L"{} NO (its camera handle does not fit player 2's copy)", cls.name);
    }
    live.actorVt          = Live(image, cls.actorVt, 0x740);
    if (cls.start.slot != 0)
    {
        live.startPlaced =
            platform::ReplaceSlot(image, cls.start, reinterpret_cast<std::uintptr_t>(&ExecStart<Index>), live.start);
    }
    if (cls.end.slot != 0)
    {
        live.endPlaced =
            platform::ReplaceSlot(image, cls.end, reinterpret_cast<std::uintptr_t>(&ExecEnd<Index>), live.end);
    }
    if (cls.tickSlot.slot != 0)
    {
        live.tickPlaced =
            platform::ReplaceSlot(image, cls.tickSlot, reinterpret_cast<std::uintptr_t>(&ExecTick<Index>), live.tick);
    }
    const bool ok = live.actorVt != 0 && (cls.start.slot == 0 || live.startPlaced) &&
                    (cls.end.slot == 0 || live.endPlaced) && (cls.tickSlot.slot == 0 || live.tickPlaced);
    return std::format(L"{} {}", cls.name, ok ? L"yes" : L"NO");
}

template <std::size_t Index>
std::wstring PlaceMove(const platform::GameImage& image)
{
    const MoveClass& cls  = kMoveClasses[Index];
    MoveLive&        live = g_move[Index];
    live.actorVt          = Live(image, cls.actorVt, 0x740);
    live.beginPlaced =
        live.actorVt != 0 &&
        platform::ReplaceSlot(image, cls.begin, reinterpret_cast<std::uintptr_t>(&MoveBegin<Index>), live.begin);
    bool gate = true;
    if (cls.gateCall != 0)
    {
        const auto site   = image.Rebase(cls.gateCall, 5);
        const auto target = platform::FindFunction(image, kMoveGate);
        gate = site && target &&
               live.gate.Install(*site, *target, reinterpret_cast<std::uintptr_t>(&MoveGateCall<Index>));
    }
    live.validatePlaced = live.beginPlaced &&
                          platform::ReplaceSlot(image, kValidateSlots[Index],
                                                reinterpret_cast<std::uintptr_t>(&ValidateHolder<Index>), live.validate);
    live.leavePlaced    = live.beginPlaced &&
                       platform::ReplaceSlot(image, kLeaveSlots[Index],
                                             reinterpret_cast<std::uintptr_t>(&HolderLeave<Index>), live.leave);
    live.endPlayPlaced  = live.beginPlaced &&
                         platform::ReplaceSlot(image, kEndPlaySlots[Index],
                                               reinterpret_cast<std::uintptr_t>(&HolderEndPlay<Index>), live.endPlay);
    return std::format(L"{} {}{}{}", cls.name, live.beginPlaced ? L"yes" : L"NO",
                       gate ? L"" : L" (its per-frame rotation stays player 1's only)",
                       !live.beginPlaced || live.validatePlaced ? L""
                                                                : L" (the holder check differs: the game removes it "
                                                                  L"one tick after player 2's install)");
}

template <std::size_t... I>
std::wstring PlaceAllExec(const platform::GameImage& image, std::index_sequence<I...>)
{
    std::wstring text;
    ((text += (text.empty() ? L"" : L", ") + PlaceExec<I>(image)), ...);
    return text;
}

template <std::size_t... I>
std::wstring PlaceAllMoves(const platform::GameImage& image, std::index_sequence<I...>)
{
    std::wstring text;
    ((text += (text.empty() ? L"" : L", ") + PlaceMove<I>(image)), ...);
    return text;
}

template <std::size_t... I>
bool PlaceHipCalls(const platform::GameImage& image, std::uintptr_t core, std::index_sequence<I...>)
{
    const auto place = [&](std::size_t index, std::uintptr_t replacement) {
        const auto site = image.Rebase(kHipCalls[index], 5);
        return site && g_hipCalls[index].Install(*site, core, replacement);
    };
    return (place(I, reinterpret_cast<std::uintptr_t>(&HipTrackCall<I>)) & ...);
}

/// Player 2's own holders: the two entry redirects and the game's leave and validate, on only with every move class's
/// slots and climb_context's Ticks placed (without the Tick context the module getter cannot tell player 2's climb).
bool PlaceTwoHolders(const platform::GameImage& image) noexcept
{
    bool slots = climb_context::TicksPlaced();
    for (const MoveLive& live : g_move)
    {
        slots = slots && live.beginPlaced && live.validatePlaced && live.leavePlaced && live.endPlayPlaced;
    }
    const auto leave    = platform::FindFunction(image, kHolderLeave);
    const auto validate = platform::FindFunction(image, kHolderValidate);
    if (!slots || !leave || !validate)
    {
        return false;
    }
    g_holderLeave    = reinterpret_cast<HolderLeaveFn>(*leave);
    g_holderValidate = reinterpret_cast<HolderValidateFn>(*validate);
    const std::span<const std::uint8_t> install(kInstallBytes);
    const auto                          installAt = image.Rebase(kInstall);
    bool installed = false;
    if (installAt && image.BytesMatch(kInstall, install))
    {
        // 1.005's entry instruction crosses a 16-byte block: the redirect goes on the second (the first is a store
        // into the caller's home area, and no branch lands on the second; RE-p2-traversal-climb-holder.md §4).
        installed = platform::RunningBuild() == platform::BuildIndex::V1005
                        ? g_installEntry.InstallAtSecondInstruction(*installAt, install.first(kInstallFirst),
                                                                    install.subspan(kInstallFirst, kInstallSecond),
                                                                    reinterpret_cast<std::uintptr_t>(&HolderInstall))
                        : g_installEntry.Install(*installAt, image.EntryInstruction(kInstall, install.first(kInstallFirst)),
                                                 reinterpret_cast<std::uintptr_t>(&HolderInstall));
    }
    const std::span<const std::uint8_t> getModule(kGetModuleBytes);
    const auto                          getAt = image.Rebase(kGetModule);
    const bool got = installed && getAt && image.BytesMatch(kGetModule, getModule) &&
                     g_getModuleEntry.Install(*getAt, image.EntryInstruction(kGetModule, getModule.first(kGetModuleFirst)),
                                              reinterpret_cast<std::uintptr_t>(&HolderModule));
    if (installed && !got)
    {
        g_installEntry.Restore();
    }
    return got;
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    try
    {
        g_image.emplace(image);
        const auto executers = PlaceAllExec(image, std::make_index_sequence<kExecCount>{});
        g_oneShotLeavePlaced = g_exec[kOneShot].tickPlaced &&
                               platform::ReplaceSlot(image, kOneShotLeaveSlot,
                                                     reinterpret_cast<std::uintptr_t>(&OneShotLeave), g_oneShotLeave);
        const auto moves     = PlaceAllMoves(image, std::make_index_sequence<kMoveCount>{});
        platform::Log(L"traversal camera: player 2's traversal cameras: {}; {} (a NO keeps that class on the follow "
                      L"camera for player 2)",
                      executers, moves);

        const auto request = platform::FindFunction(image, kRequestCamExit);
        g_requestCamExit   = request ? reinterpret_cast<RequestFn>(*request) : nullptr;
        const auto squeeze = platform::FindFunction(image, kSqueezeExit);
        g_squeezeExit      = squeeze ? reinterpret_cast<OneArgFn>(*squeeze) : nullptr;
        const auto off     = platform::FindFunction(image, kCameraOff);
        g_cameraOff        = off ? reinterpret_cast<OneArgFn>(*off) : nullptr;
        const auto core    = platform::FindFunction(image, facts::kIsPlayerCore);
        const bool hip     = core && g_move[kClimbingWall].beginPlaced &&
                         PlaceHipCalls(image, *core, std::make_index_sequence<std::size(kHipCalls)>{});
        const auto vt      = image.Rebase(kClimbingModuleVt, 0x30);
        g_climbingModuleVt = vt ? *vt : 0;
        g_twoHoldersOn     = PlaceTwoHolders(image);
        platform::Log(L"traversal camera: climbing walls, monkey bars and spider walls {}",
                      g_twoHoldersOn ? L"keep a camera holder per player (neither player's climb evicts the other's)"
                                     : L"keep the game's one holder (a hook or climb_context's Ticks are missing): the "
                                       L"first climber keeps the camera, player 2's is evicted by player 1's");
        platform::Log(L"traversal camera: squeeze exit {}, crawl exit {}, one-shot end {} (player 2's camera off where "
                      L"the move ends, as player 1's)",
                      g_squeezeExit != nullptr ? L"yes" : L"NO", g_cameraOff != nullptr ? L"yes" : L"NO",
                      g_oneShotLeavePlaced ? L"yes" : L"NO (player 2's one-shot camera ends at its own timer; the end "
                                                      L"can switch off player 1's slot 15)");
        platform::Log(L"traversal camera: player 2's ladder exit {}; climbing wall hip framing {}; collision watch {}",
                      g_requestCamExit != nullptr ? L"blends into the follow camera as player 1's (the game's request)"
                                                  : L"UNAVAILABLE (the request's bytes differ: a cut at the top)",
                      hip ? L"follows player 2's hold phases" : L"UNAVAILABLE (a call differs: player 2 climbs without it)",
                      g_climbingModuleVt != 0 ? L"on (Debug, a line per traversal camera)"
                                              : L"for player 2's ladder only (the climbing module's vtable is not found)");
    }
    catch (...)
    {
    }
}

void Remove() noexcept
{
    bool kept = false;
    for (ExecLive& live : g_exec)
    {
        kept |= live.startPlaced && !live.start.Restore();
        kept |= live.endPlaced && !live.end.Restore();
        kept |= live.tickPlaced && !live.tick.Restore();
        live.startPlaced = live.endPlaced = live.tickPlaced = false;
    }
    kept |= g_oneShotLeavePlaced && !g_oneShotLeave.Restore();
    g_oneShotLeavePlaced = false;
    g_oneShotTwoWeak.store(0, std::memory_order_release);
    for (TwoHolder& two : g_twoHolders)
    {
        ReleaseTwoHolder(two, L"the mod is removed");
    }
    g_twoHoldersOn = false;
    kept |= g_getModuleEntry.IsInstalled() && !g_getModuleEntry.Restore();
    kept |= g_installEntry.IsInstalled() && !g_installEntry.Restore();
    for (MoveLive& live : g_move)
    {
        kept |= live.beginPlaced && !live.begin.Restore();
        kept |= live.gate.IsInstalled() && !live.gate.Restore();
        kept |= live.validatePlaced && !live.validate.Restore();
        kept |= live.leavePlaced && !live.leave.Restore();
        kept |= live.endPlayPlaced && !live.endPlay.Restore();
        live.beginPlaced = live.validatePlaced = live.leavePlaced = live.endPlayPlaced = false;
    }
    for (platform::CallRedirect& call : g_hipCalls)
    {
        kept |= call.IsInstalled() && !call.Restore();
    }
    g_climbActor.store(nullptr, std::memory_order_release);
    if (kept)
    {
        platform::Log(L"traversal camera: a slot or call was changed after us; left in place (it passes through)");
    }
}

void Tick() noexcept
{
    if (!generation::OnGameThread())
    {
        return;
    }
    if (!generation::Active())
    {
        g_collision = {};
        g_ladderModule = nullptr;
        g_climbActor.store(nullptr, std::memory_order_release);
        g_memberNow.store(nullptr, std::memory_order_relaxed);
        g_memberWeakNow.store(0, std::memory_order_relaxed);
        g_oneShotTwoWeak.store(0, std::memory_order_release);
        for (TwoHolder& two : g_twoHolders)
        {
            ReleaseTwoHolder(two, L"player 2 is out");
        }
        return;
    }
    void* member = generation::Member();
    g_memberNow.store(member, std::memory_order_relaxed);
    g_memberWeakNow.store(member != nullptr ? leader_scope::WeakOf(member) : 0, std::memory_order_relaxed);
    // Player 2's holders: a line when one's camera goes on or off; an idle one is released (a new Begin claims again).
    for (TwoHolder& two : g_twoHolders)
    {
        const void* actor = two.actor.load(std::memory_order_relaxed);
        if (actor == nullptr)
        {
            continue;
        }
        const bool active = HolderActive(two.bytes);
        if (active != two.loggedActive)
        {
            two.loggedActive = active;
            try
            {
                platform::LogDebug(L"traversal camera: player 2's own holder on {}: camera {}", ActorName(actor),
                                   active ? L"on (player 2's camera manager)" : L"off");
            }
            catch (...)
            {
            }
        }
        if (!active)
        {
            ReleaseTwoHolder(two, L"idle");
        }
    }
    try
    {
        void* const controllers[2] = {generation::PlayerOneController(), generation::PlayerTwoController()};
        for (int p = 0; p < 2; ++p)
        {
            WatchCollision(p, controllers[p] != nullptr ? ReadPointer(controllers[p], facts::kControllerCameraManager)
                                                        : nullptr);
        }
    }
    catch (...)
    {
    }
}

std::uintptr_t LadderInput(LadderInputFn original, void* executer, float dt) noexcept
{
    if (g_exec[kLadder].actorVt == 0 || g_inside || !PlayerTwos(executer))
    {
        const auto result = original(executer, dt);
        // Player 1's slide flag, for the log (both on one ladder: player 2's input must not change it).
        void* one = generation::Active() && generation::OnGameThread() ? generation::PlayerOneController() : nullptr;
        if (one != nullptr && g_exec[kLadder].actorVt != 0 &&
            ObjectIn(executer, kExecCharacter) == ReadPointer(one, facts::kControllerPawn))
        {
            void* actor = ObjectIn(executer, kExecClasses[kLadder].actorLater);
            if (actor != nullptr && VTableOf(actor) == g_exec[kLadder].actorVt)
            {
                NoteSlide(0, SlideFlag(ReadHandle(actor, kExecClasses[kLadder])));
            }
        }
        return result;
    }
    // RE-p2-traversal-worker-gates.md W4: the input step sets the ladder module's slide flag (+0x434, a wider FOV while
    // sliding) through the handle on the ladder. Window B already holds player 2's scopes; only player 2's handle is
    // put in, so the write reaches player 2's camera and leaves player 1's flag alone.
    const ExecClass& cls   = kExecClasses[kLadder];
    void*            actor = ObjectIn(executer, cls.actorLater);
    const auto       found = actor != nullptr && VTableOf(actor) == g_exec[kLadder].actorVt
                                 ? g_handles.find(leader_scope::WeakOf(actor))
                                 : g_handles.end();
    if (found == g_handles.end())
    {
        return original(executer, dt);
    }
    const auto        actorWeak = found->first;
    const HandleBytes theirs    = ReadHandle(actor, cls);
    WriteHandle(actor, cls, found->second);
    std::uintptr_t result = 0;
    {
        const Inside inside;
        result = original(executer, dt);
    }
    if (leader_scope::StillNames(actorWeak, actor))
    {
        found->second = ReadHandle(actor, cls);
        WriteHandle(actor, cls, theirs);
    }
    else
    {
        found->second = {};
    }
    NoteSlide(1, SlideFlag(found->second));
    return result;
}

std::uintptr_t LadderStart(ExecFn original, void* executer) noexcept
{
    if (g_exec[kLadder].actorVt == 0 || g_inside || !PlayerTwos(executer))
    {
        return original(executer);
    }
    Call call{Kind::Start, original, nullptr, executer};
    return ExecStep(kLadder, Kind::Start, call);
}

std::uintptr_t LadderEnd(ExecFn original, void* executer) noexcept
{
    if (g_exec[kLadder].actorVt == 0 || g_inside || !PlayerTwos(executer))
    {
        return original(executer);
    }
    Call call{Kind::End, original, nullptr, executer};
    return ExecStep(kLadder, Kind::End, call);
}

}  // namespace lss::player_two::traversal_camera
