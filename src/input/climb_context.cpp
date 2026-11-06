#include "input/climb_context.h"

#include "facts/controller.h"
#include "input/pad_buttons.h"
#include "input/pad_sticks.h"
#include "platform/angles.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/every.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "scopes/leader_scope.h"
#include "session/party_member.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <utility>

namespace lss::input::climb_context {

namespace {

// 1.004 addresses; 1.005 through replaced-traversal-climb.json (every slot, function, key and call site read from both
// exes) and replaced-chocobo-jump-cam.json (the camera manager getter, the shared Finish).

constexpr std::uint8_t kTickBytes[]          = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48,
                                                0x89, 0x70, 0x10, 0x48, 0x89, 0x78, 0x18, 0x55};
constexpr std::uint8_t kClimbFinishBytes[]   = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr std::uint8_t kBarsFinishBytes[]    = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr std::uint8_t kSpiderFinishBytes[]  = {0xB8, 0x28, 0x00, 0x00, 0x00};
constexpr std::uint8_t kEdgeStopTickBytes[]  = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr std::uint8_t kCameraGetterBytes[]  = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                                0x24, 0x10, 0x57, 0xB8, 0x80, 0x00, 0x00, 0x00};
constexpr std::uint8_t kKeyValueBytes[]      = {0xB8, 0x38, 0x00, 0x00, 0x00};
constexpr std::uint8_t kPressedBytes[]       = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x40, 0x00, 0x00, 0x00};
constexpr std::size_t  kFirst                = 5;  // each redirected entry's first instruction

const platform::FunctionFact kCameraGetter{L"GetEndPlayerCameraManager", 0x140A60C50, kCameraGetterBytes};
const platform::FunctionFact kKeyValue{L"SquareInput__GetAggregatedKeyValue", 0x1407C8310, kKeyValueBytes};
const platform::FunctionFact kPressedImpl{L"SquareInput_ActionTableF8_IsPressed_Impl", 0x1408062E0, kPressedBytes};
const platform::FunctionFact kPressedGlobal{L"SquareInput_IsActionPressed_Global", 0x14080619C, kPressedBytes};
using facts::kInputKeyCopy;
constexpr std::uintptr_t     kHoldSelectCall = 0x1416368D5;  // ClimbingMove's Tick: call TickHoldSelect
constexpr std::uintptr_t     kHoldSelect     = 0x141635E70;  // ClimbingMove_TickHoldSelect_PrimaryPCMBasis

// The hold-select's three actions (FKey24 statics: the FName first).
constexpr std::uintptr_t   kActionKeys[]    = {facts::kKeyDodge, 0x1491E4508, 0x1491E4A30};
constexpr std::size_t      kActionCount     = std::size(kActionKeys);
using facts::kKeySize;

constexpr std::uint32_t kMoveCharacter = 0x450;   // the move's character, an FWeakObjectPtr
constexpr std::uint32_t kKeyFields     = 0x1120;  // the input owner's fields the key-value getter reads
constexpr std::uint32_t kKeyFieldsEnd  = 0x1264;
constexpr std::uint32_t kFreshFrames   = 1;       // a copy from this frame or the one before
constexpr auto          kSummaryEvery  = std::chrono::seconds(10);

// The edge-stop action (research/RE-p2-ledge-peek.md): its type, phase and phase-3 sub-phase, and the edge's rotation.
constexpr std::uint32_t kActionType     = 0x499;
constexpr std::uint32_t kActionPhase    = 0x411;
constexpr std::uint32_t kLeanSubPhase   = 0x440;
constexpr std::uint32_t kEdgeRotation   = 0x720;  // FQuat; its X axis is the direction the stick is tested against
constexpr unsigned      kLeanPhase      = 3;
constexpr unsigned      kLeanStart      = 0;
constexpr unsigned      kLeanEnd        = 2;
constexpr auto          kStutterWindow  = std::chrono::seconds(2);
constexpr unsigned      kStutterStarts  = 3;

