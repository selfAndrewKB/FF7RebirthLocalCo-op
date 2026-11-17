#include "battle/leader_reads.h"

#include "battle/coin_table.h"
#include "battle/enemy_leader.h"
#include "battle/equal_targeting.h"
#include "battle/target_coin.h"
#include "facts/party.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "scopes/leader_scope.h"
#include "scopes/thread_hold.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

namespace lss::battle::leader_reads {

namespace {

// Facts read from the database (research/RE-p2-leader-native-reads.md; this build; every call site's bytes
// and target and every slot's function read from both executables). 1.005:
// replaced-leader-reads.json. "Leader read" = a call of ResolvePartyLeaderWeakPtr (UObject* (); only rax is live after
// each one, both builds).

// E1-E3 enemy combinations. The initiator's action registers the pair (enemy A = the initiator: only Rude and Tseng own
// combination definitions); the battle tick runs each combination's mode every tick, whose leader read runs once, when
// both enemies are ready. A combination entry holds the enemies as FWeakObjectPtrs at +0x30 + 8 * index, the indices
// (u8) at +0x54 (A) and +0x55 (B).
constexpr std::uintptr_t kRegisterPairCall = 0x143080383;  // in EnemyCombination_ResolvePairByRowAndRegister
constexpr std::uintptr_t kRegisterPair     = 0x14313A7CC;  // (?, enemy A, enemy B, request*, target*): 5 integer args
constexpr std::uintptr_t kStartLeaderCall  = 0x14309FF7D;  // in EnemyCombination_Start_TargetModeLeader; crosses both
constexpr std::uintptr_t kMode1LeaderCall  = 0x14309EF2C;  // crosses both
constexpr std::uintptr_t kMode2LeaderCall  = 0x14309E0BA;
// Both modes, both builds: void(manager, entry, float dt in xmm2); first instruction mov [rsp+18h],rbx.
constexpr std::uint8_t kModeBytes[]  = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x4C,
                                        0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41};
// EnemyCombination_Mode2_TargetLeader
constexpr platform::FunctionFact kMode2{L"EnemyCombination_Mode2_TargetLeader", 0x14309DD10, kModeBytes};
// EnemyCombination_Mode1_TargetLeaderElseOther
constexpr platform::FunctionFact kMode1{L"EnemyCombination_Mode1_TargetLeaderElseOther", 0x14309EB90, kModeBytes};
constexpr std::size_t  kModeFirst    = 5;
constexpr std::uint32_t kEntryEnemies = 0x30, kEntryIndexA = 0x54, kEntryIndexB = 0x55;

// E4 a boss's timed damage-phase reaction: the BossBase controller's actor Tick (void(ctrl, float dt in xmm1)) calls
// BossDmgPhase_TryTimedReaction each frame once its timer runs out, until it fires; the leader read comes after every
// early return, so it runs once per phase change. The slot is the function's only reference.
constexpr std::uint8_t kBossTickBytes[] = {0x40, 0x53, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8,
                                           0x58, 0x6E, 0x5F, 0x01, 0x48, 0x2B, 0xE0, 0xC5};
constexpr platform::SlotFact kBossTickSlot{L"the BossBase controller's actor Tick slot (+0x468)", 0x145676E20,
                                           {L"BossCtrl_Vt468_Tick_TimedDmgPhase", 0x140B74D1C, kBossTickBytes}};
constexpr std::uintptr_t     kPhaseLeaderCall = 0x142EA1258;  // in BossDmgPhase_TryTimedReaction; crosses on 1.004
// E5 the damage-phase gate a boss re-runs once at the end of a party summon's cut scene (game thread), with the leader
// as the attacker. (ctrl); the slot is its only reference.
constexpr std::uint8_t kAfterSummonBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x30,
                                              0x02, 0x00, 0x00, 0xE8, 0x7C, 0xAD, 0x71, 0x01};
constexpr platform::SlotFact kAfterSummonSlot{
    L"the BossBase controller's after-summon slot (+0xAB0)", 0x145677468,
    {L"BossCtrl_VtAB0_AfterSummonCutscene_DmgPhaseAsLeader", 0x140A50DF4, kAfterSummonBytes}};
constexpr std::uintptr_t kAfterSummonLeaderCall = 0x140A50E17;

