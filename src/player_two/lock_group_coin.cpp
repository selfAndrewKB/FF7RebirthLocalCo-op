#include "player_two/lock_group_coin.h"

#include "facts/party.h"
#include "platform/call_redirect.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/equal_targeting.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/lock_group_rules.h"
#include "player_two/object_fields.h"
#include "player_two/target_coin.h"
#include "player_two/thread_hold.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace lss::player_two::lock_group_coin {

namespace {

// Facts read from the database (RE-p2-enemy-target-bias.md §18, S-LOCKGRP; this build). 1.005:
// replaced-lock-group-coin.json.

// The lock-group family's only two leader reads, each a call of ResolvePartyLeaderWeakPtr (UObject* (), the leader
// character; Pick requires an EndCharacter that is a key of its count map, i.e. in the group's TargetList).
constexpr std::uintptr_t kPickLeaderCall  = 0x1431EB28B;  // in LockGroup_PickTarget_PrioritizeUCPC; fits both builds
constexpr std::uintptr_t kCountLeaderCall = 0x1431DFC57;  // in LockGroup_CountLocksPerTarget; crosses a block on 1.005
// The rebalance, void(group*), the battle tick's per-group call before its re-picks (game thread; its only caller; the
// call crosses a block in both builds).
constexpr std::uintptr_t kRebalanceCall = 0x14295F34C;  // in LockGroups_TickPruneAndRepick
constexpr std::uintptr_t kRebalance     = 0x1431DAD5C;  // LockGroup_RebalanceLevelLocks
// The pick, bool Pick(group*, FWeakObjectPtr* out), from the battle tick's re-pick and the Blueprint registration
// (RE §8, S-TGTCENSUS; both sites fit one 16-byte block in both builds).
constexpr std::uint8_t kPickBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57,
                                       0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
constexpr platform::FunctionFact kPick{L"LockGroup_PickTarget_PrioritizeUCPC", 0x1431EB200, kPickBytes};
constexpr std::uintptr_t         kRepickCall   = 0x14295F387;  // in LockGroups_TickPruneAndRepick
constexpr std::uintptr_t         kRegisterCall = 0x143E42D05;  // in execRegisterTargetLockGroup

// Lock group (a TMap value, both builds): +0 TArray<member, 20 B> {data, num}; member +0 FWeakObjectPtr enemy,
// +8 FWeakObjectPtr target (none: no lock); +0x24 u8 bPrioritizeUCPC.
constexpr std::uint32_t kGroupMembers = 0x00, kGroupMembersNum = 0x08, kGroupPrioritize = 0x24;
constexpr std::uint32_t kMemberSize = 20, kMemberEnemy = 0x00, kMemberTarget = 0x08;
constexpr std::int32_t  kMemberCap  = 64;  // more members than this: not the layout we know

using CallFn    = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
using ResolveFn = void* (*)();

// Set once at install.
platform::CallRedirect g_repickCall, g_registerCall, g_pickLeader, g_countLeader, g_rebalance;
ResolveFn              g_resolveLeader = nullptr;  // ResolvePartyLeaderWeakPtr: player 1's character

std::atomic<bool>         g_on{false};
std::atomic<PickObserver> g_observer{nullptr};

// The character the two leader reads answer on this thread while one is held, else null (the game's leader).
thread_local const void* t_player = nullptr;
// The groups whose rebalance now holds player 2 (game thread; for the Debug line on a change). A battle has one group
// in Rebirth (Stun Ray's 'Default'); a fifth held group is still answered, only its changes go unlogged.
lock_group_rules::HeldGroups<4> g_heldGroups;

/// One of the two leader reads: the held character, else the game's leader.
template <platform::CallRedirect& Call>
std::uint64_t LeaderRead(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) noexcept
{
    if (const void* const player = t_player; player != nullptr)
    {
        return reinterpret_cast<std::uint64_t>(player);
    }
    return reinterpret_cast<CallFn>(Call.Original())(a, b, c, d);
}

/// The group's members array and count, or null when it is not the layout we know.
const std::byte* Members(const void* group, std::int32_t& num) noexcept
{
    num = ReadField<std::int32_t>(group, kGroupMembersNum);
    const auto* const data = static_cast<const std::byte*>(ReadPointer(group, kGroupMembers));
    return data != nullptr && num > 0 && num <= kMemberCap ? data : nullptr;
}

/// The member a pick is for: the first one without a live target (the tick picks for exactly that one; the
/// registration's new member is that one unless an older member also lost its target, RE §18.2).
const std::byte* PickedFor(const void* group) noexcept
{
    std::int32_t      num  = 0;
    const auto* const data = Members(group, num);
    for (std::int32_t i = 0; data != nullptr && i < num; ++i)
    {
        const std::byte* const member = data + static_cast<std::size_t>(i) * kMemberSize;
        if (leader_scope::ObjectAt(member + kMemberTarget) == nullptr)
        {
            return member;
        }
    }
    return nullptr;
}

const wchar_t* PickText(bool first) noexcept
{
    return first ? L"first pick" : L"re-pick";
}

/// The rebalance: holds player 2's member for CountLocks' leader read when player 2 holds a lock in this group and
/// player 1 holds none, so the leader-first respread runs only when neither human holds one. Game thread.
std::uint64_t Rebalance(std::uint64_t group, std::uint64_t b, std::uint64_t c, std::uint64_t d) noexcept
{
    const auto        original = reinterpret_cast<CallFn>(g_rebalance.Original());
    const auto* const self     = reinterpret_cast<const void*>(group);
    void* const       two      = generation::Member();
    if (!g_on.load(std::memory_order_acquire) || self == nullptr || two == nullptr || !equal_targeting::Types() ||
        ReadField<std::uint8_t>(self, kGroupPrioritize) == 0)
    {
        return original(group, b, c, d);
    }
    const void* const one     = g_resolveLeader != nullptr ? g_resolveLeader() : nullptr;
    bool              twoHolds = false, oneHolds = false;
    std::int32_t      num     = 0;
    const auto* const data    = Members(self, num);
    for (std::int32_t i = 0; data != nullptr && i < num; ++i)
    {
        const void* const target = leader_scope::ObjectAt(data + static_cast<std::size_t>(i) * kMemberSize + kMemberTarget);
        twoHolds                 = twoHolds || target == two;
        oneHolds                 = oneHolds || (target != nullptr && target == one);
    }
    const bool hold = lock_group_rules::RebalanceHoldsPlayerTwo(twoHolds, oneHolds);
    if (g_heldGroups.Note(self, hold))
    {
        platform::LogDebug(L"lock group coin: group {:#x}: {}", group,
                           hold ? L"player 2 holds a lock and player 1 none; the rebalance counts player 2 as the player"
                                : L"the rebalance counts player 1 as the player again");
    }
    if (!hold)
    {
        return original(group, b, c, d);
    }
    platform::LogTrace(L"lock group coin: group {:#x}: rebalance with player 2 as the player", group);
    return thread_hold::Holding(t_player, two, original, group, b, c, d);  // the leader reads answer player 2's member
}

/// One lock-group pick through the game's `pick`, with the picking enemy's coin naming "the player". `first`: the
/// registration's first pick (else the battle tick's re-pick), for the log. Off or without player 2: `pick` once.
std::uint64_t Pick(CallFn pick, std::uint64_t group, std::uint64_t out, std::uint64_t c, std::uint64_t d,
                   bool first) noexcept
{
    const auto* const self = reinterpret_cast<const void*>(group);
    void* const       two  = generation::Member();
    if (!g_on.load(std::memory_order_acquire) || self == nullptr || two == nullptr || !equal_targeting::Types() ||
        ReadField<std::uint8_t>(self, kGroupPrioritize) == 0)
    {
        return pick(group, out, c, d);
    }
    // The game's own pick first. The tick retries a failed pick every frame (no room: every target full, down or
    // untargetable), so a failure returns here with no toss and no log; a pick that can succeed is a decision.
    const std::uint64_t own = pick(group, out, c, d);
    if ((own & 0xFF) == 0)
    {
        return own;
    }
    const std::byte* const member     = PickedFor(self);
    const void* const      enemy      = member != nullptr ? leader_scope::ObjectAt(member + kMemberEnemy) : nullptr;
    const void* const      controller = target_coin::ControllerOfEnemy(enemy);  // checked before anything calls into it
    const int              coin       = controller != nullptr ? target_coin::TossForLockPick(controller) : 0;
    if (coin != 2)
    {
        platform::LogDebug(L"lock group coin: {} for enemy {:#x}: {}; the game's own term (player 1) decided",
                           PickText(first), reinterpret_cast<std::uintptr_t>(enemy),
                           coin == 1               ? L"its coin names player 1"
                           : member == nullptr     ? L"no member without a target"
                           : enemy == nullptr      ? L"the member's enemy is gone"
                           : controller == nullptr ? L"its controller did not check out"
                                                   : L"no coin (not an enemy controller, or player 2 cannot be targeted)");
        return own;
    }
    // Pick writes only `out` (RE §18.9), so it may run again: this time with player 2's member as "the player".
    const std::uint64_t held = thread_hold::Holding(t_player, two, pick, group, out, c, d);
    if ((held & 0xFF) != 0 && leader_scope::ObjectAt(reinterpret_cast<const void*>(out)) == two)
    {
        platform::LogDebug(L"lock group coin: {} for enemy {:#x}: its coin names player 2; it locks onto player 2's "
                           L"member",
                           PickText(first), reinterpret_cast<std::uintptr_t>(enemy));
        return held;
    }
    // Player 2 cannot take it (full, down, untargetable or not the least locked): the game's own pick stands, as single
    // player would for an unavailable player, and the enemy's coin follows it (its blueprint answers player 1).
    target_coin::LockPickFellBack(controller);
    platform::LogDebug(L"lock group coin: {} for enemy {:#x}: its coin named player 2, who cannot take it; the game's own "
                       L"term (player 1) decides",
                       PickText(first), reinterpret_cast<std::uintptr_t>(enemy));
    return pick(group, out, c, d);
}

/// One of the two pick call sites: the pick with the coin, then the observer.
template <platform::CallRedirect& Call, bool First>
std::uint64_t PickCall(std::uint64_t group, std::uint64_t out, std::uint64_t c, std::uint64_t d) noexcept
{
    const auto result = Pick(reinterpret_cast<CallFn>(Call.Original()), group, out, c, d, First);
    if (const PickObserver observer = g_observer.load(std::memory_order_relaxed); observer != nullptr)
    {
        observer(reinterpret_cast<const void*>(group), result, out, First);
    }
    return result;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto resolver      = platform::FindFunction(image, facts::kResolvePartyLeader);
    const auto rebalance     = image.Rebase(kRebalance);
    const auto pick          = platform::FindFunction(image, kPick);
    const auto repickSite    = image.Rebase(kRepickCall, 5);
    const auto registerSite  = image.Rebase(kRegisterCall, 5);
    const auto pickSite      = image.Rebase(kPickLeaderCall, 5);
    const auto countSite     = image.Rebase(kCountLeaderCall, 5);
    const auto rebalanceSite = image.Rebase(kRebalanceCall, 5);
    if (!resolver || !rebalance || !pick || !repickSite || !registerSite || !pickSite || !countSite || !rebalanceSite ||
        !leader_scope::Install(image))
    {
        platform::Log(L"lock group coin: UNAVAILABLE (a function or call site was not found); lock groups prefer "
                      L"player 1");
        return false;
    }
    g_resolveLeader = reinterpret_cast<ResolveFn>(*resolver);
    // All five together or none: a pick naming player 2 without the rebalance's answer would let the game release and
    // re-pick the group's locks every frame (RE §18.7.3), and the coin acts only through the two pick calls.
    // on_unreal_init: no battle yet, so no thread can be at the two crossing calls (InstallFromOwnThread's rule; a
    // reload of the mod in the middle of a battle would break it, as for the sibling modules' crossing calls).
    const bool pickCalls =
        g_repickCall.Install(*repickSite, *pick, reinterpret_cast<std::uintptr_t>(&PickCall<g_repickCall, false>)) &&
        g_registerCall.Install(*registerSite, *pick, reinterpret_cast<std::uintptr_t>(&PickCall<g_registerCall, true>));
    const bool picks = pickCalls && g_pickLeader.Install(*pickSite, *resolver,
                                                         reinterpret_cast<std::uintptr_t>(&LeaderRead<g_pickLeader>));
    const bool counts = picks && g_countLeader.InstallFromOwnThread(
                                     *countSite, *resolver, reinterpret_cast<std::uintptr_t>(&LeaderRead<g_countLeader>));
    const bool rebalances =
        counts && g_rebalance.InstallFromOwnThread(*rebalanceSite, *rebalance, reinterpret_cast<std::uintptr_t>(&Rebalance));
    if (!rebalances)
    {
        // Whatever was placed passes through (no answer is ever held while off).
        platform::Log(L"lock group coin: UNAVAILABLE (the {} call differs); lock groups prefer player 1",
                      !pickCalls ? L"pick" : !picks ? L"pick's leader" : (!counts ? L"lock count's leader" : L"rebalance"));
        return false;
    }
    g_on.store(true, std::memory_order_release);
    platform::Log(L"lock group coin: lock groups that prefer the player prefer player 1 or player 2 by each enemy's "
                  L"coin at its pick; the rebalance respreads only when neither player holds a lock");
    return true;
}

void Remove() noexcept
{
    g_on.store(false, std::memory_order_release);  // from now on no answer is held: every call passes through
    g_observer.store(nullptr, std::memory_order_relaxed);
    g_heldGroups.Clear();
    for (platform::CallRedirect* call : {&g_pickLeader, &g_repickCall, &g_registerCall})
    {
        if (call->IsInstalled() && !call->Restore())
        {
            platform::Log(L"lock group coin: a pick call was changed after us; left in place (it passes through)");
        }
    }
    // The lock count's read (on 1.005) and the rebalance call cross a block: put back only from the one thread that can
    // run them, which unloading is not; they pass through once off.
}

void SetPickObserver(PickObserver observer) noexcept
{
    g_observer.store(observer, std::memory_order_relaxed);
}

}  // namespace lss::player_two::lock_group_coin
