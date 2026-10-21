#include "player_two/target_coin.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "platform/call_redirect.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "player_two/coin_table.h"
#include "player_two/engine_facts.h"
#include "player_two/equal_targeting.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/member_movement.h"
#include "player_two/object_fields.h"
#include "player_two/player_hits.h"
#include "player_two/thread_hold.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace lss::player_two::target_coin {

namespace {

using RC::Unreal::UFunction;
namespace UObjectGlobals = RC::Unreal::UObjectGlobals;

// Facts read from the database (RE-p2-enemy-target-bias.md §16; this build). 1.005: replaced-target-coin.json.

// Where an enemy decides its next action, tossed exactly when the blueprint event is about to run (the game's own
// conditions decide whether it does):
//   - OnActionInvoke: its one call, inside EnemyCtrl_VtAD8_FireOnActionInvoke (the +0xAD8 slot, which skips it under a
//     global gate or for interval type 4; BossBase's +0xAD8 gate calls the same function in mode 1). A call redirect;
//     the call crosses a 16-byte block on this build (1.005: inside one), so it is placed at on_unreal_init.
//   - OnActionInterruptInvoke / OnActionTopPriorityInvoke: the +0xAE0 / +0xB18 slots of both controllers. EnemyBase's
//     hold the events' stubs, which fire only with a non-null second argument; BossBase's hold gates that call the
//     same stubs only when the controller's mode byte is 1. Each takes (ctrl, ptr), passed on.
constexpr std::uintptr_t kActionCall = 0x140CBA04E;  // call EnemyAICtrl_Event_OnActionInvoke(ctrl, u8 type, ptr)
constexpr std::uintptr_t kAction     = 0x140CBA078;

constexpr std::uint8_t kEventBytes[]         = {0x48, 0x85, 0xD2, 0x74, 0x48, 0x48, 0x89, 0x5C,
                                                0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr std::uint8_t kBossInterruptBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xD2, 0x98,
                                                0x7F, 0x00, 0x48, 0x2B, 0xE0, 0x80, 0xB9, 0x1C};
constexpr std::uint8_t kBossTopBytes[]       = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x7E, 0x6D,
                                                0x8B, 0x00, 0x48, 0x2B, 0xE0, 0x80, 0xB9, 0x1C};

constexpr std::size_t        kDecisions = 4;
constexpr std::size_t        kFirstBoss = 2;  // slots 2..3 are BossBase's
constexpr platform::SlotFact kDecisionSlots[kDecisions] = {
    {L"the EnemyBase controller's OnActionInterruptInvoke slot (+0xAE0)", 0x1457DC770,
     {L"EnemyAICtrl_Event_OnActionInterruptInvoke", 0x1418B5674, kEventBytes}},
    {L"the EnemyBase controller's OnActionTopPriorityInvoke slot (+0xB18)", 0x1457DC7A8,
     {L"EnemyAICtrl_Event_OnActionTopPriorityInvoke", 0x1417F81C8, kEventBytes}},
    {L"the BossBase controller's OnActionInterruptInvoke slot (+0xAE0)", 0x1457DBAE0,
     {L"BossCtrl_VtAE0_Interrupt_IfMode1", 0x1418B5654, kBossInterruptBytes}},
    {L"the BossBase controller's OnActionTopPriorityInvoke slot (+0xB18)", 0x1457DBB18,
     {L"BossCtrl_VtB18_TopPriority_IfMode1", 0x1417F81A8, kBossTopBytes}},
};

// Enemy AI controller (both builds): the mode byte BossBase's gates test, and the target selector's holder (a pointer
// to {controller, selector}).
constexpr std::uint32_t kCtrlMode       = 0x101C;
constexpr std::uint32_t kCtrlHolder     = 0xBF8;
constexpr std::uint32_t kHolderSelector = 0x8;

// The blueprint VM's frame (stock 4.26 FFrame): the object whose blueprint is running.
constexpr std::uint32_t kFrameObject = 0x18;