// E6 the reaction player-type filter, char(manager, causer, event info, lists): its only three calls (the causer in
// rdx; only al is live after each), and its leader read, compared only for equality with the causer.
constexpr std::uintptr_t kFilter           = 0x140DBF03C;  // EnemyTrans_MatchEntry_ReactionPlayerType_LeaderPtr
constexpr std::size_t    kFilterCalls      = 3;
constexpr std::uintptr_t kFilterCall[kFilterCalls] = {
    0x140DBEFD3,  // EnemyTrans_MatchEntry_Wrapper (guard / avoid)
    0x142763BFD,  // EnemyTrans_TryTransitionsForCharacter_A (RequestReactionCounter...); crosses on 1.005
    0x142E9FF72,  // EnemyTrans_TryTransitionsForCharacter_B
};
constexpr std::uintptr_t kFilterLeaderCall = 0x140DBF140;  // crosses on 1.004

// E7 Reno's mine: RenoMine_SetState (15 callers; no usable entry) first calls the mine's part getter right after its
// state gate, then (with no target) reads the leader's TYPE and dereferences that type's character without a null
// check. The getter call records the mine; the read takes it. Never redirect the state-13/14 test call between them
// (its caller keeps using rcx).
constexpr std::uintptr_t kMinePartCall = 0x142EC035C;  // crosses on 1.005
constexpr std::uintptr_t kMinePart     = 0x142EBB738;  // u64(mine)
constexpr std::uintptr_t kMineTypeCall = 0x142EC0530;  // call UEndPartyAPI_GetPartyLeaderType, u8()
using facts::kGetPartyLeaderType;
// The mine: u8 state at +0x4F0 (15 or more: none); +0x10 an object whose +0x10 is an FWeakObjectPtr to its AI controller.
constexpr std::uint32_t kMineState = 0x4F0, kMineObject = 0x10, kMineObjectController = 0x10;
constexpr std::uint8_t  kMineStates = 15;

// E8 Crimson Mare's arms: a battle action-state object whose +0x30 is an FWeakObjectPtr to the boss character (set by
// Begin's base init, so it is resolved at the reads). Its Begin (5 integer args) and Tick (arms, float dt in xmm1)
// slots and the rocket-punch notify's one PurgeArm call are the only roots of the three reads. PurgeArm's own leader read
// (moving player 1's lock-on to the purged arm) is not one of them and stays the game's.
constexpr std::uint8_t kArmsBeginBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                            0x24, 0x10, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56};
constexpr std::uint8_t kArmsTickBytes[]  = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                            0x24, 0x10, 0x57, 0xB8, 0x30, 0x00, 0x00, 0x00};
constexpr platform::SlotFact kArmsBeginSlot{L"Crimson Mare's arms state Begin slot (+0x10)", 0x145B41A80,
                                            {L"CrimsonMareArmsState_Vt10_Begin", 0x1430BDC54, kArmsBeginBytes}};
constexpr platform::SlotFact kArmsTickSlot{L"Crimson Mare's arms state Tick slot (+0x30)", 0x145B41AA0,
                                           {L"CrimsonMareArmsState_Vt30_Tick", 0x1430C8B30, kArmsTickBytes}};
constexpr std::uintptr_t kPurgeArmCall = 0x142F7298D;  // in the rocket-punch notify; crosses on 1.005
constexpr std::uintptr_t kPurgeArm     = 0x1430BEEA4;  // CrimsonMareArms_PurgeArm_MoveLeaderLockToArm(arms, u8, u8)
constexpr std::size_t    kArmReads     = 3;
constexpr std::uintptr_t kArmReadCall[kArmReads] = {
    0x1430B59CF,  // PickArmTarget_LeaderFirst (each frame per waiting arm); crosses both
    0x1430C433D,  // SetArmState (the game caches it); crosses on 1.005
    0x1430C222E,  // SyncRelease_ReactionFromLeader; crosses on 1.005
};
constexpr std::uint32_t kArmsBoss = 0x30;

using CallFn  = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
using Call5Fn = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
using TickFn  = std::uint64_t (*)(std::uint64_t, float);                  // (object, float dt in xmm1)
using ModeFn  = std::uint64_t (*)(std::uint64_t, std::uint64_t, float);   // (manager, entry, float dt in xmm2)
using CharacterByTypeFn = void* (*)(std::uint8_t lane, std::uint8_t playerType);