struct MoveClass
{
    const wchar_t*     name;
    platform::SlotFact tick;
    platform::SlotFact finish;
    bool               holdSelect;  // its Tick calls the hold-select (its redirect is required)
    bool               climb;       // a climb: player 2's identity (leader test, buttons) and the climb camera's holder
};
const MoveClass kMoves[] = {
    {L"climbing wall",
     {L"ClimbingMove's Tick slot (+0x10)", 0x145833778, {L"ClimbingMove_Tick_Vt10_CamRot", 0x141636878, kTickBytes}},
     {L"ClimbingMove's Finish slot (+0xA8)", 0x145833810,
      {L"EndBehaviorAction_Finish_CallsEndVt120", 0x1416E9604, kClimbFinishBytes}},
     true, true},
    {L"monkey bars",
     {L"MonkeyBarsMove's Tick slot (+0x10)", 0x146695A18,
      {L"MonkeyBarsMove_Tick_Vt10_CamRotIfPlayer", 0x1432FABC0, kTickBytes}},
     {L"MonkeyBarsMove's Finish slot (+0xA8)", 0x146695AB0, {L"MonkeyBarsMove_Finish_VtA8", 0x1432E91B8, kBarsFinishBytes}},
     false, true},
    {L"spider wall",
     {L"SpiderMoveMove's Tick slot (+0x10)", 0x146696CF8,
      {L"SpiderMoveMove_Tick_Vt10_CamRotIfPlayer", 0x143312EDC, kTickBytes}},
     {L"SpiderMoveMove's Finish slot (+0xA8)", 0x146696D90,
      {L"EndBehaviorAction_VtA8_Thunk_Finish", 0x1416E95EC, kSpiderFinishBytes}},
     false, true},
    // Not a climb: the ledge lean, the step after an edge stop and the walk step (one action class). Only its stick and
    // camera basis become the player's; the identity stays the game's (phases 7-10 not read).
    {L"edge stop (ledge lean)",
     {L"FieldActMove58DFA88's Tick slot (+0x10)", 0x1458DFA98,
      {L"FieldActMove58DFA88_Tick_Vt10", 0x141860338, kEdgeStopTickBytes}},
     {L"FieldActMove58DFA88's Finish slot (+0xA8)", 0x1458DFB30,
      {L"EndBehaviorAction_Finish_CallsEndVt120", 0x1416E9604, kClimbFinishBytes}},
     false, false},
};
constexpr std::size_t kMoveCount  = std::size(kMoves);
constexpr std::size_t kClimbCount = 3;  // the first three entries

using TickFn           = std::uintptr_t (*)(void* move, void* arg);
using FinishFn         = std::uintptr_t (*)(void* move);
using HoldSelectFn     = void (*)(void* move);
using CameraGetterFn   = void* (*)();
using KeyValueFn       = float (*)(const void* owner, void* key);
using PressedFn        = std::uint8_t (*)(void* key);
using KeyCopyFn        = void* (*)(void* out, const void* key);

// --- this thread's climb ----------------------------------------------------------------------------------------------

// The context names the climber by player number: 1 player 1, a seat's number plus one (2 player 2, 3 player 3), 0 none.
// A per-seat record is indexed by the player number minus one, which is the seat (index 0 player 1's).
struct Context
{
    int      player      = 0;      // 0 none, else whose move this thread is ticking (player number)
    bool     suspended   = false;  // a seat's identity (leader test, buttons) off for this call
    bool     climb       = false;  // the move is a climb (PlayerHere answers only for climbs)
    unsigned seatCameras = 0;      // times the camera manager getter answered the seat's in this context
};
thread_local Context t_climb;

/// The seat of a player number (1 player 1 is seat 0).
constexpr std::size_t IndexOfPlayer(int player) noexcept
{
    return static_cast<std::size_t>(player - 1);
}

/// The seat of this thread's context while it names a seat's climber (a player number of 2 or more), else 0.
int SeatHere() noexcept
{
    return t_climb.player >= 2 ? t_climb.player - 1 : 0;
}

class InContext
{
public:
    InContext(int player, bool climb) noexcept : saved_(t_climb) { t_climb = {player, false, climb, 0}; }
    ~InContext() { t_climb = saved_; }
    InContext(const InContext&)            = delete;
    InContext& operator=(const InContext&) = delete;

private:
    Context saved_;
};

class Suspended
{
public:
    Suspended() noexcept : saved_(t_climb.suspended) { t_climb.suspended = true; }
    ~Suspended() { t_climb.suspended = saved_; }
    Suspended(const Suspended&)            = delete;
    Suspended& operator=(const Suspended&) = delete;

private:
    bool saved_;
};

// --- written on the game thread, read on any ------------------------------------------------------------------------

constexpr std::size_t kPlayers = players::kMaxPlayers;

std::atomic<std::uint32_t> g_frame{0};  // engine ticks counted by Tick; 0 = none yet
std::array<std::atomic<std::uint64_t>, kPlayers> g_cameraWeak{};  // by seat: its camera manager (index 0 unused)
static_assert(kPlayers == 3, "the -1 initialisers below name one value per player");
std::array<std::atomic<int>, kPlayers>           g_memberType{-1, -1, -1};  // by seat: its member's type
std::atomic<const void*>   g_owner{nullptr};  // the input owner (the key-value getter's first argument)