// Whose blueprint calls are answered: the native enemy controller class and its children (BossBase and every enemy
// AIBP, summon fights included; §16.2). A class test, not vtable addresses: 1.005 moved the controllers' slots by 8, so
// a vtable placed from a slot address was 8 bytes off there and no caller ever matched (live Titan fight, 2026-10-14).
constexpr const wchar_t* kEnemyControllerClass = STR("/Script/EndGame.EndAIControllerEnemyBase");

// The natives, by their UFunctions. The exec addresses are checked against UFunction::Func before a swap.
enum class Kind : std::uint8_t
{
    Leader,      // writes the leader character (UObject*) to the result
    LeaderType,  // writes the leader's player type (u8) to the result
    Ucpc,        // IsUCPC / IsAIPC: run with the exclusive leader test for the answered type
};
struct Native
{
    const wchar_t* path;
    std::uintptr_t exec;
    Kind           kind;
};
constexpr std::size_t kNatives = 6;
constexpr Native      kNativeFacts[kNatives] = {
    {STR("/Script/EndGame.EndBattleAPI:GetPartyLeader"), 0x1414F307C, Kind::Leader},
    {STR("/Script/EndGame.EndAIControllerBluePrintBase:GetLeader"), 0x1414F307C, Kind::Leader},
    {STR("/Script/EndGame.EndAIControllerBluePrintBase:GetPlayerLeader"), 0x1414F307C, Kind::Leader},
    {STR("/Script/EndGame.EndBattleAPI:IsUCPC"), 0x1417BFCCC, Kind::Ucpc},
    {STR("/Script/EndGame.EndBattleAPI:IsAIPC"), 0x143E3D180, Kind::Ucpc},
    {STR("/Script/EndGame.EndPartyAPI:GetPartyLeaderType"), 0x14188EBE4, Kind::LeaderType},
};

using DecisionFn = std::uint64_t (*)(std::uint64_t controller, std::uint64_t b, std::uint64_t c, std::uint64_t d);
// The blueprint VM's native signature: (context, FFrame&, result).
using ExecFn     = RC::Unreal::UnrealScriptFunction;

// Set once at install.
platform::VTableEntry  g_decisions[kDecisions];
platform::CallRedirect g_actionCall;
std::atomic<const RC::Unreal::UClass*> g_enemyClass{nullptr};  // set once on the game thread; read on any thread
std::uintptr_t        g_execs[kNatives]{};     // live exec addresses
std::atomic<ExecFn>   g_originals[kNatives]{};  // null until swapped
UFunction*            g_functions[kNatives]{};  // game thread
bool                  g_swapsDone   = false;    // game thread
unsigned              g_swapTries   = 0;        // game thread

std::atomic<bool> g_on{false};

// The coins, keyed by the controller: the player each names and when it was tossed (player_two/coin_table.h).
coin_table::CoinTable g_coins;

struct Counts
{
    std::atomic<unsigned> tosses[2]{};      // player 1, player 2
    std::atomic<unsigned> answers[2]{};     // coin answers given: player 1, player 2
    std::atomic<unsigned> reactions[2]{};   // answers inside a damage or reaction call: player 1, player 2
    std::atomic<unsigned> retosses{0};      // player 2 no longer a candidate
};
Counts                                g_counts;
std::chrono::steady_clock::time_point g_lastSummary{};

enum class Player : std::uint8_t
{
    None,  // the game's own answer
    One,
    Two,
};