enum Item : std::size_t
{
    kCombination,
    kTimedPhase,
    kAfterSummon,
    kReactionFilter,
    kMine,
    kArms,
    kItems,
};
constexpr const wchar_t* kItemNames[kItems] = {L"enemy combinations",        L"timed damage-phase reactions",
                                               L"after-summon damage phase", L"the reaction player-type filter",
                                               L"Reno's mines",              L"Crimson Mare's arms"};

// Set once at install.
platform::CallRedirect  g_registerPair, g_startLeader, g_mode1Leader, g_mode2Leader, g_phaseLeader, g_afterSummonLeader,
                        g_filterLeader, g_minePart, g_mineType, g_purgeArm;
platform::CallRedirect  g_filterCall0, g_filterCall1, g_filterCall2, g_armRead[kArmReads];
platform::EntryRedirect g_mode1, g_mode2;
platform::VTableEntry   g_bossTick, g_afterSummon, g_armsBegin, g_armsTick;
CharacterByTypeFn       g_characterByType = nullptr;

std::atomic<bool> g_on{false};
std::atomic<bool> g_itemOn[kItems]{};
std::atomic<bool> g_firstSeen[kItems]{};
std::atomic<const void*> g_armAnswer[kArmReads]{};  // each arm read's last answer (for the Debug line on a change)

// What a wrapper records for the read inside its call, on this thread (null: none, the read answers the game's own).
thread_local const void* t_combination      = nullptr;  // E1: a seat's member when the initiator's coin names that seat
thread_local const void* t_combinationEntry = nullptr;  // E2/E3: the combination entry being ticked
thread_local const void* t_boss             = nullptr;  // E4/E5: the boss controller
thread_local const void* t_causer           = nullptr;  // E6: the causer, when it is a seat's member
thread_local const void* t_mine             = nullptr;  // E7: set by the part getter, taken (once) by the type read
thread_local const void* t_arms             = nullptr;  // E8: the arms state object

bool On(Item item) noexcept
{
    return g_on.load(std::memory_order_acquire) && g_itemOn[item].load(std::memory_order_relaxed);
}

/// The item may answer: on, and a seat has a member and the players' types are known.
bool Active(Item item) noexcept
{
    if (!On(item) || !equal_targeting::Types().has_value())
    {
        return false;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (players::generation::MemberOf(seat) != nullptr)
        {
            return true;
        }
    }
    return false;
}

/// The seat a coin names (its player number; coin_table.h), or 0 for player 1 or no coin.
int SeatOfCoin(int coin) noexcept
{
    return coin > 1 ? coin_table::SeatOfNumber(coin) : 0;
}

/// "player 1" / "player 2" / "player 3" for a coin naming a player (coin 1 or more).
const wchar_t* CoinName(int coin) noexcept
{
    return players::SeatName(coin_table::SeatOfNumber(coin));
}

void NoteFirst(Item item) noexcept
{
    bool seen = false;
    if (g_firstSeen[item].compare_exchange_strong(seen, true, std::memory_order_relaxed))
    {
        platform::LogDebug(L"leader reads: the first answer for {} runs on {} (thread {})", kItemNames[item],
                           players::generation::OnGameThread() ? L"the game thread" : L"another thread", GetCurrentThreadId());
    }
}

/// The character a coin names in place of the game's leader: the seat's member for a seat; for player 1, the standing
/// member enemies follow while player 1's character is down (battle/enemy_leader); else null (the game's own leader).
void* CoinCharacter(int coin) noexcept
{
    const int seat = SeatOfCoin(coin);
    return seat != 0 ? players::generation::MemberOf(seat) : coin == 1 ? enemy_leader::Instead() : nullptr;
}

/// The answer to a leader read made for `controller` (a checked enemy controller, or null): the character its coin
/// names in place of the game's leader, else the game's own.
std::uint64_t CoinAnswer(const void* controller, CallFn original, std::uint64_t a, std::uint64_t b, std::uint64_t c,
                         std::uint64_t d, int& coin) noexcept
{
    coin                     = controller != nullptr ? target_coin::CoinFor(controller) : 0;
    void* const character    = CoinCharacter(coin);
    return character != nullptr ? reinterpret_cast<std::uint64_t>(character) : original(a, b, c, d);
}

