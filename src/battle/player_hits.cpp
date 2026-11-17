#include "battle/player_hits.h"

#include "battle/equal_targeting.h"
#include "facts/party.h"
#include "platform/call_redirect.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/generation.h"
#include "players/seats.h"
#include "scopes/leader_answer.h"
#include "scopes/thread_hold.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace lss::battle::player_hits {

namespace {

// Facts read from the database (research/RE-p2-enemy-target-bias.md; this build). 1.005: replaced-player-hits.json (the
// damage slot is +0x388 there).

// The seven damage functions, each OnTakeDamage(component, attacker, hit, damage, a5): five integer arguments, no float
// (all seven prototypes, both builds), so all five are passed on.
constexpr std::uint8_t kBaseBytes[]   = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41,
                                         0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC};
constexpr std::uint8_t kEnemyBytes[]  = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48,
                                         0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48};
constexpr std::uint8_t kPlayerBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                         0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
constexpr std::uint8_t kMoogleBytes[] = {0xB8, 0x38, 0x00, 0x00, 0x00, 0xE8, 0xCA, 0x3B,
                                         0x03, 0xFF, 0x48, 0x2B, 0xE0, 0x41, 0x80, 0x79};
constexpr std::uint8_t kSummonBytes[] = {0xB8, 0x38, 0x00, 0x00, 0x00, 0xE8, 0xA6, 0xD5,
                                         0x02, 0xFF, 0x48, 0x2B, 0xE0, 0x48, 0x83, 0xC4};
constexpr std::uint8_t kCondorBytes[] = {0x40, 0x53, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8,
                                         0x8C, 0xD5, 0x02, 0xFF, 0x48, 0x2B, 0xE0, 0x48};
constexpr std::uint8_t kGatedBytes[]  = {0x48, 0x85, 0xD2, 0x0F, 0x84, 0x7C, 0x02, 0x00,
                                         0x00, 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08};

constexpr platform::FunctionFact kBase{L"BattleCtrlComp_ApplyTakeDamage_FireAIEvents", 0x14152024C, kBaseBytes};
constexpr platform::FunctionFact kEnemy{L"EnemyBattleComp_Vt380_OnTakeDamage", 0x1412E1DE8, kEnemyBytes};
constexpr platform::FunctionFact kPlayer{L"BattlePlayerComp_Vt380_OnTakeDamage", 0x1412E05D4, kPlayerBytes};
constexpr platform::FunctionFact kMoogle{L"FatMoogleComp_Vt380_OnTakeDamage_IfNotHeal", 0x143137FAC, kMoogleBytes};
constexpr platform::FunctionFact kSummon{L"SummonComp_Vt380_OnTakeDamage", 0x14313E5D0, kSummonBytes};
constexpr platform::FunctionFact kCondor{L"CondorFortComp_Vt380_OnTakeDamage", 0x14313E5E8, kCondorBytes};
constexpr platform::FunctionFact kGated{L"BattleComp2_Vt380_OnTakeDamage_Gated", 0x14313E640, kGatedBytes};

// Every vtable entry holding one of them (the only references to the six overrides; the base is otherwise called only
// by the overrides as their parent, so it runs inside a wrapped call).
constexpr std::size_t        kSlots      = 10;
constexpr std::size_t        kPlayerSlot = 2;  // a member's own component: the battle-player slot, where a human is the victim
constexpr platform::SlotFact kSlotFacts[kSlots] = {
    {L"the Enemy battle component's damage slot", 0x1455FEBB0, kEnemy},
    {L"the EnemyEN2228 battle component's damage slot", 0x145B4DDD8, kEnemy},
    {L"the BattlePlayer component's damage slot", 0x145611AC8, kPlayer},
    {L"the PlayerBase battle component's damage slot", 0x1455FE270, kBase},
    {L"the Friend battle component's damage slot", 0x1456BA658, kBase},
    {L"the Battle battle component's damage slot", 0x145B4D5D0, kBase},
    {L"the PlayerBase2 battle component's damage slot", 0x1456B9DE8, kMoogle},
    {L"the Summon battle component's damage slot", 0x14576FCA8, kSummon},
    {L"the CondorFort battle component's damage slot", 0x146545818, kCondor},
    {L"the Battle2 battle component's damage slot", 0x146544FF8, kGated},
};
constexpr const wchar_t* kSlotNames[kSlots] = {L"Enemy",    L"EnemyEN2228", L"BattlePlayer", L"PlayerBase",
                                               L"Friend",   L"Battle",      L"PlayerBase2",  L"Summon",
                                               L"CondorFort", L"Battle2"};

