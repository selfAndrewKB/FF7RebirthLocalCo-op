#include "player_two/climb_context.h"

#include "facts/controller.h"
#include "platform/angles.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/every.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/object_fields.h"
#include "player_two/pad_buttons.h"
#include "player_two/pad_sticks.h"
#include "player_two/party_member.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

namespace lss::player_two::climb_context {

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

struct Context
{
    int      player    = 0;      // 0 none, 1 or 2: whose move this thread is ticking
    bool     suspended = false;  // player 2's identity (leader test, buttons) off for this call
    bool     climb     = false;  // the move is a climb (PlayerHere answers only for climbs)
    unsigned cameraTwo = 0;      // times the camera manager getter answered player 2's in this context
};
thread_local Context t_climb;

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

std::atomic<std::uint32_t> g_frame{0};  // engine ticks counted by Tick; 0 = none yet
std::atomic<std::uint64_t> g_cameraTwoWeak{0};  // player 2's camera manager
std::atomic<int>           g_memberType{-1};
std::atomic<const void*>   g_owner{nullptr};  // the input owner (the key-value getter's first argument)

// Each player's copy of the owner's key fields, double-buffered: the game thread writes the back buffer, then publishes
// it (front, then the frame stamp, release); a reader takes the stamp (acquire), then the front.
struct alignas(16) OwnerCopy
{
    std::byte bytes[kKeyFieldsEnd + 0xC]{};
};
std::array<std::array<OwnerCopy, 2>, 2>   g_copies{};  // [player - 1][buffer]
std::array<std::atomic<int>, 2>           g_front{};
std::array<std::atomic<std::uint32_t>, 2> g_copyFrame{};

// Player 2's presses of the hold-select's actions this frame (bit per action), and the actions' FNames.
std::array<std::atomic<std::uint64_t>, kActionCount> g_actionNames{};
std::atomic<std::uint32_t>                           g_pressedBits{0};
std::atomic<std::uint32_t>                           g_pressedFrame{0};

// Written by the Ticks (any thread): the last frame each player's climb ticked, player 2's class.
std::atomic<std::uint32_t> g_playerOneClimbFrame{0};
std::atomic<std::uint32_t> g_playerTwoClimbFrame{0};
std::atomic<int>           g_playerTwoClass{-1};

struct Counts
{
    std::atomic<unsigned> ticks[2]{};      // contexts entered, per player
    std::atomic<unsigned> leaderAsks{0};   // leader tests asked inside player 2's context
    std::atomic<unsigned> cameraTwo{0};    // camera manager getter answered player 2's
    std::atomic<unsigned> keys[2]{};       // key values read from a player's own copy
    std::atomic<unsigned> buttons{0};      // hold-select actions answered with player 2's press
    std::atomic<unsigned> holdOff{0};      // hold-selects run without player 2's identity
    std::atomic<unsigned> finishOff{0};    // finishes run without player 2's identity
    std::atomic<unsigned> staleCopies{0};  // a context found no fresh copy (the merged value read instead)
    std::atomic<unsigned> edgeTicks[2]{};  // edge-stop action ticks, per player
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
std::array<EdgeSeen, 2> g_edge;

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
bool                             g_buttons       = false;  // player 2's presses recorded and answered
unsigned                         g_ticksPlaced   = 0;      // climb Tick slots placed (of kClimbCount)

// Game thread only.
bool                                  g_loggedOn = false;
platform::Every g_summary{kSummaryEvery};

bool Fresh(std::uint32_t stamp) noexcept
{
    return stamp != 0 && g_frame.load(std::memory_order_relaxed) - stamp <= kFreshFrames;
}

/// The player whose character `move` belongs to: 2 for player 2's member, 1 for player 1's pawn, else 0.
int PlayerOf(const void* move) noexcept
{
    const auto weak = ReadField<std::uint64_t>(move, kMoveCharacter);
    if (weak == 0)
    {
        return 0;
    }
    if (weak == generation::MemberWeak())
    {
        return 2;
    }
    return weak == generation::PlayerOnePawnWeak() ? 1 : 0;
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
void NoteEdgeStop(int player, const void* move, unsigned cameraTwo) noexcept
{
    EdgeSeen&      seen   = g_edge[static_cast<std::size_t>(player - 1)];
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
    const wchar_t* basis   = player == 1        ? L"pad 1's stick in player 1's camera"
                             : cameraTwo != 0u   ? L"pad 2's stick in player 2's camera"
                                                 : L"pad 2's stick; no camera read on this tick";
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
    g_counts.edgeTicks[player - 1].fetch_add(1, std::memory_order_relaxed);
    std::uintptr_t result    = 0;
    unsigned       cameraTwo = 0;
    {
        const InContext context(player, false);
        const Suspended suspended;
        result    = original(move, arg);
        cameraTwo = t_climb.cameraTwo;
    }
    NoteEdgeStop(player, move, cameraTwo);  // the caller still holds the action after its Tick
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
    if (player == 2)
    {
        g_playerTwoClass.store(static_cast<int>(Index), std::memory_order_relaxed);
        g_playerTwoClimbFrame.store(frame, std::memory_order_relaxed);
    }
    else
    {
        g_playerOneClimbFrame.store(frame, std::memory_order_relaxed);
    }
    g_counts.ticks[player - 1].fetch_add(1, std::memory_order_relaxed);
    const InContext context(player, true);
    return original(move, arg);
}

/// Finish runs inside the Tick; its leader-action record would reach player 1's controller with player 2's identity.
template <std::size_t Index>
std::uintptr_t MoveFinish(void* move) noexcept
{
    const auto original = reinterpret_cast<FinishFn>(g_moves[Index].finish.Original());
    if (t_climb.player != 2)
    {
        return original(move);
    }
    g_counts.finishOff.fetch_add(1, std::memory_order_relaxed);
    const Suspended suspended;
    return original(move);
}

/// The climbing wall's hold-select: one global prompt point, so player 2's runs without player 2's identity while
/// player 1 also climbs (player 2 then keeps the game's NPC answer there), and whenever player 2's presses were not
/// recorded this frame (it would read player 1's buttons).
void HoldSelect(void* move) noexcept
{
    const auto original = reinterpret_cast<HoldSelectFn>(g_holdSelectCall.Original());
    if (t_climb.player == 2 && !t_climb.suspended &&
        (!g_buttons || !Fresh(g_pressedFrame.load(std::memory_order_acquire)) ||
         Fresh(g_playerOneClimbFrame.load(std::memory_order_relaxed))))
    {
        g_counts.holdOff.fetch_add(1, std::memory_order_relaxed);
        const Suspended suspended;
        original(move);
        return;
    }
    original(move);
}

void* CameraGetter() noexcept
{
    const auto original = reinterpret_cast<CameraGetterFn>(g_cameraEntry.Original());
    if (t_climb.player == 2)
    {
        const std::uint64_t weak = g_cameraTwoWeak.load(std::memory_order_relaxed);
        if (void* two = weak != 0 ? leader_scope::ObjectAt(&weak) : nullptr)
        {
            g_counts.cameraTwo.fetch_add(1, std::memory_order_relaxed);
            ++t_climb.cameraTwo;
            return two;
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
    const auto index = static_cast<std::size_t>(player - 1);
    if (!Fresh(g_copyFrame[index].load(std::memory_order_acquire)))
    {
        g_counts.staleCopies.fetch_add(1, std::memory_order_relaxed);
        return original(owner, key);
    }
    const auto front = static_cast<std::size_t>(g_front[index].load(std::memory_order_acquire));
    g_counts.keys[index].fetch_add(1, std::memory_order_relaxed);
    return original(g_copies[index][front].bytes, key);
}

/// The pressed test under the hold-select's actions: player 2's own press, recorded this frame. The game's function
/// still runs (it releases the key).
std::uint8_t Pressed(void* key) noexcept
{
    const auto original = reinterpret_cast<PressedFn>(g_pressedEntry.Original());
    if (t_climb.player != 2 || t_climb.suspended || key == nullptr ||
        !Fresh(g_pressedFrame.load(std::memory_order_acquire)))
    {
        return original(key);
    }
    const auto name = ReadField<std::uint64_t>(key, 0);
    for (std::size_t i = 0; i < kActionCount; ++i)
    {
        if (name != 0 && name == g_actionNames[i].load(std::memory_order_relaxed))
        {
            original(key);
            g_counts.buttons.fetch_add(1, std::memory_order_relaxed);
            return (g_pressedBits.load(std::memory_order_relaxed) >> i) & 1u;
        }
    }
    return original(key);
}

// --- game thread --------------------------------------------------------------------------------------------------------

void Clear() noexcept
{
    g_cameraTwoWeak.store(0, std::memory_order_relaxed);
    g_memberType.store(-1, std::memory_order_relaxed);
    g_owner.store(nullptr, std::memory_order_relaxed);
    for (auto& stamp : g_copyFrame)
    {
        stamp.store(0, std::memory_order_release);
    }
    g_pressedFrame.store(0, std::memory_order_release);
}

/// Copies the owner's key fields as pad `controllerId`'s StickScope leaves them into player `index + 1`'s back buffer
/// and publishes it.
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

/// Player 2's presses of the hold-select's actions, by the game's own pressed test in player 2's button view.
void RecordPresses(std::uint32_t frame) noexcept
{
    const pad_buttons::ButtonScope buttons;
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
    g_pressedBits.store(bits, std::memory_order_relaxed);
    g_pressedFrame.store(frame, std::memory_order_release);
}

void LogChanges() noexcept
{
    const bool on = Fresh(g_playerTwoClimbFrame.load(std::memory_order_relaxed));
    if (on != g_loggedOn)
    {
        g_loggedOn      = on;
        const int index = g_playerTwoClass.load(std::memory_order_relaxed);
        platform::LogDebug(L"climb context: player 2's {} {}", index >= 0 ? kMoves[index].name : L"climb",
                           on ? L"ticks as player 2's (player 2's stick, buttons and camera basis)" : L"ended");
    }
    const auto now = std::chrono::steady_clock::now();
    if (!g_summary.Due(now))
    {
        return;
    }
    const auto two  = platform::Take(g_counts.ticks[1]);
    const auto one  = platform::Take(g_counts.ticks[0]);
    const auto asks = platform::Take(g_counts.leaderAsks);
    const auto cam  = platform::Take(g_counts.cameraTwo);
    const auto k2   = platform::Take(g_counts.keys[1]);
    const auto k1   = platform::Take(g_counts.keys[0]);
    const auto btn  = platform::Take(g_counts.buttons);
    const auto hold = platform::Take(g_counts.holdOff);
    const auto fin  = platform::Take(g_counts.finishOff);
    const auto old  = platform::Take(g_counts.staleCopies);
    const auto e2   = platform::Take(g_counts.edgeTicks[1]);
    const auto e1   = platform::Take(g_counts.edgeTicks[0]);
    if (two == 0 && one == 0 && e2 == 0 && e1 == 0)
    {
        return;
    }
    platform::LogDebug(L"climb context ({} s): player 2's climb ticks {}, player 1's {}; edge-stop ticks player 2 {}, "
                       L"player 1 {}; inside player 2's: leader tests asked {}, player 2's camera manager {}, keys from "
                       L"pad 2 {}, hold-select presses {}, hold-selects without player 2's identity {}, finishes without "
                       L"it {}; keys from pad 1 in player 1's {}; no fresh copy {}",
                       kSummaryEvery.count(), two, one, e2, e1, asks, cam, k2, btn, hold, fin, k1, old);
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
    const auto at = image.Rebase(fact.address);
    return at && image.BytesMatch(fact.address, fact.entryBytes) &&
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
    if (!g_installed || !generation::OnGameThread())
    {
        return;
    }
    auto frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
    if (frame == 0)
    {
        frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;  // 0 means "no stamp"
    }
    void* member = generation::Member();
    void* one    = generation::PlayerOneController();
    void* two    = generation::PlayerTwoController();
    if (!generation::Active() || member == nullptr || one == nullptr || two == nullptr)
    {
        Clear();
        LogChanges();
        return;
    }
    const auto type = party_member::PlayerTypeOf(member);
    g_cameraTwoWeak.store(leader_scope::WeakOf(ReadPointer(two, facts::kControllerCameraManager)),
                          std::memory_order_relaxed);
    g_memberType.store(type ? *type : -1, std::memory_order_relaxed);
    const auto* owner = static_cast<const std::byte*>(ReadPointer(reinterpret_cast<const void*>(g_ownerGlobal), 0));
    g_owner.store(owner, std::memory_order_relaxed);
    if (owner != nullptr && pad_sticks::Enabled())
    {
        CopyKeys(1, pad_sticks::ControllerIdOf(two), owner, frame);
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
            RecordPresses(frame);
        }
    }
    LogChanges();
}

int LeaderTypeHere() noexcept
{
    if (t_climb.player != 2 || t_climb.suspended)
    {
        return -1;
    }
    g_counts.leaderAsks.fetch_add(1, std::memory_order_relaxed);
    return g_memberType.load(std::memory_order_relaxed);
}

int PlayerHere() noexcept
{
    return t_climb.climb ? t_climb.player : 0;
}

bool TicksPlaced() noexcept
{
    return g_ticksPlaced == kClimbCount;
}

bool WithPlayerTwoCamera(void (*call)(void*), void* context) noexcept
{
    const std::uint64_t weak = g_cameraTwoWeak.load(std::memory_order_relaxed);
    if (call == nullptr || !g_cameraEntry.IsInstalled() || weak == 0 || leader_scope::ObjectAt(&weak) == nullptr)
    {
        return false;
    }
    const InContext inContext(2, true);
    const Suspended suspended;
    call(context);
    return true;
}

}  // namespace lss::player_two::climb_context