// ---- E1-E3 enemy combinations ----

/// The pair's registration: the initiator's coin, shared with the partner; held for the start's leader read.
std::uint64_t RegisterPair(std::uint64_t a, std::uint64_t enemyA, std::uint64_t enemyB, std::uint64_t request,
                           std::uint64_t target) noexcept
{
    const auto original = reinterpret_cast<Call5Fn>(g_registerPair.Original());
    if (!Active(kCombination))
    {
        return original(a, enemyA, enemyB, request, target);
    }
    const void* const initiator = target_coin::ControllerOfEnemy(reinterpret_cast<const void*>(enemyA));
    const void* const partner   = target_coin::ControllerOfEnemy(reinterpret_cast<const void*>(enemyB));
    const int         coin      = initiator != nullptr ? target_coin::CoinFor(initiator) : 0;
    if (coin != 0 && partner != nullptr)
    {
        target_coin::ShareCoin(initiator, partner);
    }
    if (coin == 0)
    {
        platform::LogDebug(L"leader reads: combination of enemies {:#x} and {:#x} registered: no coin (the game's own "
                           L"leader)",
                           enemyA, enemyB);
    }
    else if (partner == nullptr)
    {
        platform::LogDebug(L"leader reads: combination of enemies {:#x} and {:#x} registered: {}; the partner's "
                           L"controller did not check out",
                           enemyA, enemyB, CoinName(coin));
    }
    else
    {
        platform::LogDebug(L"leader reads: combination of enemies {:#x} and {:#x} registered: both count {} as the player",
                           enemyA, enemyB, CoinName(coin));
    }
    return scopes::thread_hold::Holding(t_combination, CoinCharacter(coin), original, a, enemyA, enemyB, request, target);
}

/// Who a character a read answers is, for the log: a seat's player's, or the standing AI ally standing in for player 1's.
const wchar_t* WhoseCharacter(const void* character) noexcept
{
    const int seat = players::generation::SeatOfMember(character);
    return players::SeatBuilt(seat) ? players::SeatName(seat) : L"the standing AI ally";
}

/// The start's leader read (ForceTargetLeader rows): the registration's answer.
std::uint64_t StartLeader(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) noexcept
{
    if (const void* const seated = t_combination; seated != nullptr)
    {
        NoteFirst(kCombination);
        platform::LogDebug(L"leader reads: a combination starts on {}'s character", WhoseCharacter(seated));
        return reinterpret_cast<std::uint64_t>(seated);
    }
    return reinterpret_cast<CallFn>(g_startLeader.Original())(a, b, c, d);
}

/// A combination's mode, every tick per combination: records the entry for the read (no coin work here).
template <platform::EntryRedirect& Entry>
std::uint64_t Mode(std::uint64_t manager, std::uint64_t entry, float dt) noexcept
{
    const auto original = reinterpret_cast<ModeFn>(Entry.Original());
    if (!On(kCombination))
    {
        return original(manager, entry, dt);
    }
    return scopes::thread_hold::Holding(t_combinationEntry, reinterpret_cast<const void*>(entry), original, manager, entry, dt);
}