std::uint32_t Seconds() noexcept
{
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void* SelectorOf(const void* controller) noexcept
{
    return ReadPointer(ReadPointer(controller, kCtrlHolder), kHolderSelector);
}

/// Tosses `controller`'s coin by the Condition select's rule (no player 2: none) and keeps it.
Player Toss(const void* controller, const wchar_t* when) noexcept
{
    const auto key    = reinterpret_cast<std::uintptr_t>(controller);
    const auto answer = equal_targeting::ChooseFor(SelectorOf(controller));
    if (answer.choice.ucpc == ucpc_choice::Ucpc::Untouched)
    {
        g_coins.Clear(key);
        return Player::None;
    }
    const bool two = answer.choice.ucpc == ucpc_choice::Ucpc::PlayerTwo;
    g_coins.Set(key, two ? 2 : 1, Seconds());
    g_counts.tosses[two ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
    platform::LogDebug(L"target coin: enemy {:#x} counts player {} as the player until its next action ({}, {})", key,
                       two ? 2 : 1, when,
                       answer.choice.why == ucpc_choice::Why::Coin ? L"coin" : L"the only candidate");
    return two ? Player::Two : Player::One;
}

/// `controller`'s held coin; tossed now if it has none, or if it names player 2 and player 2's character is no longer
/// a candidate for it.
Player CoinOf(const void* controller) noexcept
{
    const auto key  = reinterpret_cast<std::uintptr_t>(controller);
    const int  held = g_coins.PlayerOf(key);
    if (held == 0)
    {
        return Toss(controller, L"first question");
    }
    if (held == 2)
    {
        if (equal_targeting::PlayerTwoEligible(SelectorOf(controller)))
        {
            return Player::Two;
        }
        g_counts.retosses.fetch_add(1, std::memory_order_relaxed);
        return Toss(controller, L"player 2 no longer a candidate");
    }
    return Player::One;
}

/// Whom a native called from `frame` answers as the player: none outside an enemy controller's blueprint or without
/// player 2; the player a damage or reaction call is about; else the enemy's coin. `reaction` says which.
Player AnswerFor(const void* frame, bool& reaction) noexcept
{
    reaction = false;
    if (!g_on.load(std::memory_order_acquire) || !equal_targeting::Types())
    {
        return Player::None;
    }
    const auto* const caller = static_cast<const RC::Unreal::UObject*>(ReadPointer(frame, kFrameObject));
    const auto* const enemyClass = g_enemyClass.load(std::memory_order_acquire);
    if (caller == nullptr || enemyClass == nullptr || !caller->IsA(enemyClass))
    {
        return Player::None;
    }
    switch (player_hits::HumanInCall())
    {
    case player_hits::Human::PlayerOne:
        reaction = true;
        return Player::One;
    case player_hits::Human::PlayerTwo:
        reaction = true;
        return Player::Two;
    case player_hits::Human::None:
        break;
    }
    return CoinOf(caller);
}

void Count(Player player, bool reaction) noexcept
{
    if (player != Player::None)
    {
        (reaction ? g_counts.reactions : g_counts.answers)[player == Player::Two ? 1 : 0].fetch_add(
            1, std::memory_order_relaxed);
    }
}

template <std::size_t I>
void Exec(RC::Unreal::UObject* context, RC::Unreal::FFrame& frame, void* result)
{
    const ExecFn original = g_originals[I].load(std::memory_order_acquire);
    bool         reaction = false;
    const Player player   = AnswerFor(&frame, reaction);
    const auto   types    = equal_targeting::Types();
    if (player == Player::None || !types)
    {
        original(context, frame, result);
        return;
    }
    Count(player, reaction);
    if constexpr (kNativeFacts[I].kind == Kind::Ucpc)
    {
        // Inside a damage or reaction call the call's own answers stand (player 2's hits: both count).
        if (reaction)
        {
            original(context, frame, result);
        }
        else
        {
            // The game's IsUCPC / IsAIPC with only the player's type counting as the player on this thread, put
            // back afterwards.
            thread_hold::HoldingExchange(&member_movement::ExchangeThreadOnlyLeader,
                                         player == Player::Two ? types->two : types->one, original, context,
                                         frame, result);
        }
    }
    else
    {
        original(context, frame, result);  // steps the frame as the game does; then the answer replaces the result
        void* const member = player == Player::Two ? generation::Member() : nullptr;
        if (player == Player::Two && member != nullptr && result != nullptr)
        {
            if constexpr (kNativeFacts[I].kind == Kind::Leader)
            {
                WriteField<void*>(result, 0, member);
            }
            else
            {
                WriteField<std::uint8_t>(result, 0, types->two);
            }
        }
    }
    platform::LogTrace(L"target coin: {} answered player {} ({})", kNativeFacts[I].path, player == Player::Two ? 2 : 1,
                       reaction ? L"the player in this damage or reaction call" : L"the enemy's coin");
}

constexpr ExecFn kExecs[kNatives] = {&Exec<0>, &Exec<1>, &Exec<2>, &Exec<3>, &Exec<4>, &Exec<5>};

bool Tossing() noexcept
{
    return g_on.load(std::memory_order_acquire) && equal_targeting::Types().has_value();
}

/// The OnActionInvoke event's one call: the event is about to run.
std::uint64_t Action(std::uint64_t controller, std::uint64_t type, std::uint64_t extra, std::uint64_t d) noexcept
{
    if (Tossing())
    {
        Toss(reinterpret_cast<const void*>(controller), L"action decision");
    }
    return reinterpret_cast<DecisionFn>(g_actionCall.Original())(controller, type, extra, d);
}

/// The interrupt and top-priority slots: tossed only when the game's own conditions fire the event (a non-null
/// argument; BossBase also mode 1).
template <std::size_t I>
std::uint64_t Decision(std::uint64_t controller, std::uint64_t b, std::uint64_t c, std::uint64_t d) noexcept
{
    const auto original = reinterpret_cast<DecisionFn>(g_decisions[I].Original());
    const auto* self    = reinterpret_cast<const void*>(controller);
    if (b != 0 && (I < kFirstBoss || ReadField<std::uint8_t>(self, kCtrlMode) == 1) && Tossing())
    {
        Toss(self, I % 2 == 0 ? L"interrupt decision" : L"top-priority decision");
    }
    return original(controller, b, c, d);
}

constexpr DecisionFn kDecisionWrappers[kDecisions] = {&Decision<0>, &Decision<1>, &Decision<2>, &Decision<3>};

/// Swaps each UFunction's native pointer whose function exists and still holds the exec IDA named. Game thread.
void TrySwaps() noexcept
{
    if (g_enemyClass.load(std::memory_order_relaxed) == nullptr)
    {
        // Before any swap; read from any thread (an answering native, a lock-group pick).
        const auto* const found =
            UObjectGlobals::StaticFindObject<RC::Unreal::UClass*>(nullptr, nullptr, kEnemyControllerClass);
        g_enemyClass.store(found, std::memory_order_release);
        if (found == nullptr)
        {
            if (++g_swapTries == 1)
            {
                platform::LogDebug(L"target coin: {} not found yet; trying again each second", kEnemyControllerClass);
            }
            return;
        }
    }
    std::size_t done = 0, missing = 0;
    for (std::size_t i = 0; i < kNatives; ++i)
    {
        if (g_originals[i].load(std::memory_order_relaxed) != nullptr)
        {
            ++done;
            continue;
        }
        auto* function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, kNativeFacts[i].path);
        if (function == nullptr)
        {
            ++missing;
            continue;
        }
        const auto current = reinterpret_cast<std::uintptr_t>(function->GetFuncPtr());
        if (g_execs[i] == 0 || current != g_execs[i])
        {
            platform::Log(L"target coin: {} holds {:#x}, not the exec read in IDA; it keeps the game's own answer",
                          kNativeFacts[i].path, current);
            g_originals[i].store(reinterpret_cast<ExecFn>(current), std::memory_order_release);  // never swapped
            g_functions[i] = nullptr;
            ++done;
            continue;
        }
        g_originals[i].store(reinterpret_cast<ExecFn>(current), std::memory_order_release);
        function->SetFuncPtr(kExecs[i]);
        g_functions[i] = function;
        ++done;
    }
    if (missing == 0)
    {
        g_swapsDone = true;
        std::size_t swapped = 0;
        for (auto* function : g_functions)
        {
            swapped += function != nullptr ? 1 : 0;
        }
        platform::Log(L"target coin: {} of {} blueprint natives answer with the enemy's coin", swapped, kNatives);
    }
    else if (++g_swapTries == 1)
    {
        platform::LogDebug(L"target coin: {} blueprint natives not found yet; trying again each second", missing);
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    if (!equal_targeting::Installed() || !member_movement::LeaderTestReplaced())
    {
        platform::Log(L"target coin: UNAVAILABLE (equal targeting is not in place); enemies aim at player 1");
        return false;
    }
    for (std::size_t i = 0; i < kNatives; ++i)
    {
        const auto exec = image.Rebase(kNativeFacts[i].exec);
        g_execs[i]      = exec ? *exec : 0;
    }
    g_on.store(true, std::memory_order_release);
    // on_unreal_init: no world and no enemy exist yet, so no thread can be at the call (InstallFromOwnThread's rule).
    const auto actionSite = image.Rebase(kActionCall, 5);
    const auto action     = image.Rebase(kAction);
    const bool actions    = actionSite && action &&
                         g_actionCall.InstallFromOwnThread(*actionSite, *action, reinterpret_cast<std::uintptr_t>(&Action));
    std::size_t slots = 0;
    for (std::size_t i = 0; i < kDecisions; ++i)
    {
        if (platform::ReplaceSlot(image, kDecisionSlots[i], reinterpret_cast<std::uintptr_t>(kDecisionWrappers[i]),
                                  g_decisions[i]))
        {
            ++slots;
        }
    }
    platform::Log(L"target coin: enemies toss a coin per action at OnActionInvoke {} and at {} of {} interrupt and "
                  L"top-priority slots (no coin answers until the blueprint natives are swapped)",
                  actions ? L"(on)" : L"(UNAVAILABLE: the call differs)", slots, kDecisions);
    return true;
}

void Remove() noexcept
{
    g_on.store(false, std::memory_order_release);  // every native from now on passes the game's own answer through
    for (std::size_t i = 0; i < kNatives; ++i)
    {
        if (g_functions[i] != nullptr)
        {
            g_functions[i]->SetFuncPtr(g_originals[i].load(std::memory_order_relaxed));
            g_functions[i] = nullptr;
        }
    }
    for (auto& slot : g_decisions)
    {
        if (slot.Original() != 0 && !slot.Restore())
        {
            platform::Log(L"target coin: a decision slot was changed after us; left in place (it passes through)");
        }
    }
    // The action call crosses a block on the research build: put back only from the one thread that can run it,
    // which unloading is not; it passes through once g_on is off.
}

int TossForLockPick(const void* controller) noexcept
{
    const auto* const object     = static_cast<const RC::Unreal::UObject*>(controller);
    const auto* const enemyClass = g_enemyClass.load(std::memory_order_acquire);
    if (!Tossing() || object == nullptr || enemyClass == nullptr || !object->IsA(enemyClass))
    {
        return 0;
    }
    switch (Toss(controller, L"lock-group pick"))
    {
    case Player::One:
        return 1;
    case Player::Two:
        return 2;
    case Player::None:
        break;
    }
    return 0;
}

const void* ControllerOfEnemy(const void* enemy) noexcept
{
    const auto* const controller = static_cast<const std::byte*>(ReadPointer(enemy, facts::kCharacterRetainedAi));
    return controller != nullptr && enemy != nullptr &&
                   leader_scope::ObjectAt(controller + facts::kEnemyControllerCharacter) == enemy
               ? controller
               : nullptr;
}

const void* CheckedController(const void* controller) noexcept
{
    const void* const enemy =
        controller != nullptr
            ? leader_scope::ObjectAt(static_cast<const std::byte*>(controller) + facts::kEnemyControllerCharacter)
            : nullptr;
    return enemy != nullptr && ReadPointer(enemy, facts::kCharacterRetainedAi) == controller ? controller : nullptr;
}

int CoinFor(const void* controller) noexcept
{
    const auto* const object     = static_cast<const RC::Unreal::UObject*>(controller);
    const auto* const enemyClass = g_enemyClass.load(std::memory_order_acquire);
    if (!Tossing() || object == nullptr || enemyClass == nullptr || !object->IsA(enemyClass))
    {
        return 0;
    }
    switch (CoinOf(controller))
    {
    case Player::One:
        return 1;
    case Player::Two:
        return 2;
    case Player::None:
        break;
    }
    return 0;
}

void ShareCoin(const void* from, const void* to) noexcept
{
    const auto* const object     = static_cast<const RC::Unreal::UObject*>(to);
    const auto* const enemyClass = g_enemyClass.load(std::memory_order_acquire);
    if (!Tossing() || from == nullptr || to == nullptr || from == to || enemyClass == nullptr ||
        !object->IsA(enemyClass))
    {
        return;
    }
    const auto        fromKey = reinterpret_cast<std::uintptr_t>(from);
    const std::size_t found   = g_coins.Find(fromKey);
    const int         held    = g_coins.PlayerOf(fromKey);
    if (held == 0)
    {
        return;
    }
    // The partner's own rule still holds: player 2 only while player 2's character is a candidate for its selector.
    const bool two   = held == 2 && equal_targeting::PlayerTwoEligible(SelectorOf(to));
    const auto toKey = reinterpret_cast<std::uintptr_t>(to);
    g_coins.Set(toKey, two ? 2 : 1, Seconds(), found);  // never evicting `from`'s own coin
    platform::LogDebug(L"target coin: enemy {:#x} takes enemy {:#x}'s coin, player {}, until its next action "
                       L"(combination partner{})",
                       toKey, fromKey, two ? 2 : 1,
                       held == 2 && !two ? L"; player 2 is not a candidate for it" : L"");
}

void LockPickFellBack(const void* controller) noexcept
{
    const auto key = reinterpret_cast<std::uintptr_t>(controller);
    if (g_coins.FallBack(key))
    {
        platform::LogDebug(L"target coin: enemy {:#x} counts player 1 as the player until its next action (its "
                           L"lock-group pick could not take player 2)",
                           key);
    }
}

void Tick() noexcept
{
    if (!g_on.load(std::memory_order_relaxed) || !generation::OnGameThread())
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!g_swapsDone && (g_swapTries == 0 || now - g_lastSummary >= std::chrono::seconds(1)))
    {
        TrySwaps();
        if (!g_swapsDone)
        {
            g_lastSummary = now;
            return;
        }
    }
    if (now - g_lastSummary < std::chrono::seconds(60))
    {
        return;
    }
    g_lastSummary = now;
    const auto take = [](std::atomic<unsigned>& count) { return count.exchange(0, std::memory_order_relaxed); };
    const unsigned t1 = take(g_counts.tosses[0]), t2 = take(g_counts.tosses[1]);
    const unsigned a1 = take(g_counts.answers[0]), a2 = take(g_counts.answers[1]);
    const unsigned r1 = take(g_counts.reactions[0]), r2 = take(g_counts.reactions[1]);
    const unsigned again = take(g_counts.retosses);
    if (t1 + t2 + a1 + a2 + r1 + r2 + again != 0)
    {
        platform::LogDebug(L"target coin (60 s): tosses P1 {} / P2 {}; coin answers P1 {} / P2 {}; damage or reaction "
                           L"answers P1 {} / P2 {}; re-tossed {} (player 2 no longer a candidate)",
                           t1, t2, a1, a2, r1, r2, again);
    }
}

}  // namespace lss::player_two::target_coin
