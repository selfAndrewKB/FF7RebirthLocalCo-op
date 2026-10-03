#include "player_two/ride_probe.h"

#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "player_two/charapop_facts.h"
#include "player_two/chocobo_ride.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/object_fields.h"
#include "player_two/pad_buttons.h"
#include "player_two/pad_sticks.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

namespace lss::player_two::ride_probe {

namespace {

using Clock = std::chrono::steady_clock;

// RE-p2-chocobo-sprint.md §1, §5 (this build).
constexpr std::uint32_t kActiveVehicle = 0x5B8;  // AEndPlayerController::ActiveVehicleCoordinator
constexpr std::uint32_t kCoordBody     = 0x30;
constexpr std::uint32_t kGait          = 0x1A20;  // u8: 0 walk, 1 run, 2 dash
constexpr std::uint32_t kGaitSpeed     = 0x1A18;  // float, approaches the gait's TargetSpeed
constexpr std::uint32_t kSpeedRatio    = 0x1928;  // float
constexpr std::uint32_t kAppliedLevel  = 0x1910;  // u8: the root-motion scale level applied (4 none)
constexpr std::uint32_t kRidden        = 0x1924;  // u8
constexpr std::uint32_t kMoveHelper    = 0x1400;  // pointer; its +0x160 holds the anim instance as a weak pointer
constexpr std::uint32_t kHelperAnim    = 0x160;
constexpr std::uint32_t kAnimWalkType  = 0x498;  // u8: the minimum of the walk-type map's values (5 = dash)
constexpr std::uint32_t kAnimSprint    = 0x3E4;  // u8 [ASSUMPTION: the sprint flag]
constexpr std::uint32_t kAnimMapData   = 0x4A0;  // TSet<{u8 key, u8 value}>: data, num, inline allocation bits
constexpr std::uint32_t kAnimMapNum    = 0x4A8;
constexpr std::uint32_t kAnimMapBits   = 0x4B0;
constexpr std::uint32_t kAnimMapFree   = 0x4D4;
constexpr std::uint32_t kAnimMapEntry  = 12;
constexpr std::int32_t  kInlineBits    = 128;
constexpr std::uint32_t kAnimSpeedScale = 0x10CC;  // float
// The controller's move-type layers: move type, dash type and flag per layer, and the count.
constexpr std::uint32_t kLayerMove  = 0x890;
constexpr std::uint32_t kLayerDash  = 0x8A4;
constexpr std::uint32_t kLayerFlags = 0x8B8;
constexpr std::uint32_t kLayerCount = 0x8C4;
constexpr std::size_t   kLayers     = 10;
constexpr std::uint32_t kRootGameMode = 0x1B8;    // [[game root]+0x1B8]+0x3D558: the game mode byte (3/4 free path)
constexpr std::uint32_t kGameModeByte = 0x3D558;

// RE-p2-chocobo-homing.md §6 (this build).
constexpr std::uint32_t kAiPawn       = 0x258;  // AController::Pawn
constexpr std::uint32_t kAiPauseMask  = 0x460;  // u32: bit 0 SYSTEM (the possession swap's unpossess)
constexpr std::uint32_t kAiWeakChar   = 0x464;
constexpr std::uint32_t kAiWeakPawn   = 0x46C;
constexpr std::uint32_t kAiMoveComp   = 0x528;  // AEndAIController::AIMoveComponent
constexpr std::uint32_t kAiRideLatch  = 0xAD8;  // u8 (and +0xAD9)
constexpr std::uint32_t kAiBrainIndex = 0x520;
constexpr std::uint32_t kFollowTarget = 0x358;  // the follow state's weak target
constexpr std::uint32_t kFollowRange  = 0x344;  // float, teleport distance
constexpr std::uint32_t kFollowWarp   = 0x39D;  // u8, teleport pending
constexpr std::uint32_t kFollowBusy   = 0x400;  // u8, request in progress
constexpr std::uint32_t kMoveStatus   = 0x780;  // u8 x2
constexpr std::uint32_t kMoveVelocity = 0x758;  // float x4
constexpr std::uint32_t kHolder       = 0x638;  // the control holder: +0x18 the move request, +0x24 normalised
constexpr std::uint32_t kHolderMove   = 0x18;

constexpr auto kTraceFor    = std::chrono::seconds(8);
constexpr auto kTraceEvery  = std::chrono::milliseconds(100);
constexpr auto kAfterEvery  = std::chrono::milliseconds(250);
constexpr auto kSprintEvery = std::chrono::milliseconds(250);
constexpr auto kHeartbeat   = std::chrono::seconds(10);
constexpr int  kMaxLines    = 2500;

std::uintptr_t g_gameRoot = 0, g_input = 0;
bool           g_installed = false;

// Game thread.
int               g_lines = 0;
const void*       g_driven = nullptr;
Clock::time_point g_traceUntil{}, g_lastHoming{}, g_lastHomingLogged{}, g_lastSprint{}, g_lastSprintLogged{};
float             g_lastAt[3]{};
bool              g_lastAtValid = false;
std::wstring      g_lastHomingKey, g_lastSprintKey;

template <class... A>
void Line(std::wformat_string<A...> format, A&&... args) noexcept
{
    if (g_lines >= kMaxLines)
    {
        return;
    }
    ++g_lines;
    try
    {
        platform::WriteLine(L"ride probe: " + std::format(format, std::forward<A>(args)...));
    }
    catch (...)
    {
    }
    if (g_lines == kMaxLines)
    {
        platform::WriteLine(L"ride probe: line limit reached");
    }
}

const std::byte* At(const void* object, std::uint32_t offset) noexcept
{
    return object == nullptr ? nullptr : static_cast<const std::byte*>(object) + offset;
}

template <class T>
T Field(const void* object, std::uint32_t offset) noexcept
{
    return object == nullptr ? T{} : ReadField<T>(object, offset);
}

std::wstring Who(const void* controller)
{
    if (controller == nullptr)
    {
        return L"none";
    }
    if (controller == generation::PlayerOneController())
    {
        return L"PC1";
    }
    if (controller == generation::PlayerTwoController())
    {
        return L"PC2";
    }
    return std::format(L"{}", controller);
}

const void* AnimOf(const void* chocobo) noexcept
{
    const void* helper = ReadPointer(chocobo, kMoveHelper);
    return helper == nullptr ? nullptr : leader_scope::ObjectAt(At(helper, kHelperAnim));
}

/// The walk-type map's live entries as "key:value ...".
std::wstring WalkTypeMap(const void* anim)
{
    const void*  data = ReadPointer(anim, kAnimMapData);
    const auto   num  = Field<std::int32_t>(anim, kAnimMapNum);
    const auto   free = Field<std::int32_t>(anim, kAnimMapFree);
    if (data == nullptr || num <= 0)
    {
        return L"empty";
    }
    if (num > kInlineBits)
    {
        return std::format(L"{} entries (not listed)", num);
    }
    std::array<std::uint32_t, 4> bits{};
    platform::CopyGuarded(bits.data(), At(anim, kAnimMapBits), sizeof(bits));
    std::wstring text;
    for (std::int32_t i = 0; i < num; ++i)
    {
        if ((bits[static_cast<std::size_t>(i) / 32] >> (i % 32) & 1u) == 0)
        {
            continue;
        }
        const auto key   = Field<std::uint8_t>(data, static_cast<std::uint32_t>(i) * kAnimMapEntry);
        const auto value = Field<std::uint8_t>(data, static_cast<std::uint32_t>(i) * kAnimMapEntry + 1);
        text += std::format(L"{}{}:{}", text.empty() ? L"" : L" ", key, value);
    }
    if (free > 0)
    {
        text += std::format(L" ({} free)", free);
    }
    return text.empty() ? std::wstring(L"none live") : text;
}

std::wstring ChocoboText(const void* chocobo)
{
    if (chocobo == nullptr)
    {
        return L"none";
    }
    const void* anim = AnimOf(chocobo);
    return std::format(L"gait {} speed {:.0f} ratio {:.2f} level {} ridden {}; anim walk type {} (+0x3E4 {}) scale {:.2f} "
                       L"map [{}]",
                       Field<std::uint8_t>(chocobo, kGait), Field<float>(chocobo, kGaitSpeed),
                       Field<float>(chocobo, kSpeedRatio), Field<std::uint8_t>(chocobo, kAppliedLevel),
                       Field<std::uint8_t>(chocobo, kRidden), Field<std::uint8_t>(anim, kAnimWalkType),
                       Field<std::uint8_t>(anim, kAnimSprint), Field<float>(anim, kAnimSpeedScale), WalkTypeMap(anim));
}

std::wstring LayersText(const void* controller)
{
    if (controller == nullptr)
    {
        return L"none";
    }
    std::array<std::uint8_t, kLayers> move{}, dash{}, flags{};
    platform::CopyGuarded(move.data(), At(controller, kLayerMove), kLayers);
    platform::CopyGuarded(dash.data(), At(controller, kLayerDash), kLayers);
    platform::CopyGuarded(flags.data(), At(controller, kLayerFlags), kLayers);
    std::wstring text;
    for (std::size_t i = 0; i < kLayers; ++i)
    {
        text += std::format(L"{}{}/{}/{}", i == 0 ? L"" : L" ", move[i], dash[i], flags[i]);
    }
    return text + std::format(L" (count {})", Field<std::int32_t>(controller, kLayerCount));
}

/// Sprint: logged when the gait, walk types, maps or layers change (speeds rounded out), at most four times a second.
void Sprint(const void* twoChocobo, Clock::time_point now)
{
    if (now - g_lastSprint < kSprintEvery)
    {
        return;
    }
    g_lastSprint     = now;
    void* playerOne  = generation::PlayerOneController();
    void* playerTwo  = generation::PlayerTwoController();
    const void* oneChocobo = ReadPointer(ReadPointer(playerOne, kActiveVehicle), kCoordBody);
    const void* root       = ReadPointer(reinterpret_cast<const void*>(g_gameRoot), 0);
    const auto  mode       = Field<std::uint8_t>(ReadPointer(root, kRootGameMode), kGameModeByte);
    const auto  key = [](const void* c) {
        const void* anim = AnimOf(c);
        return c == nullptr ? std::wstring(L"-")
                            : std::format(L"{}/{}/{}/{}", Field<std::uint8_t>(c, kGait), Field<std::uint8_t>(c, kAppliedLevel),
                                          Field<std::uint8_t>(anim, kAnimWalkType), WalkTypeMap(anim));
    };
    std::wstring changed = key(oneChocobo) + L"|" + key(twoChocobo) + L"|" + LayersText(playerOne) + L"|" +
                           LayersText(playerTwo) + std::format(L"|{}", mode);
    if (changed == g_lastSprintKey && now - g_lastSprintLogged < kHeartbeat)
    {
        return;
    }
    g_lastSprintKey    = std::move(changed);
    g_lastSprintLogged = now;
    Line(L"sprint: player 1's chocobo {}", ChocoboText(oneChocobo));
    Line(L"sprint: player 2's chocobo {}", ChocoboText(twoChocobo));
    Line(L"sprint: move-type layers (move/dash/flag per layer) PC1 [{}]; PC2 [{}]; game mode {}", LayersText(playerOne),
         LayersText(playerTwo), mode);
}

/// Pad 2's own sticks, read through the game's merged floats inside a pad-2 stick scope.
bool PadTwoStick(float (&out)[2]) noexcept
{
    const std::int32_t id = pad_sticks::ControllerIdOf(generation::PlayerTwoController());
    const void* input     = ReadPointer(reinterpret_cast<const void*>(g_input), 0);
    if (id < 0 || input == nullptr)
    {
        return false;
    }
    pad_sticks::StickScope scope(id);
    return scope.Held() && platform::CopyGuarded(out, At(input, facts::kInputMergedSticks), sizeof(out));
}

std::wstring AiText(const void* chocobo)
{
    const void* ai    = ReadPointer(chocobo, facts::kCharacterRetainedAi);
    const void* brain = ReadPointer(ai, facts::kAiActiveBrain);
    const void* state = ReadPointer(brain, facts::kBrainState);
    const void* move  = ReadPointer(ai, kAiMoveComp);
    float velocity[4]{};
    platform::CopyGuarded(velocity, At(move, kMoveVelocity), sizeof(velocity));
    const void* target = state == nullptr ? nullptr : leader_scope::ObjectAt(At(state, kFollowTarget));
    const void* oneBody = ReadPointer(ReadPointer(generation::PlayerOneController(), kActiveVehicle), kCoordBody);
    const auto  names   = [ai, chocobo](std::uint32_t field) {
        const void* named = ai == nullptr ? nullptr : leader_scope::ObjectAt(At(ai, field));
        return named == nullptr ? L"none" : named == chocobo ? L"the chocobo" : L"other";
    };
    const void* pawn = ReadPointer(ai, kAiPawn);
    return std::format(L"AI {} (vtable {:#x}; is the chocobo's controller {}): pause mask {:#x}, pawn {}, weak pawn {}, weak "
                       L"character {}, riding latch {}/{}; brain index {} state type {} (vtable {:#x}; follow target {}, "
                       L"teleport range {:.0f}, pending {}, busy {}); move component {} status {}/{} velocity ({:.0f}, {:.0f}, "
                       L"{:.0f}, {:.0f})",
                       ai, VTableOf(ai), ai != nullptr && ai == ReadPointer(chocobo, facts::kPawnController) ? L"yes" : L"no",
                       Field<std::uint32_t>(ai, kAiPauseMask),
                       pawn == chocobo ? L"the chocobo" : pawn != nullptr ? L"other" : L"none", names(kAiWeakPawn),
                       names(kAiWeakChar), Field<std::uint8_t>(ai, kAiRideLatch), Field<std::uint8_t>(ai, kAiRideLatch + 1),
                       Field<std::int32_t>(ai, kAiBrainIndex), Field<std::uint8_t>(state, facts::kAiStateType), VTableOf(state),
                       target == nullptr ? L"none" : target == oneBody ? L"player 1's chocobo" : target == chocobo ? L"itself" : L"other",
                       Field<float>(state, kFollowRange), Field<std::uint8_t>(state, kFollowWarp),
                       Field<std::uint8_t>(state, kFollowBusy), move, Field<std::uint8_t>(move, kMoveStatus),
                       Field<std::uint8_t>(move, kMoveStatus + 1), velocity[0], velocity[1], velocity[2], velocity[3]);
}

/// Homing: ten lines a second for 8 s from the boarding, then on change (rounded), at most four a second.
void Homing(const void* chocobo, Clock::time_point now)
{
    const bool tracing = now < g_traceUntil;
    if (now - g_lastHoming < (tracing ? kTraceEvery : kAfterEvery))
    {
        return;
    }
    const float seconds = std::chrono::duration<float>(now - g_lastHoming).count();
    g_lastHoming        = now;
    float at[3]{}, one[3]{};
    const void* oneBody = ReadPointer(ReadPointer(generation::PlayerOneController(), kActiveVehicle), kCoordBody);
    const bool  read    = charapop::ActorLocation(const_cast<void*>(chocobo), at);
    const bool  oneRead = oneBody != nullptr && charapop::ActorLocation(const_cast<void*>(oneBody), one);
    const float speed   = read && g_lastAtValid && seconds > 0.0f ? charapop::Distance(at, g_lastAt) / 100.0f / seconds : -1.0f;
    const float gap     = read && oneRead ? charapop::Distance(at, one) / 100.0f : -1.0f;
    // How well the chocobo's movement points at player 1's: cos of the angle between its step and the way to player 1's.
    float toward = 0.0f;
    if (read && oneRead && g_lastAtValid)
    {
        const float sx = at[0] - g_lastAt[0], sy = at[1] - g_lastAt[1];
        const float tx = one[0] - g_lastAt[0], ty = one[1] - g_lastAt[1];
        const float ls = std::hypot(sx, sy), lt = std::hypot(tx, ty);
        toward = ls > 1.0f && lt > 1.0f ? (sx * tx + sy * ty) / (ls * lt) : 0.0f;
    }
    for (int i = 0; i < 3; ++i)
    {
        g_lastAt[i] = at[i];
    }
    g_lastAtValid = read;
    float stick[2]{};
    const bool  hasStick = PadTwoStick(stick);
    float request[6]{};
    platform::CopyGuarded(request, At(ReadPointer(chocobo, kHolder), kHolderMove), sizeof(request));
    const void* ai = ReadPointer(chocobo, facts::kCharacterRetainedAi);
    const std::wstring key = std::format(L"{:#x}/{}/{}/{}/{}", Field<std::uint32_t>(ai, kAiPauseMask),
                                         ReadPointer(ai, kAiPawn) != nullptr, Field<std::uint8_t>(ai, kAiRideLatch),
                                         static_cast<int>(speed + 0.5f), hasStick && std::hypot(stick[0], stick[1]) > 0.25f);
    if (!tracing && key == g_lastHomingKey && now - g_lastHomingLogged < kHeartbeat)
    {
        return;
    }
    g_lastHomingKey    = key;
    g_lastHomingLogged = now;
    try
    {
        Line(L"homing{}: player 2's chocobo {:.1f} m/s, {:.1f} m from player 1's, step toward player 1's {:+.2f}; pad 2 stick "
             L"({:.2f}, {:.2f}){}; move request ({:.2f}, {:.2f}, {:.2f}) normalised ({:.2f}, {:.2f}, {:.2f}); controller {}; {}",
             tracing ? L" (start)" : L"", speed, gap, toward, stick[0], stick[1], hasStick ? L"" : L" (unread)", request[0],
             request[1], request[2], request[3], request[4], request[5],
             Who(ReadPointer(chocobo, facts::kCharacterEffectiveController)), AiText(chocobo));
    }
    catch (...)
    {
    }
}

// The chocobo move state's input memory (RE-p2-chocobo-input.md §1, §6).
constexpr std::uint32_t kStStick      = 0x10;   // f32 x2, the stored stick (0.01 hysteresis)
constexpr std::uint32_t kStDashLatch  = 0x2C;   // u8, set by Dash held, cleared on the stop path
constexpr std::uint32_t kStAccelPrev  = 0x2D;   // u8, the previous tick's Accel (the edge memory)
constexpr std::uint32_t kStRunTimer   = 0x34;   // f32, DashToRunTime
constexpr std::uint32_t kStRaiseHold  = 0x398;  // f32
constexpr std::uint32_t kStSkipTimer  = 0x39C;  // f32, the whole tick is skipped while > 0
constexpr std::uint32_t kStRaiseSkip  = 0x3AC;  // u8
constexpr std::uint32_t kStDashPrev   = 0x3AD;  // u8, the previous tick's Dash

struct Slot11Before
{
    std::uint8_t accelPrev = 0, dashLatch = 0, dashPrev = 0, gait = 0;
    float        runTimer  = 0.0f;
    std::int32_t accel = -1, dash = -1;
};
Slot11Before     g_before{};
std::wstring     g_lastSlot11Key;
Clock::time_point g_lastSlot11{};

}  // namespace

void Slot11(const void* state, bool after) noexcept
{
    if (!g_installed || state == nullptr)
    {
        return;
    }
    const void* chocobo = ReadPointer(state, facts::kMoveStateCharacter);
    Slot11Before now;
    now.accelPrev = Field<std::uint8_t>(state, kStAccelPrev);
    now.dashLatch = Field<std::uint8_t>(state, kStDashLatch);
    now.dashPrev  = Field<std::uint8_t>(state, kStDashPrev);
    now.gait      = Field<std::uint8_t>(chocobo, kGait);
    now.runTimer  = Field<float>(state, kStRunTimer);
    now.accel     = pad_buttons::TracedHeldNow(0);
    now.dash      = pad_buttons::TracedHeldNow(1);
    if (!after)
    {
        g_before = now;
        return;
    }
    try
    {
        // Logged when the buttons, latches or gait change, or the run timer crosses zero.
        const std::wstring key = std::format(L"{}{}/{}{}{}/{}{}/{}{}", g_before.accel, g_before.dash, now.accelPrev,
                                             now.dashLatch, now.dashPrev, g_before.gait, now.gait, g_before.runTimer > 0.0f,
                                             now.runTimer > 0.0f);
        const auto clock = Clock::now();
        if (key == g_lastSlot11Key && clock - g_lastSlot11 < kHeartbeat)
        {
            return;
        }
        g_lastSlot11Key = key;
        g_lastSlot11    = clock;
        float stick[2]{};
        platform::CopyGuarded(stick, At(state, kStStick), sizeof(stick));
        Line(L"slot 11: Accel held {} Dash held {} (as the tick reads them); edge memory Accel {} -> {}, Dash {} -> {}; dash "
             L"latch {} -> {}; run timer {:.2f} -> {:.2f}; gait {} -> {}; raise hold {:.2f} skip {}; skip timer {:.2f}; "
             L"stored stick ({:.2f}, {:.2f}); speed {:.0f}",
             g_before.accel, g_before.dash, g_before.accelPrev, now.accelPrev, g_before.dashPrev, now.dashPrev,
             g_before.dashLatch, now.dashLatch, g_before.runTimer, now.runTimer, g_before.gait, now.gait,
             Field<float>(state, kStRaiseHold), Field<std::uint8_t>(state, kStRaiseSkip), Field<float>(state, kStSkipTimer),
             stick[0], stick[1], Field<float>(chocobo, kGaitSpeed));
    }
    catch (...)
    {
    }
}

bool Install(const platform::GameImage& image) noexcept
{
    const auto root  = image.Rebase(charapop::kGameRoot, sizeof(void*));
    const auto input = image.Rebase(facts::kSquareInputGlobal, sizeof(void*));
    g_gameRoot  = root ? *root : 0;
    g_input     = input ? *input : 0;
    g_installed = g_gameRoot != 0 && g_input != 0;
    platform::Log(L"ride probe: {}", g_installed ? L"installed (player 2's chocobo ride: sprint, homing; read-only)"
                                                 : L"UNAVAILABLE (a check failed)");
    return g_installed;
}

void Boarding(const wchar_t* step, const void* chocobo) noexcept
{
    if (!g_installed || chocobo == nullptr || !generation::OnGameThread())
    {
        return;
    }
    try
    {
        Line(L"boarding ({}): player 2's chocobo controller {}; {}", step,
             Who(ReadPointer(chocobo, facts::kCharacterEffectiveController)), AiText(chocobo));
    }
    catch (...)
    {
    }
}

void Tick() noexcept
{
    if (!g_installed || !generation::OnGameThread() || !generation::Active())
    {
        g_driven = nullptr;
        return;
    }
    const void* chocobo = chocobo_ride::DrivenChocobo();
    const auto  now     = Clock::now();
    if (chocobo != g_driven)
    {
        g_driven      = chocobo;
        g_lastAtValid = false;
        g_lastHomingKey.clear();
        g_lastSprintKey.clear();
        if (chocobo != nullptr)
        {
            g_traceUntil = now + kTraceFor;
            Boarding(L"first tick driven", chocobo);
        }
    }
    if (chocobo == nullptr)
    {
        return;
    }
    try
    {
        Homing(chocobo, now);
        Sprint(chocobo, now);
    }
    catch (...)
    {
    }
}

}  // namespace lss::player_two::ride_probe