/// A mode's leader read (once per combination, when both enemies are ready): the initiator's coin, shared again with the
/// partner (either may have tossed at an action since the registration).
template <platform::CallRedirect& Call>
std::uint64_t ModeLeader(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) noexcept
{
    const auto        original = reinterpret_cast<CallFn>(Call.Original());
    const auto* const entry    = static_cast<const std::byte*>(t_combinationEntry);
    if (entry == nullptr || !Active(kCombination))
    {
        return original(a, b, c, d);
    }
    const auto indexA = ReadField<std::uint8_t>(entry, kEntryIndexA);
    const auto indexB = ReadField<std::uint8_t>(entry, kEntryIndexB);
    if (indexA > 1 || indexB > 1)  // the registration writes 0 and 1: anything else is not the layout we know
    {
        return original(a, b, c, d);
    }
    const void* enemyA    = scopes::leader_scope::ObjectAt(entry + kEntryEnemies + 8u * indexA);
    const void* enemyB    = scopes::leader_scope::ObjectAt(entry + kEntryEnemies + 8u * indexB);
    const void* initiator = target_coin::ControllerOfEnemy(enemyA);
    const void* partner   = target_coin::ControllerOfEnemy(enemyB);
    int         coin      = 0;
    const auto  answer    = CoinAnswer(initiator, original, a, b, c, d, coin);
    if (coin != 0 && partner != nullptr)
    {
        target_coin::ShareCoin(initiator, partner);
    }
    NoteFirst(kCombination);
    if (coin == 0)
    {
        platform::LogDebug(L"leader reads: combination of enemies {:#x} and {:#x} aims at the game's own leader (no coin)",
                           reinterpret_cast<std::uintptr_t>(enemyA), reinterpret_cast<std::uintptr_t>(enemyB));
    }
    else
    {
        platform::LogDebug(L"leader reads: combination of enemies {:#x} and {:#x} aims at {} (the initiator's coin)",
                           reinterpret_cast<std::uintptr_t>(enemyA), reinterpret_cast<std::uintptr_t>(enemyB),
                           CoinName(coin));
    }
    return answer;
}

// ---- E4 / E5 a boss's damage-phase reactions ----

/// The boss controller's actor Tick, every frame: records the boss for the timed reaction's read.
std::uint64_t BossTick(std::uint64_t controller, float dt) noexcept
{
    const auto original = reinterpret_cast<TickFn>(g_bossTick.Original());
    if (!On(kTimedPhase))
    {
        return original(controller, dt);
    }
    return scopes::thread_hold::Holding(t_boss, reinterpret_cast<const void*>(controller), original, controller, dt);
}

/// After a summon's cut scene (once): records the boss for its read.
std::uint64_t AfterSummon(std::uint64_t controller, std::uint64_t b, std::uint64_t c, std::uint64_t d) noexcept
{
    const auto original = reinterpret_cast<CallFn>(g_afterSummon.Original());
    if (!On(kAfterSummon))
    {
        return original(controller, b, c, d);
    }
    return scopes::thread_hold::Holding(t_boss, reinterpret_cast<const void*>(controller), original, controller, b, c, d);
}

/// The two damage-phase reads (each once per phase change / cut scene): the boss's coin.
template <platform::CallRedirect& Call, Item I>
std::uint64_t PhaseLeader(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) noexcept
{
    const auto original = reinterpret_cast<CallFn>(Call.Original());
    const void* const boss = t_boss;
    if (boss == nullptr || !Active(I))
    {
        return original(a, b, c, d);
    }
    int        coin   = 0;
    const auto answer = CoinAnswer(target_coin::CheckedController(boss), original, a, b, c, d, coin);
    NoteFirst(I);
    if (coin == 0)
    {
        platform::LogDebug(L"leader reads: {} for boss controller {:#x}: the reaction's source is the game's own leader "
                           L"(no coin)",
                           kItemNames[I], reinterpret_cast<std::uintptr_t>(boss));
    }
    else
    {
        platform::LogDebug(L"leader reads: {} for boss controller {:#x}: the reaction's source is {} (the boss's coin)",
                           kItemNames[I], reinterpret_cast<std::uintptr_t>(boss), CoinName(coin));
    }
    return answer;
}

// ---- E6 the reaction player-type filter ----

/// One call of the filter: when the causer is a seat's member, or the member standing in for player 1's downed
/// character (battle/enemy_leader), the leader read answers the causer itself, so it passes "the player" (UCPC) and
/// fails "an AI member" (AIPC), as player 1's attacks do.
template <platform::CallRedirect& Call>
std::uint64_t Filter(std::uint64_t manager, std::uint64_t causer, std::uint64_t info, std::uint64_t lists) noexcept
{
    const auto        original = reinterpret_cast<CallFn>(Call.Original());
    const auto* const by       = reinterpret_cast<const void*>(causer);
    const bool        player   = by != nullptr && (players::SeatBuilt(players::generation::SeatOfMember(by)) ||
                                                by == enemy_leader::Instead());
    if (!player || !Active(kReactionFilter))
    {
        return original(manager, causer, info, lists);
    }
    return scopes::thread_hold::Holding(t_causer, by, original, manager, causer, info, lists);
}