// Each player's copy of the owner's key fields, double-buffered: the game thread writes the back buffer, then publishes
// it (front, then the frame stamp, release); a reader takes the stamp (acquire), then the front.
struct alignas(16) OwnerCopy
{
    std::byte bytes[kKeyFieldsEnd + 0xC]{};
};
std::array<std::array<OwnerCopy, 2>, kPlayers>   g_copies{};  // [seat][buffer] (seat = player number - 1)
std::array<std::atomic<int>, kPlayers>           g_front{};
std::array<std::atomic<std::uint32_t>, kPlayers> g_copyFrame{};

// Each seat's presses of the hold-select's actions this frame (bit per action; index 0 unused), and the actions'
// FNames.
std::array<std::atomic<std::uint64_t>, kActionCount> g_actionNames{};
std::array<std::atomic<std::uint32_t>, kPlayers>     g_pressedBits{};
std::array<std::atomic<std::uint32_t>, kPlayers>     g_pressedFrame{};

// Written by the Ticks (any thread): the last frame each player's climb ticked (by seat, 0 player 1's), each seat's
// class.
std::array<std::atomic<std::uint32_t>, kPlayers> g_climbFrame{};
std::array<std::atomic<int>, kPlayers>           g_climbClass{-1, -1, -1};  // (kPlayers == 3: static_assert above)

// By seat (index 0 player 1's where it has one).
struct Counts
{
    std::atomic<unsigned> ticks[kPlayers]{};        // contexts entered
    std::atomic<unsigned> leaderAsks[kPlayers]{};   // leader tests asked inside the seat's context
    std::atomic<unsigned> seatCameras[kPlayers]{};  // camera manager getter answered the seat's
    std::atomic<unsigned> keys[kPlayers]{};         // key values read from a player's own copy
    std::atomic<unsigned> buttons[kPlayers]{};      // hold-select actions answered with the seat's press
    std::atomic<unsigned> holdOff[kPlayers]{};      // hold-selects run without the seat's identity
    std::atomic<unsigned> finishOff[kPlayers]{};    // finishes run without the seat's identity
    std::atomic<unsigned> staleCopies{0};           // a context found no fresh copy (the merged value read instead)
    std::atomic<unsigned> edgeTicks[kPlayers]{};    // edge-stop action ticks
};
Counts g_counts;

// The edge-stop action as each player's Tick last left it (written by that player's Ticks, one at a time).
struct EdgeSeen
{
    std::atomic<std::uint32_t> state{~0u};  // type << 16 | phase << 8 | sub-phase
    std::atomic<const void*>   move{nullptr};
    std::atomic<std::int64_t>  windowAt{0};  // ms: the first lean start of the current stutter window
    std::atomic<unsigned>      starts{0};
    std::atomic<bool>          reported{false};
};
std::array<EdgeSeen, kPlayers> g_edge;  // by seat (player number - 1)

// --- set at install -----------------------------------------------------------------------------------------------------

struct MoveLive
{
    platform::VTableEntry tick;
    platform::VTableEntry finish;
};
std::array<MoveLive, kMoveCount> g_moves;
platform::EntryRedirect          g_cameraEntry;
platform::EntryRedirect          g_keyEntry;
platform::EntryRedirect          g_pressedEntry;
platform::CallRedirect           g_holdSelectCall;
std::uintptr_t                   g_ownerGlobal = 0;  // live address of the input owner's global pointer
std::array<const void*, kActionCount> g_actionKeys{};
PressedFn                        g_pressedGlobal = nullptr;
KeyCopyFn                        g_keyCopy       = nullptr;
bool                             g_installed     = false;
bool                             g_buttons       = false;  // the seats' presses recorded and answered
unsigned                         g_ticksPlaced   = 0;      // climb Tick slots placed (of kClimbCount)

// Game thread only.
std::array<bool, kPlayers> g_loggedOn{};  // by seat (index 0 unused)
platform::Every g_summary{kSummaryEvery};

bool Fresh(std::uint32_t stamp) noexcept
{
    return stamp != 0 && g_frame.load(std::memory_order_relaxed) - stamp <= kFreshFrames;
}

/// The player number whose character `move` belongs to: a seat's member's (its seat + 1), 1 for player 1's pawn, else
/// 0.
int PlayerOf(const void* move) noexcept
{
    const auto weak = ReadField<std::uint64_t>(move, kMoveCharacter);
    if (weak == 0)
    {
        return 0;
    }
    const int seat = players::generation::SeatOfMemberWeak(weak);
    if (players::SeatBuilt(seat))
    {
        return seat + 1;
    }
    return weak == players::generation::PlayerOnePawnWeak() ? 1 : 0;
}