// The summon gauge's one leader read (EnemyBattleComp_SummonGauge_AddOnHit_LeaderPtr: attacker == leader → the
// "Player" charge scale, else "Guest"), called from the Enemy override after the base. Inside one 16-byte block in both
// builds; no branch lands inside it.
constexpr std::uintptr_t kLeaderReadCall = 0x1412E19B1;

// The enemy AI's reaction sensing (AIBrainReactionSensing_Vt20_Tick): its two blueprint events, whose second argument
// is the attacking character. The first call is inside one 16-byte block in both builds; the second crosses one in
// both (InstallFromOwnThread, at on_unreal_init). No branch lands inside either.
constexpr std::uintptr_t kSensingCall     = 0x140A58971;  // OnReactionSensingInvoke(ctrl, causer, handle, type, dir)
constexpr std::uintptr_t kSensing         = 0x1417F86E0;
constexpr std::uintptr_t kTakeAbilityCall = 0x140A589AF;  // OnReactionSensingTakeAbilityInvoke(ctrl, causer, handle)
constexpr std::uintptr_t kTakeAbility     = 0x141850EBC;

using DamageFn = std::uint64_t (*)(std::uint64_t component, std::uint64_t attacker, std::uint64_t hit,
                                   std::uint64_t damage, std::uint64_t e);
using LeaderFn = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
using ResolveFn = void* (*)();

// Set once at install.
platform::VTableEntry  g_slots[kSlots];
platform::CallRedirect g_leaderRead, g_sensing, g_takeAbility;
ResolveFn              g_resolveLeader = nullptr;  // ResolvePartyLeaderWeakPtr: player 1's character

std::atomic<bool> g_on{false};

// A seat's character while its hit is applied on this thread (the summon gauge's leader read answers it), else null.
thread_local const void* t_attacker = nullptr;
// The human the damage or reaction call running on this thread is about (0 player 1, s a seat, kNoHuman).
thread_local int t_human = kNoHuman;

/// Who a damage or reaction call is about, and the answer it holds.
struct Involvement
{
    int         human    = kNoHuman;  // 0 player 1, s seat s
    bool        attacks  = false;     // the human is the attacker (else the character hit)
    int         type     = -1;        // the seat's member's player type: the leader test's extra yes (a seat only)
    const void* member   = nullptr;   // the seat's member (a seat only)
};

/// The human `attacker` is, or that `component` (on the battle-player slot only) belongs to: a seat's member, or
/// player 1's character (the party leader). Any thread: the types come from equal_targeting's per-frame copy.
Involvement Involved(std::uint64_t component, std::uint64_t attacker, bool victimSlot) noexcept
{
    const auto  types = equal_targeting::Types();
    const void* members[players::kMaxPlayers] = {};  // by seat; index 0 unused
    bool        anyMember                     = false;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        members[seat] = players::generation::MemberOf(seat);
        anyMember     = anyMember || members[seat] != nullptr;
    }
    if (!g_on.load(std::memory_order_acquire) || !types || !anyMember)
    {
        return {};
    }
    const auto* const by = reinterpret_cast<const void*>(attacker);
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (members[seat] != nullptr && by == members[seat] && types->Has(seat))
        {
            return {seat, true, types->type[seat], members[seat]};
        }
    }
    const void* const leader = g_resolveLeader != nullptr ? g_resolveLeader() : nullptr;
    if (by != nullptr && by == leader)
    {
        return {0, true, -1, nullptr};
    }
    if (victimSlot)
    {
        const int hit = scopes::leader_answer::ComponentPlayerType(reinterpret_cast<const void*>(component));
        for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
        {
            if (members[seat] != nullptr && types->Has(seat) && hit == types->type[seat])
            {
                return {seat, false, types->type[seat], members[seat]};
            }
        }
        if (hit == types->type[0])
        {
            return {0, false, -1, nullptr};
        }
    }
    return {};
}