/// The filter's leader read (reached only past the filter's own early-outs): the held causer.
std::uint64_t FilterLeader(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) noexcept
{
    if (const void* const causer = t_causer; causer != nullptr)
    {
        NoteFirst(kReactionFilter);
        platform::LogDebug(L"leader reads: a reaction filter tests {}'s character; it counts as the player",
                           WhoseCharacter(causer));
        return reinterpret_cast<std::uint64_t>(causer);
    }
    return reinterpret_cast<CallFn>(g_filterLeader.Original())(a, b, c, d);
}

// ---- E7 Reno's mines ----

/// SetState's first call after its state gate: records the mine for the type read further on (set only; the read takes
/// it, and the next SetState overwrites a value left by one that returned before the read).
std::uint64_t MinePart(std::uint64_t mine, std::uint64_t b, std::uint64_t c, std::uint64_t d) noexcept
{
    t_mine = On(kMine) ? reinterpret_cast<const void*>(mine) : nullptr;
    return reinterpret_cast<CallFn>(g_minePart.Original())(mine, b, c, d);
}

/// The leader-type read of a mine without a target: a seat's member's type while the mine's own coin names that seat,
/// and only when that type's character is the seat's live member (the game dereferences that character without a null
/// check).
std::uint64_t MineLeaderType(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) noexcept
{
    const auto        original = reinterpret_cast<CallFn>(g_mineType.Original());
    const void* const mine     = t_mine;
    t_mine                     = nullptr;
    if (mine == nullptr || !Active(kMine) || ReadField<std::uint8_t>(mine, kMineState) >= kMineStates)
    {
        return original(a, b, c, d);
    }
    const auto* const object     = static_cast<const std::byte*>(ReadPointer(mine, kMineObject));
    const void* const controller = object != nullptr
                                       ? target_coin::CheckedController(scopes::leader_scope::ObjectAt(object + kMineObjectController))
                                       : nullptr;
    const int    coin  = controller != nullptr ? target_coin::CoinFor(controller) : 0;
    const int    seat  = SeatOfCoin(coin);
    const auto   types = equal_targeting::Types();
    // The character the coin names in place of the game's leader, and its type: a seat's member, or for player 1 the
    // member standing in while player 1's character is down.
    std::uint8_t type      = seat != 0 && types && types->Has(seat) ? types->type[seat] : facts::kNoPlayerType;
    const void*  character = seat != 0 ? players::generation::MemberOf(seat) : coin == 1 ? enemy_leader::Instead(&type) : nullptr;
    void*        found     = nullptr;
    const bool   live      = character != nullptr && type < facts::kNoPlayerType && g_characterByType != nullptr &&
                      platform::CallGuarded(g_characterByType, found, std::uint8_t{0}, type) && found == character;
    NoteFirst(kMine);
    const auto at = reinterpret_cast<std::uintptr_t>(mine);
    if (live)
    {
        platform::LogDebug(L"leader reads: Reno's mine {:#x} heads for {}'s character (its coin)", at,
                           WhoseCharacter(character));
    }
    else if (seat != 0)
    {
        platform::LogDebug(L"leader reads: Reno's mine {:#x} heads for player 1 (its coin named {}, whose member is not "
                           L"live)",
                           at, players::SeatName(seat));
    }
    else
    {
        platform::LogDebug(L"leader reads: Reno's mine {:#x} heads for {}", at,
                           coin == 1 ? L"player 1 (its coin)" : L"the game's own leader (no coin)");
    }
    return live ? static_cast<std::uint64_t>(type) : original(a, b, c, d);
}

// ---- E8 Crimson Mare's arms ----

std::uint64_t ArmsBegin(std::uint64_t arms, std::uint64_t b, std::uint64_t c, std::uint64_t d, std::uint64_t e) noexcept
{
    const auto original = reinterpret_cast<Call5Fn>(g_armsBegin.Original());
    if (!On(kArms))
    {
        return original(arms, b, c, d, e);
    }
    return scopes::thread_hold::Holding(t_arms, reinterpret_cast<const void*>(arms), original, arms, b, c, d, e);
}

std::uint64_t ArmsTick(std::uint64_t arms, float dt) noexcept
{
    const auto original = reinterpret_cast<TickFn>(g_armsTick.Original());
    if (!On(kArms))
    {
        return original(arms, dt);
    }
    return scopes::thread_hold::Holding(t_arms, reinterpret_cast<const void*>(arms), original, arms, dt);
}

