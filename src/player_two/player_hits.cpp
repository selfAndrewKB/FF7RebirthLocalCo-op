#include "player_two/player_hits.h"

#include "platform/call_redirect.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "player_two/engine_facts.h"
#include "player_two/equal_targeting.h"
#include "player_two/generation.h"
#include "player_two/member_movement.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace lss::player_two::player_hits {

namespace {

// Facts read from the database (RE-p2-enemy-target-bias.md §13-§14; this build). 1.005: replaced-player-hits.json (the
// damage slot is +0x388 there).

// The seven damage functions, each OnTakeDamage(component, attacker, hit, damage, a5): five integer arguments, no float
// (all seven prototypes, both builds), so all five are passed on.
constexpr std::uint8_t kBaseBytes[]   = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41,
                                         0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC};
constexpr std::uint8_t kEnemyBytes[]  = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48,
                                         0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48};
constexpr std::uint8_t kPlayerBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                         0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
constexpr std::uint8_t kMoogleBytes[] = {0xB8, 0x38, 0x00, 0x00, 0x00, 0xE8, 0x7A, 0x28,
                                         0xC9, 0xFE, 0x48, 0x2B, 0xE0, 0x41, 0x80, 0x79};
constexpr std::uint8_t kSummonBytes[] = {0xB8, 0x38, 0x00, 0x00, 0x00, 0xE8, 0x76, 0xCE,
                                         0xC8, 0xFE, 0x48, 0x2B, 0xE0, 0x48, 0x83, 0xC4};
constexpr std::uint8_t kCondorBytes[] = {0x40, 0x53, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8,
                                         0x5C, 0xCE, 0xC8, 0xFE, 0x48, 0x2B, 0xE0, 0x48};
constexpr std::uint8_t kGatedBytes[]  = {0x48, 0x85, 0xD2, 0x0F, 0x84, 0x7C, 0x02, 0x00,
                                         0x00, 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08};

constexpr platform::FunctionFact kBase{L"BattleCtrlComp_ApplyTakeDamage_FireAIEvents", 0x1418E6AC8, kBaseBytes};
constexpr platform::FunctionFact kEnemy{L"EnemyBattleComp_Vt380_OnTakeDamage", 0x140CB40D4, kEnemyBytes};
constexpr platform::FunctionFact kPlayer{L"BattlePlayerComp_Vt380_OnTakeDamage", 0x140CB27B4, kPlayerBytes};
constexpr platform::FunctionFact kMoogle{L"FatMoogleComp_Vt380_OnTakeDamage_IfNotHeal", 0x14341C6AC, kMoogleBytes};
constexpr platform::FunctionFact kSummon{L"SummonComp_Vt380_OnTakeDamage", 0x1434220B0, kSummonBytes};
constexpr platform::FunctionFact kCondor{L"CondorFortComp_Vt380_OnTakeDamage", 0x1434220C8, kCondorBytes};
constexpr platform::FunctionFact kGated{L"BattleComp2_Vt380_OnTakeDamage_Gated", 0x143422120, kGatedBytes};

// Every vtable entry holding one of them (the only references to the six overrides; the base is otherwise called only
// by the overrides as their parent, so it runs inside a wrapped call).
constexpr std::size_t        kSlots      = 10;
constexpr std::size_t        kPlayerSlot = 2;  // player 2's own component: the one slot where player 2 is the victim
constexpr platform::SlotFact kSlotFacts[kSlots] = {
    {L"the Enemy battle component's damage slot", 0x1457F1040, kEnemy},
    {L"the EnemyEN2228 battle component's damage slot", 0x1466B7678, kEnemy},
    {L"the BattlePlayer component's damage slot", 0x145796258, kPlayer},
    {L"the PlayerBase battle component's damage slot", 0x145795B40, kBase},
    {L"the Friend battle component's damage slot", 0x14580B078, kBase},
    {L"the Battle battle component's damage slot", 0x145CE3E58, kBase},
    {L"the PlayerBase2 battle component's damage slot", 0x14580BA48, kMoogle},
    {L"the Summon battle component's damage slot", 0x145902A48, kSummon},
    {L"the CondorFort battle component's damage slot", 0x1466B87F8, kCondor},
    {L"the Battle2 battle component's damage slot", 0x1466B7F08, kGated},
};
constexpr const wchar_t* kSlotNames[kSlots] = {L"Enemy",    L"EnemyEN2228", L"BattlePlayer", L"PlayerBase",
                                               L"Friend",   L"Battle",      L"PlayerBase2",  L"Summon",
                                               L"CondorFort", L"Battle2"};

