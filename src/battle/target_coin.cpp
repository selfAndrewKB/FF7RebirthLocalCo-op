#include "battle/target_coin.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "battle/coin_table.h"
#include "battle/enemy_leader.h"
#include "battle/equal_targeting.h"
#include "battle/player_hits.h"
#include "facts/party.h"
#include "platform/call_redirect.h"
#include "platform/every.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "scopes/leader_answer.h"
#include "scopes/leader_scope.h"
#include "scopes/thread_hold.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

namespace lss::battle::target_coin {

namespace {

using RC::Unreal::UFunction;
namespace UObjectGlobals = RC::Unreal::UObjectGlobals;

// Facts read from the database (research/RE-p2-enemy-target-bias.md; this build). 1.005: replaced-target-coin.json.

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
// AIBP, summon fights included). A class test, not vtable addresses: 1.005 moved the controllers' slots by 8, so
// a vtable placed from a slot address was 8 bytes off there and no caller ever matched (seen live in a Titan fight).
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
std::atomic<RC::Unreal::UClass*> g_enemyClass{nullptr};  // set once on the game thread; read on any thread (IsA takes
                                                         // a non-const class)
std::uintptr_t        g_execs[kNatives]{};     // live exec addresses
std::atomic<ExecFn>   g_originals[kNatives]{};  // null until swapped
UFunction*            g_functions[kNatives]{};  // game thread
bool                  g_swapsDone   = false;    // game thread
unsigned              g_swapTries   = 0;        // game thread

std::atomic<bool> g_on{false};

// The coins, keyed by the controller: the player each names and when it was tossed (battle/coin_table.h).
coin_table::CoinTable g_coins;

// The tallies, by seat (index 0 player 1's).
struct Counts
{
    std::atomic<unsigned> tosses[players::kMaxPlayers]{};
    std::atomic<unsigned> answers[players::kMaxPlayers]{};    // coin answers given
    std::atomic<unsigned> reactions[players::kMaxPlayers]{};  // answers inside a damage or reaction call
    std::atomic<unsigned> retosses{0};                        // a seat's player no longer a candidate
};
Counts                                g_counts;
platform::Every                       g_swapRetry{std::chrono::seconds(1)};  // the swaps tried again
platform::Every                       g_summary{std::chrono::seconds(60)};

// The player a coin or an answer names: its number (coin_table::PlayerNumber: 1 player 1, seat + 1 a seat's player),
// kNone for the game's own answer.
using Player                   = int;
constexpr Player kNone         = 0;
constexpr Player kPlayerOne    = coin_table::PlayerNumber(0);

constexpr int SeatOf(Player player) noexcept
{
    return coin_table::SeatOfNumber(player);
}

/// The reason a coin naming `seat` is tossed again (a literal: Toss logs it).
const wchar_t* NoLongerCandidate(int seat) noexcept
{
    constexpr const wchar_t* kTexts[players::kMaxPlayers] = {L"", L"player 2 no longer a candidate",
                                                             L"player 3 no longer a candidate"};
    return players::SeatIndex(seat) ? kTexts[seat] : L"player 2 or 3 no longer a candidate";
}
static_assert(players::kMaxPlayers == 3, "NoLongerCandidate names every seat");

std::uint32_t Seconds() noexcept
{
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void* SelectorOf(const void* controller) noexcept
{
    return ReadPointer(ReadPointer(controller, kCtrlHolder), kHolderSelector);
}

/// Tosses `controller`'s coin by the Condition select's rule (no seat's member: none) and keeps it.
Player Toss(const void* controller, const wchar_t* when) noexcept
{
    const auto key    = reinterpret_cast<std::uintptr_t>(controller);
    const auto answer = equal_targeting::ChooseFor(SelectorOf(controller));
    if (answer.choice.ucpc == ucpc_choice::Ucpc::Untouched)
    {
        g_coins.Clear(key);
        return kNone;
    }
    const Player player = coin_table::PlayerNumber(answer.choice.player);
    g_coins.Set(key, player, Seconds());
    g_counts.tosses[answer.choice.player].fetch_add(1, std::memory_order_relaxed);
    platform::LogDebug(L"target coin: enemy {:#x} counts player {} as the player until its next action ({}, {})", key,
                       player, when, answer.choice.why == ucpc_choice::Why::Coin ? L"coin" : L"the only candidate");
    return player;
}

/// `controller`'s held coin; tossed now if it has none, if it names a seat's player whose character is no longer a
/// candidate for it, or if it names player 1 while player 1's character is down and another player's stands in.
Player CoinOf(const void* controller) noexcept
{
    const auto key  = reinterpret_cast<std::uintptr_t>(controller);
    const int  held = g_coins.PlayerOf(key);
    if (held == kNone)
    {
        return Toss(controller, L"first question");
    }
    if (held > kPlayerOne)
    {
        if (equal_targeting::SeatEligible(SeatOf(held), SelectorOf(controller)))
        {
            return held;
        }
        g_counts.retosses.fetch_add(1, std::memory_order_relaxed);
        return Toss(controller, NoLongerCandidate(SeatOf(held)));
    }
    // A toss then finds that player's character a candidate (none other would change the answer: player 1's again).
    if (const int standing = players::generation::SeatOfMember(enemy_leader::Instead());
        players::SeatBuilt(standing) && equal_targeting::SeatEligible(standing, SelectorOf(controller)))
    {
        g_counts.retosses.fetch_add(1, std::memory_order_relaxed);
        return Toss(controller, L"player 1's character is down");
    }
    return kPlayerOne;
}

/// Whom a native called from `frame` answers as the player: none outside an enemy controller's blueprint or without a
/// seat's member; the player a damage or reaction call is about; else the enemy's coin. `reaction` says which.
Player AnswerFor(const void* frame, bool& reaction) noexcept
{
    reaction = false;
    if (!g_on.load(std::memory_order_acquire) || !equal_targeting::Types())
    {
        return kNone;
    }
    const void* const caller = ReadPointer(frame, kFrameObject);
    if (!IsEnemyController(caller))
    {
        return kNone;
    }
    if (const int human = player_hits::HumanInCall(); human != player_hits::kNoHuman)
    {
        reaction = true;
        return coin_table::PlayerNumber(human);
    }
    return CoinOf(caller);
}

void Count(Player player, bool reaction) noexcept
{
    if (player != kNone)
    {
        (reaction ? g_counts.reactions : g_counts.answers)[SeatOf(player)].fetch_add(1, std::memory_order_relaxed);
    }
}

using Tally = std::array<unsigned, players::kMaxPlayers>;

/// One tally of the summary taken (player 1 and the built seats; reset to 0), its sum added to `total`.
Tally TakeTally(std::atomic<unsigned> (&counts)[players::kMaxPlayers], unsigned& total) noexcept
{
    Tally taken{};
    for (int seat = 0; seat <= players::kSeatsBuilt; ++seat)
    {
        taken[static_cast<std::size_t>(seat)] = platform::Take(counts[seat]);
        total += taken[static_cast<std::size_t>(seat)];
    }
    return taken;
}

/// "P1 a / P2 b" over a taken tally. Allocates (may throw): only for a line that is written.
std::wstring TallyText(const Tally& tally)
{
    std::wstring text;
    for (int seat = 0; seat <= players::kSeatsBuilt; ++seat)
    {
        text += std::format(L"{}P{} {}", seat == 0 ? L"" : L" / ", seat + 1, tally[static_cast<std::size_t>(seat)]);
    }
    return text;
}

template <std::size_t I>
void Exec(RC::Unreal::UObject* context, RC::Unreal::FFrame& frame, void* result)
{
    const ExecFn original = g_originals[I].load(std::memory_order_acquire);
    bool         reaction = false;
    const Player player   = AnswerFor(&frame, reaction);
    const auto   types    = equal_targeting::Types();
    if (player == kNone || !types)
    {
        original(context, frame, result);
        return;
    }
    Count(player, reaction);
    if constexpr (kNativeFacts[I].kind == Kind::Ucpc)
    {
        // Inside a damage or reaction call the call's own answers stand (a seat's hits: every human counts).
        if (reaction)
        {
            original(context, frame, result);
        }
        else
        {
            // The game's IsUCPC / IsAIPC with only the player's type counting as the player on this thread, put
            // back afterwards: player 1's character's stand-in's while it is down.
            std::uint8_t type = types->type[SeatOf(player)];
            if (player == kPlayerOne)
            {
                std::uint8_t standIn = facts::kNoPlayerType;
                type                 = enemy_leader::Instead(&standIn) != nullptr ? standIn : type;
            }
            scopes::thread_hold::HoldingExchange(&scopes::leader_answer::ExchangeThreadOnly, type, original, context, frame,
                                                 result);
        }
    }
    else
    {
        original(context, frame, result);  // steps the frame as the game does; then the answer replaces the result
        const int    seat   = SeatOf(player);
        std::uint8_t type   = seat >= 1 ? types->type[seat] : facts::kNoPlayerType;
        void*        member = seat >= 1 ? players::generation::MemberOf(seat) : enemy_leader::Instead(&type);
        if (member != nullptr && result != nullptr)
        {
            if constexpr (kNativeFacts[I].kind == Kind::Leader)
            {
                WriteField<void*>(result, 0, member);
            }
            else
            {
                WriteField<std::uint8_t>(result, 0, type);
            }
        }
    }
    platform::LogTrace(L"target coin: {} answered player {} ({})", kNativeFacts[I].path, player,
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
        auto* const found = UObjectGlobals::StaticFindObject<RC::Unreal::UClass*>(nullptr, nullptr, kEnemyControllerClass);
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
    if (!equal_targeting::Installed() || !scopes::leader_answer::Replaced())
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

bool IsEnemyController(const void* controller) noexcept
{
    const auto* const object     = static_cast<const RC::Unreal::UObject*>(controller);
    auto* const       enemyClass = g_enemyClass.load(std::memory_order_acquire);
    return object != nullptr && enemyClass != nullptr && object->IsA(enemyClass);
}

int TossForLockPick(const void* controller) noexcept
{
    if (!Tossing() || !IsEnemyController(controller))
    {
        return 0;
    }
    return Toss(controller, L"lock-group pick");
}

const void* ControllerOfEnemy(const void* enemy) noexcept
{
    const auto* const controller = static_cast<const std::byte*>(ReadPointer(enemy, facts::kCharacterRetainedAi));
    return controller != nullptr && enemy != nullptr &&
                   scopes::leader_scope::ObjectAt(controller + facts::kEnemyControllerCharacter) == enemy
               ? controller
               : nullptr;
}

const void* CheckedController(const void* controller) noexcept
{
    const void* const enemy =
        controller != nullptr
            ? scopes::leader_scope::ObjectAt(static_cast<const std::byte*>(controller) + facts::kEnemyControllerCharacter)
            : nullptr;
    return enemy != nullptr && ReadPointer(enemy, facts::kCharacterRetainedAi) == controller ? controller : nullptr;
}

int CoinFor(const void* controller) noexcept
{
    if (!Tossing() || !IsEnemyController(controller))
    {
        return 0;
    }
    return CoinOf(controller);
}

void ShareCoin(const void* from, const void* to) noexcept
{
    if (!Tossing() || from == nullptr || from == to || !IsEnemyController(to))
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
    // The partner's own rule still holds: a seat's player only while its character is a candidate for its selector.
    const bool   seated = held > kPlayerOne && equal_targeting::SeatEligible(SeatOf(held), SelectorOf(to));
    const Player player = seated ? held : kPlayerOne;
    const auto   toKey  = reinterpret_cast<std::uintptr_t>(to);
    g_coins.Set(toKey, player, Seconds(), found);  // never evicting `from`'s own coin
    if (held > kPlayerOne && !seated)
    {
        platform::LogDebug(L"target coin: enemy {:#x} takes enemy {:#x}'s coin, player {}, until its next action "
                           L"(combination partner; {} is not a candidate for it)",
                           toKey, fromKey, player, players::SeatName(SeatOf(held)));
    }
    else
    {
        platform::LogDebug(L"target coin: enemy {:#x} takes enemy {:#x}'s coin, player {}, until its next action "
                           L"(combination partner)",
                           toKey, fromKey, player);
    }
}

void LockPickFellBack(const void* controller) noexcept
{
    const auto key  = reinterpret_cast<std::uintptr_t>(controller);
    const int  from = g_coins.FallBack(key);  // the player the coin named when it fell back (0: it did not)
    if (from != 0)
    {
        platform::LogDebug(L"target coin: enemy {:#x} counts player 1 as the player until its next action (its "
                           L"lock-group pick could not take {})",
                           key, players::SeatName(SeatOf(from)));
    }
}

void Tick() noexcept
{
    if (!g_on.load(std::memory_order_relaxed) || !players::generation::OnGameThread())
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!g_swapsDone && (g_swapTries == 0 || g_swapRetry.Due(now)))
    {
        TrySwaps();
        if (!g_swapsDone)
        {
            return;
        }
    }
    if (!g_summary.Due(now))
    {
        return;
    }
    unsigned       total     = 0;
    const Tally    tosses    = TakeTally(g_counts.tosses, total);
    const Tally    answers   = TakeTally(g_counts.answers, total);
    const Tally    reactions = TakeTally(g_counts.reactions, total);
    const unsigned again     = platform::Take(g_counts.retosses);
    if (total + again != 0 && platform::Enabled(platform::Level::Debug))
    {
        try
        {
            platform::LogDebug(L"target coin (60 s): tosses {}; coin answers {}; damage or reaction answers {}; re-tossed "
                               L"{} ({} no longer a candidate)",
                               TallyText(tosses), TallyText(answers), TallyText(reactions), again,
                               players::kSeatsBuilt == 1 ? L"player 2" : L"player 2 or 3");
        }
        catch (...)
        {
        }
    }
}

}  // namespace lss::battle::target_coin