std::uint64_t PurgeArm(std::uint64_t arms, std::uint64_t b, std::uint64_t c, std::uint64_t d) noexcept
{
    const auto original = reinterpret_cast<CallFn>(g_purgeArm.Original());
    if (!On(kArms))
    {
        return original(arms, b, c, d);
    }
    return scopes::thread_hold::Holding(t_arms, reinterpret_cast<const void*>(arms), original, arms, b, c, d);
}

/// One of the three arm reads: the boss's coin (the first runs every frame for each waiting arm: Trace per answer,
/// Debug when its answer changes).
template <std::size_t I>
std::uint64_t ArmLeader(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) noexcept
{
    const auto        original = reinterpret_cast<CallFn>(g_armRead[I].Original());
    const auto* const arms     = static_cast<const std::byte*>(t_arms);
    if (arms == nullptr || !Active(kArms))
    {
        return original(a, b, c, d);
    }
    const void* const boss   = scopes::leader_scope::ObjectAt(arms + kArmsBoss);
    int               coin   = 0;
    const auto        answer = CoinAnswer(target_coin::ControllerOfEnemy(boss), original, a, b, c, d, coin);
    NoteFirst(kArms);
    const auto* const now  = reinterpret_cast<const void*>(answer);
    const void*       last = g_armAnswer[I].exchange(now, std::memory_order_relaxed);
    if (last != now)
    {
        if (coin == 0)
        {
            platform::LogDebug(L"leader reads: Crimson Mare's arm read {} now names the game's own leader (no coin)",
                               I + 1);
        }
        else
        {
            platform::LogDebug(L"leader reads: Crimson Mare's arm read {} now names {} (the boss's coin)", I + 1,
                               CoinName(coin));
        }
    }
    platform::LogTrace(L"leader reads: Crimson Mare's arm read {}: coin {}", I + 1, coin);
    return answer;
}

// ---- install ----

template <class F>
bool PlaceCall(const platform::GameImage& image, platform::CallRedirect& call, std::uintptr_t site,
               std::optional<std::uintptr_t> target, F* replacement) noexcept
{
    const auto live = image.Rebase(site, 5);
    // on_unreal_init: no battle yet, so no thread can be at a crossing call (InstallFromOwnThread's rule; one that fits
    // its block is installed as by Install).
    return live && target && call.InstallFromOwnThread(*live, *target, reinterpret_cast<std::uintptr_t>(replacement));
}

template <class F>
bool PlaceMode(const platform::GameImage& image, platform::EntryRedirect& entry, const platform::FunctionFact& mode,
               F* replacement) noexcept
{
    const auto live = platform::FindFunction(image, mode);
    return live && entry.Install(*live, image.EntryInstruction(mode.address, mode.entryBytes.first(kModeFirst)),
                                 reinterpret_cast<std::uintptr_t>(replacement));
}