// The summon gauge's one leader read (EnemyBattleComp_SummonGauge_AddOnHit_LeaderPtr: attacker == leader → the
// "Player" charge scale, else "Guest"), called from the Enemy override after the base. Inside one 16-byte block in both
// builds; no branch lands inside it.
constexpr std::uintptr_t kLeaderReadCall = 0x140CB3D51;

// The enemy AI's reaction sensing (AIBrainReactionSensing_Vt20_Tick): its two blueprint events, whose second argument
// is the attacking character. The first call is inside one 16-byte block in both builds; the second crosses one in
// both (InstallFromOwnThread, at on_unreal_init). No branch lands inside either.
constexpr std::uintptr_t kSensingCall     = 0x1409B7A91;  // OnReactionSensingInvoke(ctrl, causer, handle, type, dir)
constexpr std::uintptr_t kSensing         = 0x14176F28C;
constexpr std::uintptr_t kTakeAbilityCall = 0x1409B7ACF;  // OnReactionSensingTakeAbilityInvoke(ctrl, causer, handle)
constexpr std::uintptr_t kTakeAbility     = 0x1417B7008;

using DamageFn = std::uint64_t (*)(std::uint64_t component, std::uint64_t attacker, std::uint64_t hit,
                                   std::uint64_t damage, std::uint64_t e);
using LeaderFn = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
using ResolveFn = void* (*)();

// Set once at install.
platform::VTableEntry  g_slots[kSlots];
platform::CallRedirect g_leaderRead, g_sensing, g_takeAbility;
ResolveFn              g_resolveLeader = nullptr;  // ResolvePartyLeaderWeakPtr: player 1's character

std::atomic<bool> g_on{false};

// Player 2's character while its hit is applied on this thread (the summon gauge's leader read answers it), else null.
thread_local const void* t_attacker = nullptr;
// The human the damage or reaction call running on this thread is about.
thread_local Human t_human = Human::None;

/// Who a damage or reaction call is about, and the answer it holds.
struct Involvement
{
    Human       human    = Human::None;
    bool        attacks  = false;  // the human is the attacker (else the character hit)
    int         type     = -1;     // player 2's member's player type: the leader test's extra yes (player 2 only)
    const void* member   = nullptr;
};

/// The human `attacker` is, or that `component` (on the battle-player slot only) belongs to: player 2's member, or
/// player 1's character (the party leader). Any thread: the types come from equal_targeting's per-frame copy.
Involvement Involved(std::uint64_t component, std::uint64_t attacker, bool victimSlot) noexcept
{
    const auto types = equal_targeting::Types();
    const void* const member = generation::Member();
    if (!g_on.load(std::memory_order_acquire) || !types || member == nullptr)
    {
        return {};
    }
    const auto* const by = reinterpret_cast<const void*>(attacker);
    if (by == member)
    {
        return {Human::PlayerTwo, true, types->two, member};
    }
    const void* const leader = g_resolveLeader != nullptr ? g_resolveLeader() : nullptr;
    if (by != nullptr && by == leader)
    {
        return {Human::PlayerOne, true, -1, member};
    }
    if (victimSlot)
    {
        const int hit = member_movement::ComponentPlayerType(reinterpret_cast<const void*>(component));
        if (hit == types->two)
        {
            return {Human::PlayerTwo, false, types->two, member};
        }
        if (hit == types->one)
        {
            return {Human::PlayerOne, false, -1, member};
        }
    }
    return {};
}

/// Player 2's answers for one call, put back however it leaves.
struct Held
{
    int         type;
    const void* attacker;
    Human       human;
};

Held Hold(const Involvement& who) noexcept
{
    const Held outer{member_movement::ExchangeThreadLeaderAnswer(who.type), t_attacker, t_human};
    if (who.human != Human::PlayerTwo)
    {
        member_movement::ExchangeThreadLeaderAnswer(outer.type);  // player 1's calls keep any outer answer
    }
    t_attacker = who.human == Human::PlayerTwo && who.attacks ? who.member : nullptr;  // null: a nested call's own
    t_human    = who.human;
    return outer;
}