/// A seat's answers for one call, put back however it leaves.
struct Held
{
    int         type;
    const void* attacker;
    int         human;
};

/// The call's answers held on this thread (scopes/thread_hold.h's enter): for a seat, the leader test's yes
/// for the seat's member's type and the seat's character for the summon gauge's leader read when it attacks; for any
/// human, the reaction context. Restore puts the outer answers back, also when a structured exception unwinds the call.
Held Hold(const Involvement& who) noexcept
{
    const Held outer{scopes::leader_answer::ExchangeThread(who.type), t_attacker, t_human};
    if (who.human < 1)
    {
        scopes::leader_answer::ExchangeThread(outer.type);  // player 1's calls keep any outer answer
    }
    t_attacker = who.human >= 1 && who.attacks ? who.member : nullptr;  // null: a nested call's own
    t_human    = who.human;
    return outer;
}

void Restore(const Held& outer) noexcept
{
    t_attacker = outer.attacker;
    t_human    = outer.human;
    scopes::leader_answer::ExchangeThread(outer.type);
}

template <std::size_t I>
std::uint64_t Damage(std::uint64_t component, std::uint64_t attacker, std::uint64_t hit, std::uint64_t damage,
                     std::uint64_t e) noexcept
{
    const auto original = reinterpret_cast<DamageFn>(g_slots[I].Original());
    if (!players::generation::OnGameThread())
    {
        return original(component, attacker, hit, damage, e);
    }
    const Involvement who = Involved(component, attacker, I == kPlayerSlot);
    if (who.human == kNoHuman)
    {
        return original(component, attacker, hit, damage, e);
    }
    if (who.human >= 1)
    {
        platform::LogDebug(L"player hits: {}'s character {} on {} {:#x}; {} counts as the player in this call",
                           players::SeatName(who.human), who.attacks ? L"hits" : L"is hit", kSlotNames[I], component,
                           players::SeatName(who.human));
    }
    else
    {
        platform::LogTrace(L"player hits: player 1's character {} on {} {:#x}; reactions aim at player 1",
                           who.attacks ? L"hits" : L"is hit", kSlotNames[I], component);
    }
    return scopes::thread_hold::HoldingBy(&Hold, &Restore, who, original, component, attacker, hit, damage, e);
}

/// The summon gauge's leader read: a seat's character while its hit is applied, else the game's leader.
std::uint64_t LeaderRead(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) noexcept
{
    if (const void* const attacker = t_attacker; attacker != nullptr)
    {
        platform::LogDebug(L"player hits: the summon gauge charges at the player's rate for {}'s hit",
                           players::SeatName(t_human));
        return reinterpret_cast<std::uint64_t>(attacker);
    }
    return reinterpret_cast<LeaderFn>(g_leaderRead.Original())(a, b, c, d);
}

/// One reaction-sensing event with the attacker's answers held (any thread). Five integer arguments, the fifth on the
/// stack (the attack direction), all passed on.
template <platform::CallRedirect& Call>
std::uint64_t Sensing(std::uint64_t controller, std::uint64_t causer, std::uint64_t handle, std::uint64_t type,
                      std::uint64_t direction) noexcept
{
    const auto        original = reinterpret_cast<DamageFn>(Call.Original());
    const Involvement who      = Involved(0, causer, false);
    if (who.human == kNoHuman)
    {
        return original(controller, causer, handle, type, direction);
    }
    if (who.human >= 1)
    {
        platform::LogTrace(L"player hits: an enemy senses {}'s attack; {} counts as the player",
                           players::SeatName(who.human), players::SeatName(who.human));
    }
    else
    {
        platform::LogTrace(L"player hits: an enemy senses player 1's attack; the game's own answers");
    }
    return scopes::thread_hold::HoldingBy(&Hold, &Restore, who, original, controller, causer, handle, type, direction);
}

