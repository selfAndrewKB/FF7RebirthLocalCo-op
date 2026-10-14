#include "player_two/target_coin.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "player_two/equal_targeting.h"
#include "player_two/generation.h"
#include "player_two/member_movement.h"
#include "player_two/object_fields.h"
#include "player_two/player_hits.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace lss::player_two::target_coin {

namespace {

using RC::Unreal::UFunction;
namespace UObjectGlobals = RC::Unreal::UObjectGlobals;

// Facts read from the database (RE-p2-enemy-target-bias.md §16; this build). 1.005: replaced-target-coin.json.

// The enemy controllers' decision slots. EnemyBase's hold the blueprint events' stubs; BossBase's hold gates that call
// the same stubs only when the controller's mode byte is 1. Each takes the controller first and at most three integer
// arguments (+0xAD8: ctrl, u8, ptr; +0xAE0 / +0xB18: ctrl, ptr), all passed on.
constexpr std::uint8_t kActionBytes[]      = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                              0x24, 0x18, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr std::uint8_t kEventBytes[]       = {0x48, 0x85, 0xD2, 0x74, 0x48, 0x48, 0x89, 0x5C,
                                              0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr std::uint8_t kBossActionBytes[]  = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x86, 0x62,
                                              0x88, 0x00, 0x48, 0x2B, 0xE0, 0x80, 0xB9, 0x1C};
constexpr std::uint8_t kBossInterruptBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xD2, 0x98,
                                                0x7F, 0x00, 0x48, 0x2B, 0xE0, 0x80, 0xB9, 0x1C};
constexpr std::uint8_t kBossTopBytes[]     = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x7E, 0x6D,
                                              0x8B, 0x00, 0x48, 0x2B, 0xE0, 0x80, 0xB9, 0x1C};

constexpr std::size_t kDecisions  = 6;
constexpr std::size_t kFirstBoss  = 3;  // slots 3..5 are BossBase's
constexpr platform::SlotFact kDecisionSlots[kDecisions] = {
    {L"the EnemyBase controller's OnActionInvoke slot (+0xAD8)", 0x1457DC768,
     {L"EnemyCtrl_VtAD8_FireOnActionInvoke", 0x140CB9FF4, kActionBytes}},
    {L"the EnemyBase controller's OnActionInterruptInvoke slot (+0xAE0)", 0x1457DC770,
     {L"EnemyAICtrl_Event_OnActionInterruptInvoke", 0x1418B5674, kEventBytes}},
    {L"the EnemyBase controller's OnActionTopPriorityInvoke slot (+0xB18)", 0x1457DC7A8,
     {L"EnemyAICtrl_Event_OnActionTopPriorityInvoke", 0x1417F81C8, kEventBytes}},
    {L"the BossBase controller's OnActionInvoke slot (+0xAD8)", 0x1457DBAD8,
     {L"BossCtrl_VtAD8_FireOnActionInvoke_IfMode1", 0x141828CA0, kBossActionBytes}},
    {L"the BossBase controller's OnActionInterruptInvoke slot (+0xAE0)", 0x1457DBAE0,
     {L"BossCtrl_VtAE0_Interrupt_IfMode1", 0x1418B5654, kBossInterruptBytes}},
    {L"the BossBase controller's OnActionTopPriorityInvoke slot (+0xB18)", 0x1457DBB18,
     {L"BossCtrl_VtB18_TopPriority_IfMode1", 0x1417F81A8, kBossTopBytes}},
};
constexpr std::uint32_t kActionSlot = 0xAD8;  // the vtable is the first slot's address minus this (both builds)

// Enemy AI controller (both builds): the mode byte BossBase's gates test, and the target selector's holder (a pointer
// to {controller, selector}).
constexpr std::uint32_t kCtrlMode       = 0x101C;
constexpr std::uint32_t kCtrlHolder     = 0xBF8;
constexpr std::uint32_t kHolderSelector = 0x8;

// The blueprint VM's frame (stock 4.26 FFrame): the object whose blueprint is running.
constexpr std::uint32_t kFrameObject = 0x18;

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
platform::VTableEntry g_decisions[kDecisions];
std::uintptr_t        g_enemyVTable = 0, g_bossVTable = 0;
std::uintptr_t        g_execs[kNatives]{};     // live exec addresses
std::atomic<ExecFn>   g_originals[kNatives]{};  // null until swapped
UFunction*            g_functions[kNatives]{};  // game thread
bool                  g_swapsDone   = false;    // game thread
unsigned              g_swapTries   = 0;        // game thread

std::atomic<bool> g_on{false};

// The coins: the controller's address with the answered player in its low bits (1 player 1, 2 player 2), 0 empty, and
// when it was tossed. A controller's coin is in one of four neighbouring slots; a new one takes an empty slot, else the
// one tossed longest ago (a live enemy tosses at each decision, every few seconds; the oldest is a gone enemy's). Two
// threads placing coins in the same slot at once leave one of them: the other tosses again at its next question.
constexpr std::size_t    kCoinBits  = 10;
constexpr std::size_t    kCoinSlots = std::size_t{1} << kCoinBits;
constexpr std::size_t    kProbe     = 4;
constexpr std::uintptr_t kCoinMask  = 3;
constexpr std::size_t    kNoSlot    = kCoinSlots;
std::atomic<std::uintptr_t> g_coins[kCoinSlots]{};
std::atomic<std::uint32_t>  g_tossedAt[kCoinSlots]{};  // seconds on the steady clock

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