void Restore(const Held& outer) noexcept
{
    t_attacker = outer.attacker;
    t_human    = outer.human;
    member_movement::ExchangeThreadLeaderAnswer(outer.type);
}

/// The game's damage call with the call's answers held on this thread: for player 2, the leader test's yes for player
/// 2's type and player 2's character for the summon gauge's leader read when it attacks; for either human, the
/// reaction context. All put back in the __finally, which also runs when a structured exception unwinds through here
/// (destructors would not, under /EHsc), so a thread never keeps a stale answer. No object here needs unwinding (the
/// __try rule).
std::uint64_t CallHolding(DamageFn original, std::uint64_t component, std::uint64_t attacker, std::uint64_t hit,
                          std::uint64_t damage, std::uint64_t e, const Involvement& who) noexcept
{
    const Held    outer  = Hold(who);
    std::uint64_t result = 0;
    __try
    {
        result = original(component, attacker, hit, damage, e);
    }
    __finally
    {
        Restore(outer);
    }
    return result;
}

const wchar_t* HumanText(Human human) noexcept
{
    return human == Human::PlayerTwo ? L"player 2's" : L"player 1's";
}

template <std::size_t I>
std::uint64_t Damage(std::uint64_t component, std::uint64_t attacker, std::uint64_t hit, std::uint64_t damage,
                     std::uint64_t e) noexcept
{
    const auto original = reinterpret_cast<DamageFn>(g_slots[I].Original());
    if (!generation::OnGameThread())
    {
        return original(component, attacker, hit, damage, e);
    }
    const Involvement who = Involved(component, attacker, I == kPlayerSlot);
    if (who.human == Human::None)
    {
        return original(component, attacker, hit, damage, e);
    }
    if (who.human == Human::PlayerTwo)
    {
        platform::LogDebug(L"player hits: player 2's character {} on {} {:#x}; player 2 counts as the player in this "
                           L"call",
                           who.attacks ? L"hits" : L"is hit", kSlotNames[I], component);
    }
    else
    {
        platform::LogTrace(L"player hits: player 1's character {} on {} {:#x}; reactions aim at player 1",
                           who.attacks ? L"hits" : L"is hit", kSlotNames[I], component);
    }
    return CallHolding(original, component, attacker, hit, damage, e, who);
}

/// The summon gauge's leader read: player 2's character while its hit is applied, else the game's leader.
std::uint64_t LeaderRead(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) noexcept
{
    if (const void* const attacker = t_attacker; attacker != nullptr)
    {
        platform::LogDebug(L"player hits: the summon gauge charges at the player's rate for player 2's hit");
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
    if (who.human == Human::None)
    {
        return original(controller, causer, handle, type, direction);
    }
    platform::LogTrace(L"player hits: an enemy senses {} attack; {}", HumanText(who.human),
                       who.human == Human::PlayerTwo ? L"player 2 counts as the player" : L"the game's own answers");
    return CallHolding(original, controller, causer, handle, type, direction, who);
}

constexpr DamageFn kWrappers[kSlots] = {&Damage<0>, &Damage<1>, &Damage<2>, &Damage<3>, &Damage<4>,
                                        &Damage<5>, &Damage<6>, &Damage<7>, &Damage<8>, &Damage<9>};

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    if (!member_movement::LeaderTestReplaced() || !equal_targeting::Installed())
    {
        platform::Log(L"player hits: UNAVAILABLE (the leader-test slot or equal targeting is not in place); player 2's "
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
        platform::Log(L"player hits: UNAVAILABLE (no damage slot or reaction call could be changed); player 2's hits "
                      L"count as an AI ally's");
        return false;
    }
    platform::Log(L"player hits: player 2's hits and the hits player 2 takes count as the player's ({} of {} damage "
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
    for (auto* call : {&g_leaderRead, &g_sensing, &g_takeAbility})
    {
        if (call->IsInstalled() && !call->Restore())
        {
            platform::Log(L"player hits: a redirected call was changed after us; left in place (it passes through)");
        }
    }
}

Human HumanInCall() noexcept
{
    return t_human;
}

}  // namespace lss::player_two::player_hits