/// The yaw (degrees) of the edge rotation's X axis, the direction the lean's stick test compares with.
float EdgeYaw(const void* move) noexcept
{
    const float x = ReadField<float>(move, kEdgeRotation), y = ReadField<float>(move, kEdgeRotation + 0x4);
    const float z = ReadField<float>(move, kEdgeRotation + 0x8), w = ReadField<float>(move, kEdgeRotation + 0xC);
    return platform::QuaternionYawDegrees(x, y, z, w);
}

std::uint32_t EdgeStateOf(const void* move) noexcept
{
    return static_cast<std::uint32_t>(ReadField<std::uint8_t>(move, kActionType)) << 16 |
           static_cast<std::uint32_t>(ReadField<std::uint8_t>(move, kActionPhase)) << 8 |
           ReadField<std::uint8_t>(move, kLeanSubPhase);
}

/// After a player's edge-stop Tick (that player's thread): Trace per Tick, Debug on change, and the stutter detector
/// (Info once per episode when the lean restarts kStutterStarts times within kStutterWindow).
void NoteEdgeStop(int player, const void* move, unsigned seatCameras) noexcept
{
    EdgeSeen&      seen   = g_edge[IndexOfPlayer(player)];
    const auto     state  = EdgeStateOf(move);
    const unsigned phase  = (state >> 8) & 0xFFu;
    const unsigned sub    = state & 0xFFu;
    const auto     before = seen.state.exchange(state, std::memory_order_relaxed);
    const auto*    was    = seen.move.exchange(move, std::memory_order_relaxed);
    platform::LogTrace(L"edge stop: player {}'s action ticks: type {}, phase {}, sub-phase {}", player, state >> 16,
                       phase, sub);
    if (before == state && was == move)
    {
        return;
    }
    std::wstring basis;  // pad N is player N's (a seat's pad is its player number's)
    try
    {
        basis = player == 1 || seatCameras != 0u
                    ? std::format(L"pad {}'s stick in player {}'s camera", player, player)
                    : std::format(L"pad {}'s stick; no camera read on this tick", player);
    }
    catch (...)
    {
    }
    if (phase == kLeanPhase && sub == kLeanEnd)
    {
        platform::LogDebug(L"edge stop: player {}'s ledge lean ends (stick released or turned away; tested as {}; the "
                           L"edge faces {:.0f} deg)",
                           player, basis, EdgeYaw(move));
        return;
    }
    platform::LogDebug(L"edge stop: player {}'s action: type {}, phase {}, sub-phase {} ({})", player, state >> 16, phase,
                       sub, basis);
    if (phase != kLeanPhase || sub != kLeanStart)
    {
        return;  // a lean starts on each change into sub-phase 0
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    const auto windowAt = seen.windowAt.load(std::memory_order_relaxed);
    if (windowAt == 0 || now - windowAt > std::chrono::milliseconds(kStutterWindow).count())
    {
        seen.windowAt.store(now, std::memory_order_relaxed);
        seen.starts.store(1, std::memory_order_relaxed);
        seen.reported.store(false, std::memory_order_relaxed);
        return;
    }
    const auto starts = seen.starts.fetch_add(1, std::memory_order_relaxed) + 1;
    if (starts >= kStutterStarts && !seen.reported.exchange(true, std::memory_order_relaxed))
    {
        platform::Log(L"edge stop: player {}'s ledge lean restarted {} times in {} s (a stutter; tested as {})", player,
                      starts, kStutterWindow.count(), basis);
    }
}

/// The edge-stop action's Tick: the player's stick and camera basis, the game's identity (the context is suspended for
/// player 2, so the leader test and the buttons stay the game's).
template <std::size_t Index>
std::uintptr_t EdgeStopTick(void* move, void* arg, int player) noexcept
{
    const auto original = reinterpret_cast<TickFn>(g_moves[Index].tick.Original());
    g_counts.edgeTicks[IndexOfPlayer(player)].fetch_add(1, std::memory_order_relaxed);
    std::uintptr_t result      = 0;
    unsigned       seatCameras = 0;
    {
        const InContext context(player, false);
        const Suspended suspended;
        result      = original(move, arg);
        seatCameras = t_climb.seatCameras;
    }
    NoteEdgeStop(player, move, seatCameras);  // the caller still holds the action after its Tick
    return result;
}

template <std::size_t Index>
std::uintptr_t MoveTick(void* move, void* arg) noexcept
{
    const auto original = reinterpret_cast<TickFn>(g_moves[Index].tick.Original());
    const int  player   = PlayerOf(move);
    if (player == 0)
    {
        return original(move, arg);
    }
    if (!kMoves[Index].climb)
    {
        return EdgeStopTick<Index>(move, arg, player);
    }
    const auto frame = g_frame.load(std::memory_order_relaxed);
    if (player >= 2)
    {
        g_climbClass[IndexOfPlayer(player)].store(static_cast<int>(Index), std::memory_order_relaxed);
    }
    g_climbFrame[IndexOfPlayer(player)].store(frame, std::memory_order_relaxed);
    g_counts.ticks[IndexOfPlayer(player)].fetch_add(1, std::memory_order_relaxed);
    const InContext context(player, true);
    return original(move, arg);
}

/// Finish runs inside the Tick; its leader-action record would reach player 1's controller with a seat's identity.
template <std::size_t Index>
std::uintptr_t MoveFinish(void* move) noexcept
{
    const auto original = reinterpret_cast<FinishFn>(g_moves[Index].finish.Original());
    const int  seat     = SeatHere();
    if (seat == 0)
    {
        return original(move);
    }
    g_counts.finishOff[static_cast<std::size_t>(seat)].fetch_add(1, std::memory_order_relaxed);
    const Suspended suspended;
    return original(move);
}

/// Another player than `seat` climbs this frame (player 1, or another seat).
bool OtherClimbs(int seat) noexcept
{
    for (int other = 0; other <= players::kSeatsBuilt; ++other)
    {
        if (other != seat && Fresh(g_climbFrame[static_cast<std::size_t>(other)].load(std::memory_order_relaxed)))
        {
            return true;
        }
    }
    return false;
}

/// The climbing wall's hold-select: one global prompt point, so a seat's runs without the seat's identity while
/// another player also climbs (the seat then keeps the game's NPC answer there), and whenever the seat's presses were not
/// recorded this frame (it would read player 1's buttons).
void HoldSelect(void* move) noexcept
{
    const auto original = reinterpret_cast<HoldSelectFn>(g_holdSelectCall.Original());
    const int  seat     = SeatHere();
    if (seat != 0 && !t_climb.suspended &&
        (!g_buttons || !Fresh(g_pressedFrame[static_cast<std::size_t>(seat)].load(std::memory_order_acquire)) ||
         OtherClimbs(seat)))
    {
        g_counts.holdOff[static_cast<std::size_t>(seat)].fetch_add(1, std::memory_order_relaxed);
        const Suspended suspended;
        original(move);
        return;
    }
    original(move);
}

void* CameraGetter() noexcept
{
    const auto original = reinterpret_cast<CameraGetterFn>(g_cameraEntry.Original());
    if (const int seat = SeatHere(); seat != 0)
    {
        const std::uint64_t weak = g_cameraWeak[static_cast<std::size_t>(seat)].load(std::memory_order_relaxed);
        if (void* cameraManager = weak != 0 ? scopes::leader_scope::ObjectAt(&weak) : nullptr)
        {
            g_counts.seatCameras[static_cast<std::size_t>(seat)].fetch_add(1, std::memory_order_relaxed);
            ++t_climb.seatCameras;
            return cameraManager;
        }
    }
    return original();
}

/// The game's key-value getter on the climber's own copy of the owner's key fields: it picks the key, combines digital
/// over analog and releases the key itself.
float KeyValue(const void* owner, void* key) noexcept
{
    const auto original = reinterpret_cast<KeyValueFn>(g_keyEntry.Original());
    const int  player   = t_climb.player;
    if (player == 0 || owner == nullptr || owner != g_owner.load(std::memory_order_relaxed))
    {
        return original(owner, key);
    }
    const auto index = IndexOfPlayer(player);
    if (!Fresh(g_copyFrame[index].load(std::memory_order_acquire)))
    {
        g_counts.staleCopies.fetch_add(1, std::memory_order_relaxed);
        return original(owner, key);
    }
    const auto front = static_cast<std::size_t>(g_front[index].load(std::memory_order_acquire));
    g_counts.keys[index].fetch_add(1, std::memory_order_relaxed);
    return original(g_copies[index][front].bytes, key);
}

/// The pressed test under the hold-select's actions: the seat's own press, recorded this frame. The game's function
/// still runs (it releases the key).
std::uint8_t Pressed(void* key) noexcept
{
    const auto original = reinterpret_cast<PressedFn>(g_pressedEntry.Original());
    const auto seat     = static_cast<std::size_t>(SeatHere());
    if (seat == 0 || t_climb.suspended || key == nullptr || !Fresh(g_pressedFrame[seat].load(std::memory_order_acquire)))
    {
        return original(key);
    }
    const auto name = ReadField<std::uint64_t>(key, 0);
    for (std::size_t i = 0; i < kActionCount; ++i)
    {
        if (name != 0 && name == g_actionNames[i].load(std::memory_order_relaxed))
        {
            original(key);
            g_counts.buttons[seat].fetch_add(1, std::memory_order_relaxed);
            return (g_pressedBits[seat].load(std::memory_order_relaxed) >> i) & 1u;
        }
    }
    return original(key);
}

// --- game thread --------------------------------------------------------------------------------------------------------

/// A seat's this-frame values forgotten (its player out).
void ClearSeat(std::size_t seat) noexcept
{
    g_cameraWeak[seat].store(0, std::memory_order_relaxed);
    g_memberType[seat].store(-1, std::memory_order_relaxed);
    g_copyFrame[seat].store(0, std::memory_order_release);
    g_pressedFrame[seat].store(0, std::memory_order_release);
}

/// Every seat's and player 1's this-frame values forgotten (no seat's player in).
void Clear() noexcept
{
    for (std::size_t seat = 1; seat < kPlayers; ++seat)
    {
        ClearSeat(seat);
    }
    g_owner.store(nullptr, std::memory_order_relaxed);
    g_copyFrame[0].store(0, std::memory_order_release);
}

/// Copies the owner's key fields as pad `controllerId`'s StickScope leaves them into seat `index`'s back buffer (0
/// player 1's) and publishes it.
void CopyKeys(std::size_t index, std::int32_t controllerId, const std::byte* owner, std::uint32_t frame) noexcept
{
    const pad_sticks::StickScope sticks(controllerId);
    if (!sticks.Held())
    {
        return;
    }
    const auto back = static_cast<std::size_t>(1 - g_front[index].load(std::memory_order_relaxed));
    if (!platform::CopyGuarded(g_copies[index][back].bytes + kKeyFields, owner + kKeyFields, kKeyFieldsEnd - kKeyFields))
    {
        return;
    }
    g_front[index].store(static_cast<int>(back), std::memory_order_release);
    g_copyFrame[index].store(frame, std::memory_order_release);
}

/// A seat's presses of the hold-select's actions, by the game's own pressed test in the seat's button view.
void RecordPresses(int seat, std::uint32_t frame) noexcept
{
    const pad_buttons::ButtonScope buttons(seat);
    if (!buttons.Held())
    {
        return;
    }
    std::uint32_t bits = 0;
    for (std::size_t i = 0; i < kActionCount; ++i)
    {
        alignas(8) std::byte key[kKeySize]{};
        g_keyCopy(key, g_actionKeys[i]);  // the game's copy (adds a reference); the pressed test releases it
        if (g_pressedGlobal(key) != 0)
        {
            bits |= 1u << i;
        }
    }
    g_pressedBits[static_cast<std::size_t>(seat)].store(bits, std::memory_order_relaxed);
    g_pressedFrame[static_cast<std::size_t>(seat)].store(frame, std::memory_order_release);
}

void LogChanges() noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const auto s  = static_cast<std::size_t>(seat);
        const bool on = Fresh(g_climbFrame[s].load(std::memory_order_relaxed));
        if (on != g_loggedOn[s])
        {
            g_loggedOn[s]   = on;
            const int index = g_climbClass[s].load(std::memory_order_relaxed);
            const wchar_t* name = players::SeatName(seat);
            try
            {
                platform::LogDebug(L"climb context: {}'s {} {}", name, index >= 0 ? kMoves[index].name : L"climb",
                                   on ? std::format(L"ticks as {}'s ({}'s stick, buttons and camera basis)", name, name)
                                      : std::wstring(L"ended"));
            }
            catch (...)
            {
            }
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (!g_summary.Due(now))
    {
        return;
    }
    // Each seat's counts, and player 1's; the line as the two-player mod wrote it, a part per seat.
    std::array<unsigned, kPlayers> ticks{}, edges{}, asks{}, cams{}, keys{}, presses{}, holds{}, finishes{};
    unsigned                       any = 0;
    for (std::size_t s = 0; s < kPlayers; ++s)
    {
        ticks[s]    = platform::Take(g_counts.ticks[s]);
        edges[s]    = platform::Take(g_counts.edgeTicks[s]);
        keys[s]     = platform::Take(g_counts.keys[s]);
        asks[s]     = platform::Take(g_counts.leaderAsks[s]);
        cams[s]     = platform::Take(g_counts.seatCameras[s]);
        presses[s]  = platform::Take(g_counts.buttons[s]);
        holds[s]    = platform::Take(g_counts.holdOff[s]);
        finishes[s] = platform::Take(g_counts.finishOff[s]);
        any += ticks[s] + edges[s];
    }
    const auto old = platform::Take(g_counts.staleCopies);
    if (any == 0)
    {
        return;
    }
    try
    {
        std::wstring climbs, edgeTicks, inside;
        for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
        {
            const auto     s    = static_cast<std::size_t>(seat);
            const wchar_t* name = players::SeatName(seat);
            climbs += std::format(L"{}'s climb ticks {}, ", name, ticks[s]);
            edgeTicks += std::format(L"{} {}, ", name, edges[s]);
            inside += std::format(L"inside {}'s: leader tests asked {}, {}'s camera manager {}, keys from pad {} {}, "
                                  L"hold-select presses {}, hold-selects without {}'s identity {}, finishes without it "
                                  L"{}; ",
                                  name, asks[s], name, cams[s], seat + 1, keys[s], presses[s], name, holds[s],
                                  finishes[s]);
        }
        platform::LogDebug(L"climb context ({} s): {}player 1's {}; edge-stop ticks {}player 1 {}; {}keys from pad 1 in "
                           L"player 1's {}; no fresh copy {}",
                           kSummaryEvery.count(), climbs, ticks[0], edgeTicks, edges[0], inside, keys[0], old);
    }
    catch (...)
    {
    }
}

template <std::size_t Index>
std::wstring PlaceMove(const platform::GameImage& image, bool holdSelect)
{
    const MoveClass& cls  = kMoves[Index];
    MoveLive&        live = g_moves[Index];
    if (cls.holdSelect && !holdSelect)
    {
        return std::wstring(cls.name) + L" NO (its hold-select call differs)";
    }
    // Finish first: a Tick in context without it would let player 2's finish record into player 1's controller.
    const bool finish =
        platform::ReplaceSlot(image, cls.finish, reinterpret_cast<std::uintptr_t>(&MoveFinish<Index>), live.finish);
    const bool tick = finish &&
                      platform::ReplaceSlot(image, cls.tick, reinterpret_cast<std::uintptr_t>(&MoveTick<Index>), live.tick);
    if (finish && !tick)
    {
        live.finish.Restore();
    }
    g_ticksPlaced += tick && cls.climb ? 1u : 0u;
    return std::wstring(cls.name) + (tick ? L" yes" : L" NO");
}

template <std::size_t... I>
std::wstring PlaceMoves(const platform::GameImage& image, bool holdSelect, std::index_sequence<I...>)
{
    std::wstring text;
    ((text += (text.empty() ? L"" : L", ") + PlaceMove<I>(image, holdSelect)), ...);
    return text;
}

bool Entry(const platform::GameImage& image, platform::EntryRedirect& redirect, const platform::FunctionFact& fact,
           std::uintptr_t replacement) noexcept
{
    const auto at = platform::FindFunction(image, fact);
    return at &&
           redirect.Install(*at, image.EntryInstruction(fact.address, fact.entryBytes.first(kFirst)), replacement);
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const auto owner = image.Rebase(facts::kSquareInputGlobal, sizeof(void*));
    g_ownerGlobal    = owner ? *owner : 0;
    // The two getters first: outside a context they answer the game's own.
    const bool core = g_ownerGlobal != 0 &&
                      Entry(image, g_cameraEntry, kCameraGetter, reinterpret_cast<std::uintptr_t>(&CameraGetter)) &&
                      Entry(image, g_keyEntry, kKeyValue, reinterpret_cast<std::uintptr_t>(&KeyValue));
    if (!core)
    {
        g_cameraEntry.Restore();
        platform::Log(L"climb context: NOT installed (the input owner, the camera manager getter or the key-value getter "
                      L"differs); player 2 climbs the NPC way, toward player 1");
        return;
    }
    const auto pressedGlobal = platform::FindFunction(image, kPressedGlobal);
    const auto keyCopy       = platform::FindFunction(image, kInputKeyCopy);
    bool       keys          = pressedGlobal && keyCopy;
    for (std::size_t i = 0; keys && i < kActionCount; ++i)
    {
        const auto at  = image.Rebase(kActionKeys[i], kKeySize);
        g_actionKeys[i] = at ? reinterpret_cast<const void*>(*at) : nullptr;
        keys           = g_actionKeys[i] != nullptr;
    }
    if (keys)
    {
        g_pressedGlobal = reinterpret_cast<PressedFn>(*pressedGlobal);
        g_keyCopy       = reinterpret_cast<KeyCopyFn>(*keyCopy);
        g_buttons       = Entry(image, g_pressedEntry, kPressedImpl, reinterpret_cast<std::uintptr_t>(&Pressed));
    }
    const auto site       = image.Rebase(kHoldSelectCall, 5);
    const auto target     = image.Rebase(kHoldSelect);
    const bool holdSelect = site && target &&
                            g_holdSelectCall.Install(*site, *target, reinterpret_cast<std::uintptr_t>(&HoldSelect));
    g_installed           = true;
    try
    {
        platform::Log(L"climb context: {}; player 2's hold-select presses {}",
                      PlaceMoves(image, holdSelect, std::make_index_sequence<kMoveCount>{}),
                      g_buttons ? L"from pad 2" : L"NOT read (player 2's hold-select keeps the NPC answer)");
    }
    catch (...)
    {
        platform::Log(L"climb context: installed (the per-class line could not be written)");
    }
}

void Remove() noexcept
{
    g_installed   = false;
    g_ticksPlaced = 0;
    Clear();
    for (MoveLive& live : g_moves)
    {
        if (live.tick.Original() != 0)
        {
            live.tick.Restore();
        }
        if (live.finish.Original() != 0)
        {
            live.finish.Restore();
        }
    }
    if (g_holdSelectCall.IsInstalled())
    {
        g_holdSelectCall.Restore();
    }
    for (platform::EntryRedirect* entry : {&g_pressedEntry, &g_keyEntry, &g_cameraEntry})
    {
        if (entry->IsInstalled())
        {
            entry->Restore();
        }
    }
}

void Tick() noexcept
{
    if (!g_installed || !players::generation::OnGameThread())
    {
        return;
    }
    auto frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
    if (frame == 0)
    {
        frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;  // 0 means "no stamp"
    }
    void* one = players::generation::ControllerOf(0);
    // The seats whose player is in with a member (seat order); the others' values forgotten.
    std::array<bool, kPlayers> in{};
    bool                       any = false;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const auto s = static_cast<std::size_t>(seat);
        in[s] = one != nullptr && players::generation::Active(seat) && players::generation::MemberOf(seat) != nullptr &&
                players::generation::ControllerOf(seat) != nullptr;
        any   = any || in[s];
    }
    if (!any)
    {
        Clear();
        LogChanges();
        return;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const auto s = static_cast<std::size_t>(seat);
        if (!in[s])
        {
            ClearSeat(s);
            continue;
        }
        const auto type = session::party_member::PlayerTypeOf(players::generation::MemberOf(seat));
        g_cameraWeak[s].store(scopes::leader_scope::WeakOf(ReadPointer(players::generation::ControllerOf(seat),
                                                               facts::kControllerCameraManager)),
                              std::memory_order_relaxed);
        g_memberType[s].store(type ? *type : -1, std::memory_order_relaxed);
    }
    const auto* owner = static_cast<const std::byte*>(ReadPointer(reinterpret_cast<const void*>(g_ownerGlobal), 0));
    g_owner.store(owner, std::memory_order_relaxed);
    if (owner != nullptr && pad_sticks::Enabled())
    {
        for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
        {
            if (in[static_cast<std::size_t>(seat)])
            {
                CopyKeys(static_cast<std::size_t>(seat), pad_sticks::ControllerIdOf(players::generation::ControllerOf(seat)),
                         owner, frame);
            }
        }
        if (pad_sticks::PartActs(pad_sticks::kPartPlayerOneMove))
        {
            CopyKeys(0, pad_sticks::ControllerIdOf(one), owner, frame);
        }
        if (g_buttons)
        {
            for (std::size_t i = 0; i < kActionCount; ++i)
            {
                g_actionNames[i].store(ReadField<std::uint64_t>(g_actionKeys[i], 0), std::memory_order_relaxed);
            }
            for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
            {
                if (in[static_cast<std::size_t>(seat)])
                {
                    RecordPresses(seat, frame);
                }
            }
        }
    }
    LogChanges();
}

int LeaderTypeHere() noexcept
{
    const int seat = SeatHere();
    if (seat == 0 || t_climb.suspended)
    {
        return -1;
    }
    g_counts.leaderAsks[static_cast<std::size_t>(seat)].fetch_add(1, std::memory_order_relaxed);
    return g_memberType[static_cast<std::size_t>(seat)].load(std::memory_order_relaxed);
}

int PlayerHere() noexcept
{
    return t_climb.climb ? t_climb.player : 0;
}

bool TicksPlaced() noexcept
{
    return g_ticksPlaced == kClimbCount;
}

bool WithSeatCamera(int seat, void (*call)(void*), void* context) noexcept
{
    if (!players::SeatBuilt(seat))
    {
        return false;
    }
    const std::uint64_t weak = g_cameraWeak[static_cast<std::size_t>(seat)].load(std::memory_order_relaxed);
    if (call == nullptr || !g_cameraEntry.IsInstalled() || weak == 0 || scopes::leader_scope::ObjectAt(&weak) == nullptr)
    {
        return false;
    }
    const InContext inContext(seat + 1, true);
    const Suspended suspended;
    call(context);
    return true;
}

}  // namespace lss::input::climb_context