std::size_t SlotOf(std::uintptr_t controller, std::size_t probe) noexcept
{
    const auto base = static_cast<std::size_t>(((controller >> 4) * 0x9E3779B97F4A7C15ull) >> (64 - kCoinBits));
    return (base + probe) & (kCoinSlots - 1);
}

std::uint32_t Seconds() noexcept
{
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

/// The slot holding `controller`'s coin, or kNoSlot.
std::size_t Find(std::uintptr_t key) noexcept
{
    for (std::size_t probe = 0; probe < kProbe; ++probe)
    {
        const std::size_t slot = SlotOf(key, probe);
        if ((g_coins[slot].load(std::memory_order_relaxed) & ~kCoinMask) == key)
        {
            return slot;
        }
    }
    return kNoSlot;
}

/// Where a new coin for `key` goes: its own slot, else an empty one, else the one tossed longest ago.
std::size_t Place(std::uintptr_t key) noexcept
{
    if (const std::size_t own = Find(key); own != kNoSlot)
    {
        return own;
    }
    const std::uint32_t now    = Seconds();
    std::size_t         oldest = SlotOf(key, 0);
    std::uint32_t       age    = 0;
    for (std::size_t probe = 0; probe < kProbe; ++probe)
    {
        const std::size_t slot = SlotOf(key, probe);
        if (g_coins[slot].load(std::memory_order_relaxed) == 0)
        {
            return slot;
        }
        const std::uint32_t slotAge = now - g_tossedAt[slot].load(std::memory_order_relaxed);
        if (slotAge >= age)
        {
            age    = slotAge;
            oldest = slot;
        }
    }
    return oldest;
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
        if (const std::size_t slot = Find(key); slot != kNoSlot)
        {
            std::uintptr_t held = g_coins[slot].load(std::memory_order_relaxed);
            if ((held & ~kCoinMask) == key)
            {
                g_coins[slot].compare_exchange_strong(held, 0, std::memory_order_relaxed);
            }
        }
        return Player::None;
    }
    const bool two = answer.choice.ucpc == ucpc_choice::Ucpc::PlayerTwo;
    const std::size_t slot = Place(key);
    g_tossedAt[slot].store(Seconds(), std::memory_order_relaxed);
    g_coins[slot].store(key | (two ? 2u : 1u), std::memory_order_relaxed);
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
    const auto        key  = reinterpret_cast<std::uintptr_t>(controller);
    const std::size_t slot = Find(key);
    const auto        held = slot == kNoSlot ? 0 : g_coins[slot].load(std::memory_order_relaxed);
    if ((held & ~kCoinMask) != key)
    {
        return Toss(controller, L"first question");
    }
    if ((held & kCoinMask) == 2)
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
    const void* const caller = ReadPointer(frame, kFrameObject);
    const auto        vtable = VTableOf(caller);
    if (vtable == 0 || (vtable != g_enemyVTable && vtable != g_bossVTable))
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

/// The game's IsUCPC / IsAIPC with only `type` counting as the player on this thread, put back in the __finally (see
/// equal_targeting's CallHolding). No object here needs unwinding (the __try rule).
void CallOnly(ExecFn original, RC::Unreal::UObject* context, RC::Unreal::FFrame& frame, void* result, int type) noexcept
{
    const int outer = member_movement::ExchangeThreadOnlyLeader(type);
    __try
    {
        original(context, frame, result);
    }
    __finally
    {
        member_movement::ExchangeThreadOnlyLeader(outer);
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
            CallOnly(original, context, frame, result, player == Player::Two ? types->two : types->one);
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

template <std::size_t I>
std::uint64_t Decision(std::uint64_t controller, std::uint64_t b, std::uint64_t c, std::uint64_t d) noexcept
{
    const auto original = reinterpret_cast<DecisionFn>(g_decisions[I].Original());
    const auto* self    = reinterpret_cast<const void*>(controller);
    if (g_on.load(std::memory_order_acquire) && equal_targeting::Types() &&
        (I < kFirstBoss || ReadField<std::uint8_t>(self, kCtrlMode) == 1))
    {
        Toss(self, L"action decision");
    }
    return original(controller, b, c, d);
}

constexpr DecisionFn kDecisionWrappers[kDecisions] = {&Decision<0>, &Decision<1>, &Decision<2>,
                                                      &Decision<3>, &Decision<4>, &Decision<5>};

/// Swaps each UFunction's native pointer whose function exists and still holds the exec IDA named. Game thread.
void TrySwaps() noexcept
{
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
    const auto enemySlot = image.Rebase(kDecisionSlots[0].slot, sizeof(void*));
    const auto bossSlot  = image.Rebase(kDecisionSlots[kFirstBoss].slot, sizeof(void*));
    g_enemyVTable        = enemySlot ? *enemySlot - kActionSlot : 0;
    g_bossVTable         = bossSlot ? *bossSlot - kActionSlot : 0;
    g_on.store(true, std::memory_order_release);
    std::size_t slots = 0;
    for (std::size_t i = 0; i < kDecisions; ++i)
    {
        if (platform::ReplaceSlot(image, kDecisionSlots[i], reinterpret_cast<std::uintptr_t>(kDecisionWrappers[i]),
                                  g_decisions[i]))
        {
            ++slots;
        }
    }
    if (g_enemyVTable == 0 || g_bossVTable == 0)
    {
        g_on.store(false, std::memory_order_release);
        platform::Log(L"target coin: UNAVAILABLE (the enemy controllers' vtables could not be placed); enemies aim at "
                      L"player 1");
        return false;
    }
    platform::Log(L"target coin: {} of {} enemy decision slots toss a coin per action (no coin answers until the "
                  L"blueprint natives are swapped)",
                  slots, kDecisions);
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