template <class F>
bool PlaceSlot(const platform::GameImage& image, const platform::SlotFact& fact, platform::VTableEntry& entry,
               F* replacement) noexcept
{
    return platform::ReplaceSlot(image, fact, reinterpret_cast<std::uintptr_t>(replacement), entry);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto leader          = platform::FindFunction(image, facts::kResolvePartyLeader);
    const auto characterByType = platform::FindFunction(image, facts::kCharacterByPlayerType);  // Reno's mines only
    if (!leader || !scopes::leader_scope::Installed())
    {
        platform::Log(L"leader reads: UNAVAILABLE (the leader resolver was not found); these enemy reads keep player 1");
        return false;
    }
    g_characterByType = characterByType ? reinterpret_cast<CharacterByTypeFn>(*characterByType) : nullptr;

    // Each item's levers together; an item with one missing stays off (what was placed passes through: wrappers act
    // only while their item is on, and a read answers only what a wrapper of its own item recorded).
    bool placed[kItems] = {};
    placed[kCombination] =
        PlaceCall(image, g_registerPair, kRegisterPairCall, image.Rebase(kRegisterPair), &RegisterPair) &&
        PlaceCall(image, g_startLeader, kStartLeaderCall, leader, &StartLeader) &&
        PlaceCall(image, g_mode1Leader, kMode1LeaderCall, leader, &ModeLeader<g_mode1Leader>) &&
        PlaceCall(image, g_mode2Leader, kMode2LeaderCall, leader, &ModeLeader<g_mode2Leader>) &&
        PlaceMode(image, g_mode1, kMode1, &Mode<g_mode1>) && PlaceMode(image, g_mode2, kMode2, &Mode<g_mode2>);
    placed[kTimedPhase] =
        PlaceCall(image, g_phaseLeader, kPhaseLeaderCall, leader, &PhaseLeader<g_phaseLeader, kTimedPhase>) &&
        PlaceSlot(image, kBossTickSlot, g_bossTick, &BossTick);
    placed[kAfterSummon] = PlaceCall(image, g_afterSummonLeader, kAfterSummonLeaderCall, leader,
                                     &PhaseLeader<g_afterSummonLeader, kAfterSummon>) &&
                           PlaceSlot(image, kAfterSummonSlot, g_afterSummon, &AfterSummon);
    placed[kReactionFilter] = PlaceCall(image, g_filterLeader, kFilterLeaderCall, leader, &FilterLeader) &&
                       PlaceCall(image, g_filterCall0, kFilterCall[0], image.Rebase(kFilter), &Filter<g_filterCall0>) &&
                       PlaceCall(image, g_filterCall1, kFilterCall[1], image.Rebase(kFilter), &Filter<g_filterCall1>) &&
                       PlaceCall(image, g_filterCall2, kFilterCall[2], image.Rebase(kFilter), &Filter<g_filterCall2>);
    placed[kMine] = g_characterByType != nullptr &&
                    PlaceCall(image, g_mineType, kMineTypeCall, image.Rebase(kGetPartyLeaderType.address),
                              &MineLeaderType) &&
                    PlaceCall(image, g_minePart, kMinePartCall, image.Rebase(kMinePart), &MinePart);
    placed[kArms] = PlaceCall(image, g_armRead[0], kArmReadCall[0], leader, &ArmLeader<0>) &&
                    PlaceCall(image, g_armRead[1], kArmReadCall[1], leader, &ArmLeader<1>) &&
                    PlaceCall(image, g_armRead[2], kArmReadCall[2], leader, &ArmLeader<2>) &&
                    PlaceCall(image, g_purgeArm, kPurgeArmCall, image.Rebase(kPurgeArm), &PurgeArm) &&
                    PlaceSlot(image, kArmsBeginSlot, g_armsBegin, &ArmsBegin) &&
                    PlaceSlot(image, kArmsTickSlot, g_armsTick, &ArmsTick);

    std::size_t on = 0;
    for (std::size_t i = 0; i < kItems; ++i)
    {
        g_itemOn[i].store(placed[i], std::memory_order_relaxed);
        on += placed[i] ? 1 : 0;
        if (!placed[i])
        {
            platform::Log(L"leader reads: {} UNAVAILABLE (a lever differs); they keep player 1", kItemNames[i]);
        }
    }
    g_on.store(on != 0, std::memory_order_release);
    platform::Log(L"leader reads: {} of {} enemy leader reads follow the enemy's coin (combinations, damage-phase "
                  L"reactions, the reaction player-type filter: every player counts, Reno's mines, Crimson Mare's arms)",
                  on, static_cast<std::size_t>(kItems));
    return on != 0;
}

void Remove() noexcept
{
    g_on.store(false, std::memory_order_release);  // from now on every wrapper and read passes through
    for (auto* slot : {&g_bossTick, &g_afterSummon, &g_armsBegin, &g_armsTick})
    {
        if (slot->Original() != 0 && !slot->Restore())
        {
            platform::Log(L"leader reads: a slot was changed after us; left in place (it passes through)");
        }
    }
    for (auto* entry : {&g_mode1, &g_mode2})
    {
        if (entry->IsInstalled() && !entry->Restore())
        {
            platform::Log(L"leader reads: a combination mode's entry was changed after us; left in place (it passes "
                          L"through)");
        }
    }
    // The call redirects stay: several cross a block, which only the one thread that can run them may put back, and
    // unloading is not that thread. Every one passes through once off.
}

}  // namespace lss::battle::leader_reads