constexpr DamageFn kWrappers[kSlots] = {&Damage<0>, &Damage<1>, &Damage<2>, &Damage<3>, &Damage<4>,
                                        &Damage<5>, &Damage<6>, &Damage<7>, &Damage<8>, &Damage<9>};

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    if (!scopes::leader_answer::Replaced() || !equal_targeting::Installed())
    {
        platform::Log(L"player hits: UNAVAILABLE (the leader-test slot or equal targeting is not in place); players 2 and 3's "
                      L"hits count as an AI ally's");
        return false;
    }
    const auto resolver = platform::FindFunction(image, facts::kResolvePartyLeader);
    g_resolveLeader     = resolver ? reinterpret_cast<ResolveFn>(*resolver) : nullptr;
    // The redirects first: they answer only inside a held call, so they change nothing until a slot is in place.
    const auto site  = image.Rebase(kLeaderReadCall, 5);
    const bool gauge = site && resolver &&
                       g_leaderRead.Install(*site, *resolver, reinterpret_cast<std::uintptr_t>(&LeaderRead));
    const auto sensingSite = image.Rebase(kSensingCall, 5);
    const auto sensing     = image.Rebase(kSensing);
    const auto takeSite    = image.Rebase(kTakeAbilityCall, 5);
    const auto take        = image.Rebase(kTakeAbility);
    // on_unreal_init: no world and no enemy exist yet, so no thread can be at the calls (InstallFromOwnThread's rule).
    const bool senses =
        sensingSite && sensing &&
        g_sensing.InstallFromOwnThread(*sensingSite, *sensing, reinterpret_cast<std::uintptr_t>(&Sensing<g_sensing>));
    const bool takes = takeSite && take &&
                       g_takeAbility.InstallFromOwnThread(*takeSite, *take,
                                                          reinterpret_cast<std::uintptr_t>(&Sensing<g_takeAbility>));
    g_on.store(true, std::memory_order_release);
    std::size_t slots = 0;
    for (std::size_t i = 0; i < kSlots; ++i)
    {
        if (platform::ReplaceSlot(image, kSlotFacts[i], reinterpret_cast<std::uintptr_t>(kWrappers[i]), g_slots[i]))
        {
            ++slots;
        }
        else
        {
            platform::Log(L"player hits: the {} damage slot keeps the game's own answers", kSlotNames[i]);
        }
    }
    if (slots == 0 && !senses && !takes)
    {
        g_on.store(false, std::memory_order_release);
        platform::Log(L"player hits: UNAVAILABLE (no damage slot or reaction call could be changed); players 2 and 3's hits "
                      L"count as an AI ally's");
        return false;
    }
    platform::Log(L"player hits: players 2 and 3's hits and the hits they take count as the player's ({} of {} damage "
                  L"slots); summon gauge at the player's rate {}; reaction sensing {}",
                  slots, kSlots, gauge ? L"on" : L"UNAVAILABLE (the leader read differs)",
                  senses && takes ? L"on" : (senses || takes ? L"partly on" : L"UNAVAILABLE"));
    return true;
}

void Remove() noexcept
{
    g_on.store(false, std::memory_order_release);  // every call from now on keeps the game's own answers
    for (std::size_t i = 0; i < kSlots; ++i)
    {
        if (g_slots[i].Original() != 0 && !g_slots[i].Restore())
        {
            platform::Log(L"player hits: the {} damage slot was changed after us; left in place (it passes through)",
                          kSlotNames[i]);
        }
    }
    for (auto* call : {&g_leaderRead, &g_sensing})
    {
        if (call->IsInstalled() && !call->Restore())
        {
            platform::Log(L"player hits: a redirected call was changed after us; left in place (it passes through)");
        }
    }
    // The TakeAbility call crosses a block in both builds: put back only from the one thread that can run it, which
    // unloading is not; it passes through once g_on is off.
}

int HumanInCall() noexcept
{
    return t_human;
}

}  // namespace lss::battle::player_hits
